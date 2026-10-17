#include "sprite_gpu_structs.h"
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <simd/simd.h>
#include <sstream>
#include <vector>
using V4 = simd_float4;
static constexpr int SpritePBR = 128, SpriteReflections = 256, HDREmissive = 64;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;
static id<MTLRenderPipelineState> defaultPipeline;
static id<MTLRenderPipelineState> gbufferPipeline;
static id<MTLSamplerState> sampler;
struct InputImage {
  int width = 0, height = 0;
  std::vector<V4> pixels;
};
struct SpriteAssets {
  InputImage layers[5]; // base, authored normal, metallic, roughness, AO
};
struct Region {
  std::string name;
  float x0, y0, x1, y1;
};

static InputImage readPNG(const std::string &filename) {
  NSURL *url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:filename.c_str()]];
  CGImageSourceRef source =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  if (!source)
    throw std::runtime_error("Cannot read PNG: " + filename);
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!image)
    throw std::runtime_error("Cannot decode PNG: " + filename);
  InputImage result;
  result.width = int(CGImageGetWidth(image));
  result.height = int(CGImageGetHeight(image));
  std::vector<uint8_t> bytes(result.width * result.height * 4);
  // Match the source RGB space to preserve numeric normal/scalar map values
  // even if the PNG carries a profile. Expand indexed/grayscale PNGs to RGB.
  // Unpremultiply after decoding to preserve source RGB/alpha.
  CGColorSpaceRef sourceSpace = CGImageGetColorSpace(image);
  CGColorSpaceRef space =
      sourceSpace && CGColorSpaceGetModel(sourceSpace) == kCGColorSpaceModelRGB
          ? CGColorSpaceRetain(sourceSpace)
          : CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(
      bytes.data(), result.width, result.height, 8, result.width * 4, space,
      kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  if (!context)
    throw std::runtime_error("Cannot allocate PNG decode context");
  CGContextSetBlendMode(context, kCGBlendModeCopy);
  CGContextDrawImage(context, CGRectMake(0, 0, result.width, result.height),
                     image);
  CGContextRelease(context);
  CGColorSpaceRelease(space);
  CGImageRelease(image);
  result.pixels.resize(result.width * result.height);
  for (size_t i = 0; i < result.pixels.size(); i++) {
    float a = bytes[i * 4 + 3] / 255.f;
    result.pixels[i] = {0, 0, 0, a};
    if (a > 0)
      for (int k = 0; k < 3; k++)
        result.pixels[i][k] = std::min(bytes[i * 4 + k] / (255.f * a), 1.f);
  }
  return result;
}
static id<MTLTexture> texture(int width, int height,
                              const std::vector<V4> &pixels, int slices = 0) {
  auto desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:width
                                  height:height
                               mipmapped:NO];
  desc.storageMode = MTLStorageModeShared;
  desc.usage = MTLTextureUsageShaderRead;
  if (slices) {
    desc.textureType = MTLTextureType2DArray;
    desc.arrayLength = slices;
  }
  auto result = [device newTextureWithDescriptor:desc];
  for (int s = 0; s < std::max(slices, 1); s++)
    [result replaceRegion:MTLRegionMake2D(0, 0, width, height)
              mipmapLevel:0
                    slice:s
                withBytes:pixels.data() + s * width * height
              bytesPerRow:width * 16
            bytesPerImage:width * height * 16];
  return result;
}
struct Params {
  float metal = 0, rough = .8, ambient = .15, lightX = 0;
  float lightY = 0, lightIntensity = 1;
  float normalStrength = 1, marker = -1;
  float lightStrength = 1;
  V4 lightColor = {1, 1, 1, 0};
  V4 dynColor = {0, 0, 0, 0};
  V4 normal = {128.f / 255, 128.f / 255, 1, 1};
  int debug = 0, light = 1, mirror = 0, features = SpritePBR;
  int size = 32;
  int attachment = 0;
  float gbufferMode = 0, reactive = 0;
  bool reflection = false, sphere = false, defaultMaterial = false;
  bool gbuffer = false;
  bool geometricNormal = true;
  const SpriteAssets *assets = nullptr;
};
static std::vector<V4> renderImage(Params p) {
  const int size = p.size;
  auto desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                   width:size
                                  height:size
                               mipmapped:NO];
  desc.storageMode = MTLStorageModeShared;
  desc.usage = MTLTextureUsageRenderTarget;
  auto target = [device newTextureWithDescriptor:desc];
  id<MTLTexture> targets[3] = {target, nil, nil};
  if (p.gbuffer) {
    targets[1] = [device newTextureWithDescriptor:desc];
    targets[2] = [device newTextureWithDescriptor:desc];
  }
  ViewpointUBO vp = {};
  vp.ProjectionMatrix = vp.ViewMatrix = vp.NormalViewMatrix =
      matrix_identity_float4x4;
  vp.uCameraPos = {0, 0, 10, 1};
  vp.uRenderFeatures = p.features;
  vp.uGIStrength = vp.uGISaturation = 1;
  vp.uGIOrigin = {0, 0, 0, 1};
  vp.uGISize = {1, 1, 1, 0};
  vp.uReflectionOrigin = {0, 0, 0, 1};
  vp.uReflectionSize = {1, 1, 1, 6};
  vp.uReflectionAtlas = {1, 1, 16, p.reflection ? 1.f : 0.f};
  StreamData stream = {};
  stream.uObjectColor = {1, 1, 1, 1};
  stream.uDynLightColor = p.dynColor;
  stream.uMaterialParams = {p.normalStrength, (float)p.debug, p.lightStrength, p.marker};
  stream.uMaterialParams2.w = p.gbufferMode;
  stream.uSurfaceParams.x = p.reactive;
  PushConstants pc = {};
  pc.uLightLevel = -1;
  pc.uLightIndex = p.light ? 0 : -1;
  pc.uAlphaThreshold = .01;
  V4 lights[5] = {
      {0, 4, 4, 4}, {0, 0, 5, 32}, {1, 1, 1, 1025}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  lights[1].x = p.lightX;
  lights[1].y = p.lightY;
  lights[2].xyz = (p.lightColor * p.lightIntensity).xyz;
  if (p.light == 2)
    lights[0] = {0, 0, 4, 4};
  if (p.light == 3)
    lights[0] = {0, 0, 0, 4};
  id<MTLTexture> maps[17] = {};
  maps[0] = texture(1, 1, {{.5, .5, .5, 1}});
  maps[1] = texture(1, 1, {p.normal});
  maps[2] = texture(1, 1, {{p.metal, p.metal, p.metal, 1}});
  maps[3] = texture(1, 1, {{p.rough, p.rough, p.rough, 1}});
  maps[4] = texture(1, 1, {{1, 1, 1, 1}});
  if (p.sphere) {
    // Analytic synthetic test data only. This is not an asset-generation path
    // and does not add, replace, or infer maps for any game sprite.
    std::vector<V4> base(size * size), normal(size * size), metal(size * size),
        rough(size * size), ao(size * size);
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        float nx = ((x + .5f) / size * 2 - 1) / .85f;
        float ny = (1 - (y + .5f) / size * 2) / .85f;
        float r2 = nx * nx + ny * ny;
        size_t index = y * size + x;
        bool inside = r2 < 1;
        base[index] = {.55, .55, .55, inside ? 1.f : 0.f};
        auto encode = [](float n) { return (127 * n + 128) / 255; };
        normal[index] =
            inside ? V4{encode(nx), encode(ny), encode(std::sqrt(1 - r2)), 1}
                   : V4{128.f / 255, 128.f / 255, 1, 1};
        float m = x < size / 2 ? 1 : 0;
        float r = y < size / 2 ? .18 : .82;
        float a = 1 - .25f * std::pow(std::min(r2, 1.f), 4);
        metal[index] = {m, m, m, 1};
        rough[index] = {r, r, r, 1};
        ao[index] = {a, a, a, 1};
      }
    maps[0] = texture(size, size, base);
    maps[1] = texture(size, size, normal);
    maps[2] = texture(size, size, metal);
    maps[3] = texture(size, size, rough);
    maps[4] = texture(size, size, ao);
  }
  if (p.assets)
    for (int i = 0; i < 5; i++) {
      const auto &layer = p.assets->layers[i];
      maps[i] = texture(layer.width, layer.height, layer.pixels);
    }
  for (int i = 5; i < 13; i++)
    maps[i] = texture(1, 1, {{0, 0, 0, 1}});
  maps[13] = texture(1, 1, {{0, 0, 0, 1}}, 1);
  std::vector<V4> irradiance(36 * 36 * 7, V4{0, 0, 0, 1});
  for (int f = 0; f < 6; f++) {
    V4 c = f == 0 ? V4{1, .1, .1, 1}
                  : f == 1 ? V4{.1, .1, 1, 1} : V4{.1, .1, .1, 1};
    for (int y = 0; y < 36; y++)
      for (int x = 0; x < 36; x++)
        irradiance[(f * 36 + y) * 36 + x] = c;
  }
  V4 reflection[4] = {
      {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {.2, .2, .2, 1}};
  for (int y = 0; y < 36; y++)
    for (int x = 0; x < 36; x++)
      irradiance[(6 * 36 + y) * 36 + x] = reflection[(y / 18) * 2 + x / 18];
  maps[14] = texture(36, 36, irradiance, 7);
  maps[15] = texture(1, 1, {{0, 0, 0, 0}}, 1);
  maps[16] = texture(1, 1, {{1000, 0, 0, 0}});
  auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
  for (int i = 0; i < (p.gbuffer ? 3 : 1); i++) {
    pass.colorAttachments[i].texture = targets[i];
    pass.colorAttachments[i].loadAction = MTLLoadActionClear;
    pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[i].storeAction = MTLStoreActionStore;
  }
  auto command = [queue commandBuffer];
  auto encoder = [command renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:p.gbuffer
                                      ? gbufferPipeline
                                      : p.defaultMaterial ? defaultPipeline
                                                          : pipeline];
  [encoder setFragmentBytes:&vp length:sizeof(vp) atIndex:0];
  auto streamBuffer = [device newBufferWithLength:sizeof(StreamData) * 1024
                                          options:MTLResourceStorageModeShared];
  memcpy(streamBuffer.contents, &stream, sizeof(stream));
  [encoder setFragmentBuffer:streamBuffer offset:0 atIndex:2];
  [encoder setFragmentBytes:lights length:sizeof(lights) atIndex:3];
  [encoder setFragmentBytes:&pc length:sizeof(pc) atIndex:5];
  V4 params = {(float)p.mirror, p.ambient, p.geometricNormal ? 0.f : 1.f, 0};
  [encoder setVertexBytes:&params length:sizeof(params) atIndex:0];
  V4 geometry = {1, 1, 0, 0};
  if (p.assets) {
    float maximum =
        std::max(p.assets->layers[0].width, p.assets->layers[0].height);
    geometry.x = p.assets->layers[0].width / maximum;
    geometry.y = p.assets->layers[0].height / maximum;
  }
  [encoder setVertexBytes:&geometry length:sizeof(geometry) atIndex:1];
  for (int i = 0; i < 17; i++) {
    [encoder setFragmentTexture:maps[i] atIndex:i];
    if (i < 15)
      [encoder setFragmentSamplerState:sampler atIndex:i];
  }
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
  [encoder endEncoding];
  [command commit];
  [command waitUntilCompleted];
  if (command.status == MTLCommandBufferStatusError)
    throw std::runtime_error(command.error.localizedDescription.UTF8String);
  std::vector<V4> result(size * size);
  [targets[p.attachment] getBytes:result.data()
                      bytesPerRow:size * 16
                       fromRegion:MTLRegionMake2D(0, 0, size, size)
                      mipmapLevel:0];
  for (auto c : result)
    for (int k = 0; k < 4; k++)
      if (!std::isfinite(c[k]))
        throw std::runtime_error("non-finite framebuffer component");
  return result;
}
static V4 render(Params p) {
  auto result = renderImage(p);
  return result[(p.size / 2) * p.size + p.size / 2];
}
static int checks = 0;
static void check(bool condition, const char *name) {
  checks++;
  if (!condition)
    throw std::runtime_error(name);
  std::cout << "PASS " << name << "\n";
}

static float luminance(V4 pixel) {
  return pixel.x * .2126f + pixel.y * .7152f + pixel.z * .0722f;
}

static float patchMean(const std::vector<V4> &pixels, int size, int cx, int cy,
                       int radius = 3) {
  float total = 0;
  for (int y = cy - radius; y <= cy + radius; ++y)
    for (int x = cx - radius; x <= cx + radius; ++x)
      total += luminance(pixels[y * size + x]);
  return total / ((radius * 2 + 1) * (radius * 2 + 1));
}

static void writePNG(const std::string &filename, const std::vector<V4> &pixels,
                     int size, bool transparent = false,
                     bool checkerboard = true) {
  std::vector<uint8_t> bytes(size * size * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      auto pixel = pixels[y * size + x];
      float checker =
          checkerboard ? (((x / 16 + y / 16) & 1) ? .13f : .09f) : .18f;
      for (int k = 0; k < 3; ++k)
        bytes[(y * size + x) * 4 + k] = uint8_t(std::lround(
            std::clamp(pixel[k] * pixel.w +
                           (transparent ? 0 : checker * (1 - pixel.w)),
                       0.f, 1.f) *
            255));
      bytes[(y * size + x) * 4 + 3] =
          transparent
              ? uint8_t(std::lround(std::clamp(pixel.w, 0.f, 1.f) * 255))
              : 255;
    }
  CFDataRef data = CFDataCreate(nullptr, bytes.data(), bytes.size());
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGImageRef image =
      CGImageCreate(size, size, 8, 32, size * 4, space,
                    kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                    provider, nullptr, false, kCGRenderingIntentDefault);
  NSURL *url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:filename.c_str()]];
  CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  if (!image || !destination)
    throw std::runtime_error("Cannot create fixture PNG");
  CGImageDestinationAddImage(destination, image, nullptr);
  bool ok = CGImageDestinationFinalize(destination);
  CFRelease(destination);
  CGImageRelease(image);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(provider);
  CFRelease(data);
  if (!ok)
    throw std::runtime_error("Cannot save fixture PNG");
}

static void validateSpriteAssets(const std::string &spritePath,
                                 const std::string &materialsDirectory,
                                 const std::string &previewDirectory,
                                 const std::vector<Region> &regions) {
  if (materialsDirectory.empty() || previewDirectory.empty())
    throw std::runtime_error(
        "Sprite previews require --materials-dir and --preview-dir");
  NSString *spriteName =
      [[NSString stringWithUTF8String:spritePath.c_str()] lastPathComponent];
  std::string basename = spriteName.UTF8String;
  std::string stem = [spriteName stringByDeletingPathExtension].UTF8String;
  SpriteAssets assets;
  assets.layers[0] = readPNG(spritePath);
  const char *folders[] = {"normalmaps", "metallic", "roughness", "ao"};
  for (int i = 1; i < 5; i++) {
    assets.layers[i] = readPNG(materialsDirectory + "/" + folders[i - 1] +
                               "/auto/" + basename);
    if (assets.layers[i].width != assets.layers[0].width ||
        assets.layers[i].height != assets.layers[0].height)
      throw std::runtime_error(
          "Authored map dimensions differ from the source sprite");
  }
  NSError *error = nil;
  if (![[NSFileManager defaultManager]
                createDirectoryAtPath:[NSString
                                          stringWithUTF8String:previewDirectory
                                                                   .c_str()]
          withIntermediateDirectories:YES
                           attributes:nil
                                error:&error])
    throw std::runtime_error(error.localizedDescription.UTF8String);
  Params p;
  p.assets = &assets;
  p.size = 768;
  p.ambient = .45;
  p.lightIntensity = 4;
  p.lightY = 1.5;
  p.lightX = -3.5;
  auto left = renderImage(p);
  p.lightX = 3.5;
  auto right = renderImage(p);
  p.normalStrength = 0;
  auto flatRight = renderImage(p);
  p.normalStrength = 1;
  p.debug = 1;
  auto normals = renderImage(p);
  p.debug = 2;
  auto roughness = renderImage(p);
  p.debug = 3;
  auto metallic = renderImage(p);
  p.debug = 4;
  auto ao = renderImage(p);
  Params redParams = p;
  redParams.debug = 0;
  redParams.ambient = .55;
  redParams.lightIntensity = 1;
  redParams.lightColor = {1, .025, .01, 0};
  redParams.lightStrength = 1;
  auto redOriginal = renderImage(redParams);
  redParams.lightStrength = 3;
  auto redBoosted = renderImage(redParams);
  float maximum = std::max(assets.layers[0].width, assets.layers[0].height);
  std::vector<V4> classicLeft, classicRight;
  if (defaultPipeline) {
    // Match HWDrawInfo::GetDynSpriteLight for this single unshadowed point
    // light: accumulate RGB at the actor center, then let the actual default
    // shader apply its normal clamp. Equal-distance left/right lights must
    // yield an identical color shade over the whole unmapped sprite.
    Params legacy = p;
    legacy.defaultMaterial = true;
    legacy.features = 0;
    legacy.marker = 0;
    legacy.geometricNormal = false;
    legacy.light = 0;
    legacy.debug = 0;
    float radius = 32, lightZ = 5;
    auto centerLight = [&](float lightX) {
      float distance =
          std::sqrt(lightX * lightX + p.lightY * p.lightY + lightZ * lightZ);
      float value = p.lightIntensity * std::max(1 - distance / radius, 0.f);
      return V4{value, value, value, 0};
    };
    legacy.dynColor = centerLight(-3.5);
    classicLeft = renderImage(legacy);
    legacy.dynColor = centerLight(3.5);
    classicRight = renderImage(legacy);
    check(std::memcmp(classicLeft.data(), classicRight.data(),
                      classicLeft.size() * sizeof(V4)) == 0,
          "legacy default sprite fixture is byte-identical under "
          "equal-distance left/right point lights");
  }
  float sx = assets.layers[0].width / maximum,
        sy = assets.layers[0].height / maximum;
  bool alphaMatches = true, outsideClear = true;
  double normalDifference = 0, movedDifference = 0;
  double originalRed = 0, originalGreen = 0, boostedRed = 0, boostedGreen = 0;
  size_t covered = 0;
  for (int y = 0; y < p.size; y++)
    for (int x = 0; x < p.size; x++) {
      size_t index = y * p.size + x;
      float u = (((x + .5f) / p.size * 2 - 1) / sx + 1) * .5f;
      float v = (((y + .5f) / p.size * 2 - 1) / sy + 1) * .5f;
      float alpha = 0;
      if (u >= 0 && u < 1 && v >= 0 && v < 1)
        alpha = assets.layers[0]
                    .pixels[int(v * assets.layers[0].height) *
                                assets.layers[0].width +
                            int(u * assets.layers[0].width)]
                    .w;
      if (alpha <= .01)
        alpha = 0;
      alphaMatches &= std::abs(right[index].w - alpha) < .0001 &&
                      right[index].w == left[index].w &&
                      right[index].w == flatRight[index].w;
      if (alpha > .5) {
        covered++;
        originalRed += redOriginal[index].x;
        originalGreen += redOriginal[index].y;
        boostedRed += redBoosted[index].x;
        boostedGreen += redBoosted[index].y;
        alphaMatches &= redOriginal[index].w == right[index].w &&
                        redBoosted[index].w == right[index].w;
        normalDifference +=
            std::abs(luminance(right[index]) - luminance(flatRight[index]));
        movedDifference +=
            std::abs(luminance(right[index]) - luminance(left[index]));
      } else if (alpha == 0) {
        for (int c = 0; c < 4; c++)
          outsideClear &= right[index][c] == 0 && left[index][c] == 0;
      }
    }
  check(covered > 0 && alphaMatches && outsideClear,
        "real sprite preview preserves the source alpha silhouette and clear "
        "exterior");
  check(normalDifference / covered > .0001,
        "authored sprite normal map changes lighting relative to flat-normal "
        "PBR");
  check(movedDifference / covered > .0001,
        "actual sprite responds to the moving test light");
  check(boostedRed / boostedGreen > originalRed / originalGreen * 1.15,
        "3x direct lighting makes the actual sprite's red-light cast clearer");
  NSMutableArray *measurements = [NSMutableArray array];
  for (const auto &region : regions) {
    double count = 0, m = 0, r = 0, meanDelta = 0;
    float peakDelta = 0;
    double momentLeft = 0, momentRight = 0, energyLeft = 0, energyRight = 0;
    for (int y = 0; y < p.size; y++)
      for (int x = 0; x < p.size; x++) {
        float u = (((x + .5f) / p.size * 2 - 1) / sx + 1) * .5f;
        float v = (((y + .5f) / p.size * 2 - 1) / sy + 1) * .5f;
        size_t i = y * p.size + x;
        if (u < region.x0 || u >= region.x1 || v < region.y0 ||
            v >= region.y1 || right[i].w <= .5)
          continue;
        count++;
        m += metallic[i].x;
        r += roughness[i].x;
        float delta = std::abs(luminance(right[i]) - luminance(left[i]));
        peakDelta = std::max(peakDelta, delta);
        meanDelta += delta;
        double el = std::pow(std::max(luminance(left[i]), 0.f), 4);
        double er = std::pow(std::max(luminance(right[i]), 0.f), 4);
        energyLeft += el;
        energyRight += er;
        momentLeft += x * el;
        momentRight += x * er;
      }
    if (count == 0)
      throw std::runtime_error("No covered pixels in region: " + region.name);
    std::string name =
        "authored " + region.name + " region responds to moving light";
    check(peakDelta > .001, name.c_str());
    double leftX = energyLeft > 0 ? momentLeft / energyLeft : 0;
    double rightX = energyRight > 0 ? momentRight / energyRight : 0;
    std::cout << region.name << ": metallic " << m / count << ", roughness "
              << r / count << ", mean light delta " << meanDelta / count
              << ", peak delta " << peakDelta << ", brightness centroid X "
              << leftX << " -> " << rightX << "\n";
    [measurements addObject:@{
      @"region" : [NSString stringWithUTF8String:region.name.c_str()],
      @"mean_metallic" : @(m / count),
      @"mean_roughness" : @(r / count),
      @"mean_light_delta" : @(meanDelta / count),
      @"peak_light_delta" : @(peakDelta),
      @"brightness_centroid_left_x" : @(leftX),
      @"brightness_centroid_right_x" : @(rightX)
    }];
  }
  std::string prefix = previewDirectory + "/" + stem;
  writePNG(prefix + "-authored-pbr-light-left.png", left, p.size, false, false);
  writePNG(prefix + "-authored-pbr-light-right.png", right, p.size, false,
           false);
  writePNG(prefix + "-flat-normal-pbr-light-right.png", flatRight, p.size,
           false, false);
  writePNG(prefix + "-authored-pbr-transparent.png", right, p.size, true);
  writePNG(prefix + "-world-normals.png", normals, p.size, false, false);
  writePNG(prefix + "-roughness.png", roughness, p.size, false, false);
  writePNG(prefix + "-metallic.png", metallic, p.size, false, false);
  writePNG(prefix + "-ao.png", ao, p.size, false, false);
  writePNG(prefix + "-red-light-strength-1.png", redOriginal, p.size, false, false);
  writePNG(prefix + "-red-light-strength-3.png", redBoosted, p.size, false, false);
  if (!classicLeft.empty()) {
    writePNG(prefix + "-legacy-default-light-left.png", classicLeft, p.size,
             false, false);
    writePNG(prefix + "-legacy-default-light-right.png", classicRight, p.size,
             false, false);
  }
  NSDictionary *report = @{
    @"sprite" : [NSString stringWithUTF8String:spritePath.c_str()],
    @"materials" : [NSString stringWithUTF8String:materialsDirectory.c_str()],
    @"preview_size" : @(p.size),
    @"mean_normal_response" : @(normalDifference / covered),
    @"mean_moving_light_response" : @(movedDifference / covered),
    @"source_alpha_preserved" : @YES,
    @"red_light_strength_1_mean_rgb_rg" : @[@(originalRed / covered), @(originalGreen / covered)],
    @"red_light_strength_3_mean_rgb_rg" : @[@(boostedRed / covered), @(boostedGreen / covered)],
    @"regions" : measurements
  };
  NSData *json =
      [NSJSONSerialization dataWithJSONObject:report
                                      options:NSJSONWritingPrettyPrinted
                                        error:&error];
  if (!json ||
      ![json writeToFile:[NSString stringWithUTF8String:(prefix +
                                                         "-measurements.json")
                                                            .c_str()]
                 options:NSDataWritingAtomic
                   error:&error])
    throw std::runtime_error("Cannot write sprite measurements");
  std::ofstream note(prefix + "-README.txt");
  note
      << "Actual authored sprite maps rendered through the engine's "
         "translated PBR fragment shader.\n"
      << "This is an offscreen material preview, not a screenshot of the game "
         "level.\n"
      << "Source sprite: " << spritePath
      << "\nMaterials: " << materialsDirectory << "\n"
      << "768px, nearest sampling, neutral gray background. Transparent PNG "
         "also supplied.\n"
      << "White point light: X -3.5 or +3.5, Y 1.5, Z 5, radius 32, intensity "
         "4. Ambient 0.45.\n"
      << "Red-light comparison: RGB (1, 0.025, 0.01), intensity 1, ambient 0.55, "
         "sprite direct-light strength 1 versus 3.\n"
      << "The flat-normal comparison keeps the same PBR maps and light; it is "
         "NOT classic engine rendering.\n"
      << "Legacy-default images use the actual default shader, no maps/marker, "
         "and the center-sampled light accumulation from GetDynSpriteLight.\n"
      << "The legacy fixture uses the same source/light/ambient with standard "
         "light clamping; symmetric lights produce the same uniform shade.\n"
      << "Authored data is loaded directly. No normal or material maps are "
         "inferred or generated here.\n"
      << "PNG output clamps HDR highlights to SDR. Region coordinates are "
         "explicit preview test inputs.\n"
      << "Brightness centroids measure the full shaded response, not isolated "
         "specular energy.\n";
  std::cout << "Actual sprite material previews: " << prefix << "\n";
}

static void validateSpatialMaps(const std::string &previewDirectory) {
  Params p;
  p.sphere = true;
  p.size = 512;
  const int size = p.size, mid = size / 2;
  p.debug = 1;
  auto normals = renderImage(p);
  check(normals[mid * size + size / 3].x < .4 &&
            normals[mid * size + size * 2 / 3].x > .6 &&
            normals[(size / 3) * size + mid].y > .6 &&
            normals[(size * 2 / 3) * size + mid].y < .4,
        "spatial normal texture varies in both tangent axes");
  p.mirror = 1;
  auto mirrored = renderImage(p);
  float mirrorDifference = 0;
  for (size_t i = 0; i < normals.size(); ++i)
    for (int c = 0; c < 3; ++c)
      mirrorDifference =
          std::max(mirrorDifference, std::abs(normals[i][c] - mirrored[i][c]));
  check(mirrorDifference < .0001,
        "mirrored UVs preserve world normals of symmetric spatial sphere map");
  p.mirror = 0;
  p.debug = 2;
  auto roughness = renderImage(p);
  check(std::abs(patchMean(roughness, size, mid - 60, mid - 60) - .18) < .001 &&
            std::abs(patchMean(roughness, size, mid + 60, mid + 60) - .82) <
                .001,
        "spatial roughness map preserves smooth top and rough bottom regions");
  p.debug = 3;
  auto metallic = renderImage(p);
  check(patchMean(metallic, size, mid - 60, mid - 60) > .999 &&
            patchMean(metallic, size, mid + 60, mid + 60) < .001,
        "spatial metallic map separates metal left and dielectric right");
  p.debug = 4;
  auto ao = renderImage(p);
  check(patchMean(ao, size, mid, mid) > .999 &&
            patchMean(ao, size, mid - 195, mid) < .91,
        "spatial ambient-occlusion map samples edge and center distinctly");
  p.debug = 0;
  auto headOn = renderImage(p);
  float smoothMetal = patchMean(headOn, size, mid - 12, mid - 12);
  float smoothDielectric = patchMean(headOn, size, mid + 11, mid - 12);
  float roughMetal = patchMean(headOn, size, mid - 12, mid + 11);
  float roughDielectric = patchMean(headOn, size, mid + 11, mid + 11);
  check(smoothMetal > smoothDielectric * 1.3 &&
            roughDielectric > roughMetal * 1.2,
        "quadrant materials change specular and diffuse energy independently");
  check(smoothMetal > roughMetal * 2,
        "spatial roughness changes nearby metal highlight intensity");
  p.lightX = 3;
  auto right = renderImage(p);
  p.lightX = -3;
  auto left = renderImage(p);
  auto highlightX = [size](const std::vector<V4> &pixels) {
    double weight = 0, moment = 0;
    for (int y = 0; y < size / 2; ++y)
      for (int x = 0; x < size; ++x) {
        double excess =
            std::max(double(luminance(pixels[y * size + x])) - .4, 0.0);
        double w = excess * excess;
        weight += w;
        moment += x * w;
      }
    return weight > 0 ? moment / weight : -1;
  };
  check(highlightX(right) - highlightX(left) > 60,
        "highlight moves across spatial material maps with the light");
  p.lightX = 3;
  p.normalStrength = 0;
  auto flat = renderImage(p);
  double difference = 0, covered = 0, changedPixels = 0;
  float maximumDifference = 0;
  bool clearOutside = true, alphaMatches = true;
  for (size_t i = 0; i < right.size(); ++i) {
    alphaMatches &= right[i].w == flat[i].w;
    if (right[i].w > .5) {
      float delta = std::abs(luminance(right[i]) - luminance(flat[i]));
      difference += delta;
      maximumDifference = std::max(maximumDifference, delta);
      changedPixels += delta > .01;
      covered++;
    } else {
      for (int c = 0; c < 4; ++c)
        clearOutside &= right[i][c] == 0 && flat[i][c] == 0;
    }
  }
  std::cout << "Spatial normal response: mean luminance difference "
            << difference / covered << ", maximum " << maximumDifference
            << ", fraction changing by >0.01 " << changedPixels / covered
            << "\n";
  // Glossy highlights occupy a small area, so a whole-image mean alone is a
  // poor shape test. Require both a strong local response and broad change.
  check(maximumDifference > .15 && changedPixels / covered > .1 &&
            difference / covered > .005,
        "spatial authored normals materially change PBR lighting versus flat "
        "normals");
  check(clearOutside && alphaMatches && covered > size * size * .56 &&
            covered < size * size * .575,
        "sphere silhouette remains transparent and finite for both normal "
        "strengths");
  if (!previewDirectory.empty()) {
    NSError *error = nil;
    if (![[NSFileManager defaultManager]
                  createDirectoryAtPath:
                      [NSString stringWithUTF8String:previewDirectory.c_str()]
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error])
      throw std::runtime_error(error.localizedDescription.UTF8String);
    writePNG(previewDirectory + "/synthetic-normal-pbr-light-left.png", left,
             size);
    writePNG(previewDirectory + "/synthetic-normal-pbr-light-right.png", right,
             size);
    writePNG(previewDirectory + "/synthetic-flat-normal-pbr-light-right.png",
             flat, size);
    writePNG(previewDirectory + "/synthetic-world-normals.png", normals, size);
    writePNG(previewDirectory + "/synthetic-roughness-map.png", roughness,
             size);
    writePNG(previewDirectory + "/synthetic-metallic-map.png", metallic, size);
    writePNG(previewDirectory + "/synthetic-ao-map.png", ao, size);
    std::ofstream note(previewDirectory + "/README.txt");
    note << "Synthetic material fixture only; not a game sprite or production "
            "map generator.\n"
            "All images use the actual translated sprite PBR fragment shader "
            "at 512x512.\n"
            "Left half: metal. Right half: dielectric. Top: roughness 0.18. "
            "Bottom: roughness 0.82.\n"
            "Flat-normal PBR uses normal strength 0; it is NOT the "
            "original/classic renderer.\n"
            "Shape-aware PBR uses normal strength 1. Moving-light images "
            "change only light X.\n"
            "PNG previews clamp HDR highlights to SDR and composite "
            "transparent pixels over a checkerboard.\n"
            "Assertions inspect unclamped floating-point pixels, including the "
            "transparent silhouette.\n";
    std::cout << "Synthetic preview PNGs: " << previewDirectory << "\n";
  }
}

int main(int argc, char **argv) {
  @autoreleasepool {
    try {
      if (argc < 2)
        throw std::runtime_error(
            "Usage: sprite_gpu_check <PBR fragment.metal> [--default-shader "
            "file] [--gbuffer-shader file] [--preview-dir directory]");
      std::string defaultShader, gbufferShader, previewDirectory, spritePath,
          materialsDirectory;
      std::vector<Region> regions;
      for (int i = 2; i < argc; ++i) {
        std::string option = argv[i];
        if (++i >= argc)
          throw std::runtime_error("Missing option value");
        if (option == "--default-shader")
          defaultShader = argv[i];
        else if (option == "--gbuffer-shader")
          gbufferShader = argv[i];
        else if (option == "--preview-dir")
          previewDirectory = argv[i];
        else if (option == "--sprite-path")
          spritePath = argv[i];
        else if (option == "--materials-dir")
          materialsDirectory = argv[i];
        else if (option == "--region") {
          std::string value = argv[i];
          std::replace(value.begin(), value.end(), ',', ' ');
          std::istringstream input(value);
          Region region;
          if (!(input >> region.name >> region.x0 >> region.y0 >> region.x1 >>
                region.y1) ||
              region.x0 < 0 || region.y0 < 0 || region.x1 > 1 ||
              region.y1 > 1 || region.x0 >= region.x1 || region.y0 >= region.y1)
            throw std::runtime_error("Region requires name,x0,y0,x1,y1 in "
                                     "normalized sprite coordinates");
          regions.push_back(region);
        } else
          throw std::runtime_error("Unknown option: " + option);
      }
      device = MTLCreateSystemDefaultDevice();
      if (!device)
        throw std::runtime_error("No Metal device");
      queue = [device newCommandQueue];
      NSError *error = nil;
      NSString *source = [NSString
          stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]
                          encoding:NSUTF8StringEncoding
                             error:&error];
      NSString *vertex = @R"(#include <metal_stdlib>
using namespace metal;
struct Out {float4 position [[position]];float4 uv [[user(locn0)]];float4 color [[user(locn1)]];float4 point [[user(locn2)]];float3 glow [[user(locn3)]];float3 gradient [[user(locn4)]];float4 normal [[user(locn5)]];float4 eyeNormal [[user(locn6)]];float4 clipA [[user(locn7)]];float4 clipB [[user(locn8)]];float3 lightmap [[user(locn9)]];float4 volume0 [[user(locn10), flat]];float4 volume1 [[user(locn11), flat]];float4 volume2 [[user(locn12), flat]];float4 volumeParams [[user(locn13), flat]];};
vertex Out test_vertex(uint id [[vertex_id]],constant float4&params [[buffer(0)]],constant float4&geometry [[buffer(1)]]) {float2 coords[6]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,-1),float2(1,1),float2(-1,1)};float2 p=coords[id];Out o;o.position=float4(p*geometry.xy,0,1);o.point=float4(p*geometry.xy,0,5);float2 uv=float2(p.x*.5+.5,.5-p.y*.5);if(params.x>.5)uv.x=1-uv.x;o.uv=float4(uv,0,0);o.color=float4(float3(params.y),1);o.normal=params.z>.5?float4(0):float4(0,0,1,0);o.eyeNormal=o.normal;o.glow=o.gradient=float3(0);o.clipA=o.clipB=float4(1);o.lightmap=float3(0,0,-1);o.volume0=o.volume1=o.volume2=o.volumeParams=float4(0);return o;})";
      MTLCompileOptions *options = [MTLCompileOptions new];
      options.languageVersion = MTLLanguageVersion2_4;
      auto lib = [device newLibraryWithSource:source
                                      options:options
                                        error:&error];
      if (!lib)
        throw std::runtime_error(error.localizedDescription.UTF8String);
      auto vlib = [device newLibraryWithSource:vertex
                                       options:options
                                         error:&error];
      if (!vlib)
        throw std::runtime_error(error.localizedDescription.UTF8String);
      auto desc = [MTLRenderPipelineDescriptor new];
      desc.vertexFunction = [vlib newFunctionWithName:@"test_vertex"];
      desc.fragmentFunction = [lib newFunctionWithName:@"main0"];
      desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
      pipeline = [device newRenderPipelineStateWithDescriptor:desc
                                                        error:&error];
      if (!pipeline)
        throw std::runtime_error(error.localizedDescription.UTF8String);
      if (!defaultShader.empty()) {
        NSString *defaultSource = [NSString
            stringWithContentsOfFile:[NSString
                                         stringWithUTF8String:defaultShader
                                                                  .c_str()]
                            encoding:NSUTF8StringEncoding
                               error:&error];
        auto defaultLibrary = [device newLibraryWithSource:defaultSource
                                                   options:options
                                                     error:&error];
        if (!defaultLibrary)
          throw std::runtime_error(error.localizedDescription.UTF8String);
        desc.fragmentFunction = [defaultLibrary newFunctionWithName:@"main0"];
        defaultPipeline = [device newRenderPipelineStateWithDescriptor:desc
                                                                 error:&error];
        if (!defaultPipeline)
          throw std::runtime_error(error.localizedDescription.UTF8String);
      }
      if (!gbufferShader.empty()) {
        NSString *gbufferSource = [NSString
            stringWithContentsOfFile:[NSString
                                         stringWithUTF8String:gbufferShader
                                                                  .c_str()]
                            encoding:NSUTF8StringEncoding
                               error:&error];
        auto gbufferLibrary = [device newLibraryWithSource:gbufferSource
                                                   options:options
                                                     error:&error];
        if (!gbufferLibrary)
          throw std::runtime_error(error.localizedDescription.UTF8String);
        desc.fragmentFunction = [gbufferLibrary newFunctionWithName:@"main0"];
        desc.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Float;
        desc.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA32Float;
        gbufferPipeline = [device newRenderPipelineStateWithDescriptor:desc
                                                                 error:&error];
        if (!gbufferPipeline)
          throw std::runtime_error(error.localizedDescription.UTF8String);
      }
      auto samp = [MTLSamplerDescriptor new];
      samp.minFilter = samp.magFilter = MTLSamplerMinMagFilterNearest;
      samp.sAddressMode = samp.tAddressMode = MTLSamplerAddressModeClampToEdge;
      sampler = [device newSamplerStateWithDescriptor:samp];
      if (!spritePath.empty()) {
        validateSpriteAssets(spritePath, materialsDirectory, previewDirectory,
                             regions);
        std::cout << checks << " asset GPU assertions passed on "
                  << device.name.UTF8String << "\n";
        return 0;
      }
      Params p;
      p.debug = 1;
      p.light = 0;
      p.normal = {.8, 128.f / 255, .9, 1};
      auto a = render(p);
      p.mirror = 1;
      auto b = render(p);
      check(a.x > .7 && b.x < .3 && std::abs(a.z - b.z) < .001,
            "mirrored UV reverses tangent, keeps front normal");
      p.mirror = 0;
      p.normal = {128.f / 255, .8, .9, 1};
      a = render(p);
      check(a.y > .7, "OpenGL green-up normal maps point upward");
      p = {};
      p.metal = 1;
      p.rough = .3;
      p.normal = {.8, 128.f / 255, .9, 1};
      p.lightX = 12;
      a = render(p);
      p.lightX = -12;
      b = render(p);
      check(a.x > b.x * 1.5,
            "direct specular responds to moving-light direction");
      p = {};
      p.metal = 1;
      p.rough = .15;
      a = render(p);
      p.rough = .85;
      b = render(p);
      check(a.x > b.x * 2,
            "metal roughness widens and lowers direct specular peak");
      p = {};
      p.light = 0;
      p.metal = 1;
      a = render(p);
      check(
          a.x > 0 && a.x < 1,
          "unlit metallic material retains finite ambient reflection fallback");
      p = {};
      p.ambient = .6;
      p.light = 0;
      auto baseline = render(p);
      p.light = 1;
      auto modulated = render(p);
      p.light = 2;
      auto subtractive = render(p);
      p.light = 3;
      auto additive = render(p);
      check(modulated.x > baseline.x && subtractive.x < baseline.x,
            "modulated and subtractive light signs");
      check(std::abs(modulated.x - additive.x) < .0001,
            "additive light traversed once through same BRDF");
      p = {};
      p.ambient = .55;
      p.lightColor = {1, .025, .01, 0};
      auto originalRedLight = render(p);
      p.lightStrength = 3;
      auto boostedRedLight = render(p);
      p.lightStrength = 0;
      auto zeroStrength = render(p);
      p.light = 0;
      p.lightStrength = 1;
      auto ambientOnly = render(p);
      check(std::abs(zeroStrength.x - ambientOnly.x) < .0001 &&
            std::abs(zeroStrength.y - ambientOnly.y) < .0001,
            "zero sprite light strength preserves ambient illumination");
      float originalDirect = std::pow(originalRedLight.x, 2.2f) - std::pow(ambientOnly.x, 2.2f);
      float boostedDirect = std::pow(boostedRedLight.x, 2.2f) - std::pow(ambientOnly.x, 2.2f);
      check(std::abs(boostedDirect / originalDirect - 3) < .001,
            "sprite strength scales direct radiance exactly once in linear space");
      check(boostedRedLight.x / boostedRedLight.y > originalRedLight.x / originalRedLight.y * 1.15,
            "red light remains visibly colored against neutral ambient");
      p = {};
      p.light = 0;
      p.features = SpritePBR | 8;
      p.ambient = .4;
      p.normal = {.8, 128.f / 255, .9, 1};
      a = render(p);
      p.mirror = 1;
      b = render(p);
      std::cout << "GI colors " << a.x << "," << a.y << "," << a.z << " vs "
                << b.x << "," << b.y << "," << b.z << "\n";
      check(a.x > a.z && b.z > b.x,
            "per-pixel normals select directional GI colors");
      p = {};
      p.light = 0;
      p.features = SpritePBR | SpriteReflections;
      p.reflection = true;
      p.metal = 1;
      p.ambient = 0;
      p.rough = .12;
      a = render(p);
      p.rough = .82;
      b = render(p);
      check(a.x > a.z && b.z > b.x,
            "roughness selects prefiltered environment radiance levels");
      p = {};
      p.light = 0;
      p.features = SpritePBR | SpriteReflections;
      p.metal = 1;
      a = render(p);
      check(a.x > 0, "inactive probe atlas preserves ambient fallback");
      if (defaultPipeline) {
        p = {};
        p.defaultMaterial = true;
        p.geometricNormal = false;
        p.marker = 0;
        p.light = 0;
        p.features = 0;
        p.dynColor = {.18, .06, .02, 0};
        auto classic = renderImage(p);
        p.features = SpritePBR;
        auto spriteFeature = renderImage(p);
        check(std::memcmp(classic.data(), spriteFeature.data(),
                          classic.size() * sizeof(V4)) == 0,
              "unmapped default shader is byte-identical with sprite PBR "
              "feature off/on");
        p.features = SpritePBR | SpriteReflections;
        auto spriteReflections = renderImage(p);
        check(std::memcmp(classic.data(), spriteReflections.data(),
                          classic.size() * sizeof(V4)) == 0,
              "unmapped default shader stays byte-identical with sprite "
              "reflections enabled");
      }
      p = {};
      p.light = 0;
      p.debug = 1;
      p.features = 0;
      p.normal = {.8, 128.f / 255, .9, 1};
      auto noFeatures = renderImage(p);
      p.features = HDREmissive;
      auto hdrOnly = renderImage(p);
      check(
          std::memcmp(noFeatures.data(), hdrOnly.data(),
                      noFeatures.size() * sizeof(V4)) == 0,
          "HDR emission feature does not activate sprite PBR or debug shading");
      if (gbufferPipeline) {
        p = {};
        p.gbuffer = true;
        p.attachment = 2;
        p.reactive = .7;
        p.normal = {.8, 128.f / 255, .9, 1};
        a = render(p);
        check(a.x > .7 && std::abs(a.w - .7) < .0001,
              "solid sprite G-buffer retains authored normal and TAA reactive "
              "alpha");
        p.attachment = 1;
        a = render(p);
        int packed = int(std::round(255 - a.w * 255));
        check(packed / 8 == 0 && a.w > .5,
              "sprite G-buffer uses current packed format with zero SSR "
              "reflectivity");
        p.gbufferMode = 1;
        p.attachment = 2;
        a = render(p);
        check(a.x > .7 && std::abs(a.w - .7) < .0001,
              "cover-mode sprite retains authored normal and reactive alpha");
        p.gbufferMode = 2;
        a = render(p);
        check(a.x == 0 && a.y == 0 && a.z == 0 && std::abs(a.w - .7) < .0001,
              "keep-mode sprite preserves background normals and raises "
              "reactive alpha");
      }
      validateSpatialMaps(previewDirectory);
      std::cout << checks << " GPU assertions passed on "
                << device.name.UTF8String << "\n";
      return 0;
    } catch (std::exception &e) {
      std::cerr << "FAIL " << e.what() << "\n";
      return 1;
    }
  }
}
