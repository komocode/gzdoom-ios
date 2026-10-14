/*
** mtl_renderdevice.mm
** Native Metal renderer.
**
** The device mirrors the Vulkan backend's structure (and shares its shaders and
** shader interface), so both stay interchangeable for the hardware renderer.
** Command buffers are committed and waited for at the end of every frame.
*/

#include "mtl_renderdevice.h"
// Simulator SDKs have no MetalFX.
#if !TARGET_OS_TV && __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define HAVE_METALFX 1
#endif
#include "mtl_renderstate.h"
#include "mtl_postprocess.h"
#include "mtl_create.h"

#include "v_video.h"
#include "v_draw.h"
#include "v_2ddrawer.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "printf.h"
#include "textures.h"
#include "hw_levelmesh.h"

EXTERN_CVAR(Bool, vk_raytrace)

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

static void *framePool;

MetalRenderDevice::MetalRenderDevice(CAMetalLayer *metalLayer, bool fullscreen) : Super(nullptr, fullscreen), layer(metalLayer)
{
	device = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
	if (!device)
		throw CMetalError("No Metal device");
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	queue = [device newCommandQueue];
#if TARGET_OS_IPHONE
	mUnifiedMemory = true;
#else
	mUnifiedMemory = device.hasUnifiedMemory;
#endif
}

MetalRenderDevice::~MetalRenderDevice()
{
	FlushCommands(true);

	delete mVertexData;
	delete mSkyData;
	delete mFXParticleVertices;
	delete mFXParticleIndices;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	mRenderState.reset();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
}

void MetalRenderDevice::InitializeState()
{
	vendorstring = "Apple";
	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = 256;
	maxuniformblock = 65536;

	// Ray traced shadows replace the shadow maps when the GPU supports Metal ray
	// tracing. Shaders are compiled for it at startup, so changing it needs a restart.
	mRaytracing = vk_raytrace && device.supportsRaytracing;
	mRaytrace.reset(new MtlRaytrace(this));
	if (mRaytracing)
		mRaytrace->SetLevelMesh(nullptr);

	mBufferManager.reset(new MtlBufferManager(this));
	mSamplerManager.reset(new MtlSamplerManager(this));
	mTextureManager.reset(new MtlTextureManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new MtlRenderBuffers(this));
	mSaveBuffers.reset(new MtlRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new MtlPostprocess(this));
	mShaderManager.reset(new MtlShaderManager(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mRenderState.reset(new MtlRenderState(this));

	Printf("Metal device: %s%s\n", device.name.UTF8String, mRaytracing ? ", ray traced shadows" : "");
}

/////////////////////////////////////////////////////////////////////////////

id<MTLCommandBuffer> MetalRenderDevice::GetDrawCommands()
{
	if (!mDrawCommands)
		mDrawCommands = [queue commandBuffer];
	return mDrawCommands;
}

id<MTLBlitCommandEncoder> MetalRenderDevice::GetUploadEncoder()
{
	if (!mUploadCommands)
		mUploadCommands = [queue commandBuffer];
	if (!mUploadEncoder)
		mUploadEncoder = [mUploadCommands blitCommandEncoder];
	return mUploadEncoder;
}

void MetalRenderDevice::FlushCommands(bool wait)
{
	if (mRenderState)
		mRenderState->EndRenderPass();

	id<MTLCommandBuffer> upload = nil, draw = nil;
	if (mUploadCommands)
	{
		if (mUploadEncoder)
			[mUploadEncoder endEncoding];
		mUploadEncoder = nil;
		[mUploadCommands commit];
		upload = mUploadCommands;
		mUploadCommands = nil;
	}
	if (mDrawCommands)
	{
		[mDrawCommands commit];
		draw = mDrawCommands;
		mDrawCommands = nil;
	}
	if (wait)
	{
		if (draw) [draw waitUntilCompleted];
		if (upload) [upload waitUntilCompleted];
		for (id<MTLCommandBuffer> cb : { upload, draw })
		{
			if (cb && cb.GPUEndTime > cb.GPUStartTime)
				mFrameGpuTime += cb.GPUEndTime - cb.GPUStartTime;
		}
	}
}

void MetalRenderDevice::WaitForCommands(bool finish)
{
	FlushCommands(true);
}

id<MTLTexture> MetalRenderDevice::GetDrawableTexture()
{
	if (!mDrawable)
		mDrawable = [layer nextDrawable];
	return mDrawable ? mDrawable.texture : nil;
}

void MetalRenderDevice::Present()
{
	Finish.Reset();
	Finish.Clock();

	CGSize size = CGSizeMake(GetClientWidth(), GetClientHeight());
	if (size.width > 0 && size.height > 0 && !CGSizeEqualToSize(layer.drawableSize, size))
		layer.drawableSize = size;

	mPostprocess->DrawPresentTexture(mOutputLetterbox, true, false);
	if (mDrawable)
		[GetDrawCommands() presentDrawable:mDrawable];

	if (!mVSync)
		FPSLimit();

	FlushCommands(true);
	mDrawable = nil;

	if (gpuStatActive)
		gpuStatOutput.Format("Metal GPU frame=%04.2f ms\n", mFrameGpuTime * 1000.0);
	mFrameGpuTime = 0.0;
	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;

	Finish.Unclock();
}

void MetalRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	twod->Clear();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	Present();

	// The game loop never returns to the run loop, so drain Metal's autoreleased objects here.
	if (framePool)
		objc_autoreleasePoolPop(framePool);
	framePool = objc_autoreleasePoolPush();

	Super::Update();
}

/////////////////////////////////////////////////////////////////////////////

bool MetalRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void MetalRenderDevice::RenderTextureView(FCanvasTexture *tex, std::function<void(IntRect &)> renderFunc)
{
	auto baseLayer = static_cast<MtlHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	MtlTextureImage *image = baseLayer->GetImage(tex, 0, 0);
	MtlTextureImage *depthStencil = baseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();
	mRenderState->SetRenderTarget(image, depthStencil->Texture, image->Width(), image->Height(), image->Texture.pixelFormat);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = std::min(tex->GetWidth(), image->Width());
	bounds.height = std::min(tex->GetHeight(), image->Height());

	renderFunc(bounds);

	mRenderState->EndRenderPass();
	SetSceneRenderTarget(false);

	tex->SetUpdated(true);
}

void MetalRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene)
		mPostprocess->BlitSceneToPostprocess();
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

const char *MetalRenderDevice::DeviceName() const
{
	static FString name;
	name = device.name.UTF8String;
	return name.GetChars();
}

void MetalRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
#if !TARGET_OS_IPHONE
	layer.displaySyncEnabled = vsync ? YES : NO;
#endif
}

void MetalRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas)
		return;

	MaterialLayerInfo *layerInfo;
	auto systex = static_cast<MtlHardwareTexture*>(mat->GetLayer(0, translation, &layerInfo));
	systex->GetImage(layerInfo->layerTexture, translation, layerInfo->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<MtlHardwareTexture*>(mat->GetLayer(i, 0, &layerInfo));
		syslayer->GetImage(layerInfo->layerTexture, 0, layerInfo->scaleFlags);
	}
}

IHardwareTexture *MetalRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new MtlHardwareTexture(this, numchannels);
}

FMaterial *MetalRenderDevice::CreateMaterial(FGameTexture *tex, int scaleflags)
{
	return new MtlMaterial(this, tex, scaleflags);
}

IVertexBuffer *MetalRenderDevice::CreateVertexBuffer()
{
	return mBufferManager->CreateVertexBuffer();
}

IIndexBuffer *MetalRenderDevice::CreateIndexBuffer()
{
	return mBufferManager->CreateIndexBuffer();
}

IDataBuffer *MetalRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return mBufferManager->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void MetalRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
		mSamplerManager->ResetHWSamplers();
}

void MetalRenderDevice::StartPrecaching()
{
	// Drop cached material bindings so they cannot refer to stale textures.
	if (mSamplerManager)
		mSamplerManager->Generation++;
}

void MetalRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void MetalRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *MetalRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<MtlHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");
	return tex;
}

FTexture *MetalRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<MtlHardwareTexture*>(tex->GetSystemTexture());
	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");
	return tex;
}

static uint8_t HalfToByte(uint16_t h)
{
	uint32_t sign = h >> 15;
	uint32_t exponent = (h >> 10) & 0x1f;
	uint32_t mantissa = h & 0x3ff;
	float value;
	if (exponent == 0)
		value = mantissa / 16777216.0f;
	else if (exponent == 31)
		value = mantissa ? 0.0f : 1.0f;
	else
		value = (1.0f + mantissa / 1024.0f) * ldexpf(1.0f, int(exponent) - 15);
	if (sign)
		value = 0.0f;
	return (uint8_t)std::clamp(int(value * 255.0f + 0.5f), 0, 255);
}

void MetalRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	auto source = GetBuffers()->PipelineImage[mPostprocess->GetCurrentPipelineImage()].Texture;
	w = std::min(w, (int)source.width);
	h = std::min(h, (int)source.height);

	mRenderState->EndRenderPass();
	id<MTLBuffer> staging = [device newBufferWithLength:w * h * 8 options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit = [GetDrawCommands() blitCommandEncoder];
	[blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		toBuffer:staging destinationOffset:0 destinationBytesPerRow:w * 8 destinationBytesPerImage:w * h * 8];
	[blit endEncoding];
	FlushCommands(true);

	// rgba16f to rgb8, bottom row first as the other backends return it
	const uint16_t *pixels = (const uint16_t*)staging.contents;
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		const uint16_t *row = pixels + (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			data[dindex++] = HalfToByte(row[x * 4]);
			data[dindex++] = HalfToByte(row[x * 4 + 1]);
			data[dindex++] = HalfToByte(row[x * 4 + 2]);
		}
	}
}

void MetalRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> MetalRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void MetalRenderDevice::BeginFrame()
{
	SetViewportRects(nullptr);
	mViewpoints->Clear();
	mTextureManager->BeginFrame();
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
}

void MetalRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t> &LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		mTextureManager->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
		LMTextureData.Reset();
	}
}

void MetalRenderDevice::InitIrradianceVolume(int width, int height, int layers, TArray<uint16_t> &rgbaHalfData)
{
	mTextureManager->SetIrradianceVolume(width, height, layers, rgbaHalfData);
}

void MetalRenderDevice::Draw2D()
{
	::Draw2D(twod, *mRenderState);
}

void MetalRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void MetalRenderDevice::SetSaveBuffers(bool yes)
{
	mActiveRenderBuffers = yes ? mSaveBuffers.get() : mScreenBuffers.get();
}

void MetalRenderDevice::ImageTransitionScene(bool unknown)
{
}

FRenderState *MetalRenderDevice::RenderState()
{
	return mRenderState.get();
}

void MetalRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void MetalRenderDevice::SetLevelMesh(hwrenderer::LevelMesh *mesh)
{
	if (mRaytracing)
		mRaytrace->SetLevelMesh(mesh);
}

void MetalRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	auto buffers = GetBuffers();
	mRenderState->SetRenderTarget(&buffers->SceneColor, buffers->SceneDepthStencil.Texture, buffers->GetWidth(), buffers->GetHeight(), MTLPixelFormatRGBA16Float, buffers->GetSceneSamples());
}

/////////////////////////////////////////////////////////////////////////////

SystemBaseFrameBuffer *CreateMetalFrameBuffer(void *metalLayer, bool fullscreen)
{
	return new MetalRenderDevice((__bridge CAMetalLayer*)metalLayer, fullscreen);
}

bool MetalIsSupported()
{
	return MTLCreateSystemDefaultDevice() != nil;
}

/////////////////////////////////////////////////////////////////////////////

MtlRaytrace::MtlRaytrace(MetalRenderDevice *fb) : fb(fb)
{
	// A small box away from any map, used while no level is loaded.
	NullMesh = new hwrenderer::LevelMesh;
	const FVector3 corners[] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } };
	for (int z = 0; z < 2; z++)
		for (auto c : corners)
			NullMesh->MeshVertices.Push({ c.X, c.Y, z ? 1.f : -1.f });
	for (int i = 0; i < 12; i++)
		NullMesh->MeshElements.Push(i);
}

MtlRaytrace::~MtlRaytrace()
{
	delete NullMesh;
}

void MtlRaytrace::SetLevelMesh(hwrenderer::LevelMesh *mesh)
{
	if (!mesh)
		mesh = NullMesh;
	if (mesh == Mesh && Built)
		return;
	Mesh = mesh;
	Built = true;
	TopLevel = nil;
	BottomLevel = nil;
	if (mesh && mesh->MeshElements.Size() >= 3)
		Build(mesh);
}

void MtlRaytrace::Build(const hwrenderer::LevelMesh *mesh)
{
	id<MTLDevice> device = fb->device;
	id<MTLBuffer> vertices = [device newBufferWithBytes:mesh->MeshVertices.Data() length:mesh->MeshVertices.Size() * sizeof(FVector3) options:MTLResourceStorageModeShared];
	id<MTLBuffer> indices = [device newBufferWithBytes:mesh->MeshElements.Data() length:mesh->MeshElements.Size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];

	MTLAccelerationStructureTriangleGeometryDescriptor *geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geometry.vertexBuffer = vertices;
	geometry.vertexStride = sizeof(FVector3);
	geometry.indexBuffer = indices;
	geometry.indexType = MTLIndexTypeUInt32;
	geometry.triangleCount = mesh->MeshElements.Size() / 3;
	geometry.opaque = YES;

	MTLPrimitiveAccelerationStructureDescriptor *primitive = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	primitive.geometryDescriptors = @[ geometry ];
	MTLAccelerationStructureSizes primitiveSizes = [device accelerationStructureSizesWithDescriptor:primitive];
	BottomLevel = [device newAccelerationStructureWithSize:primitiveSizes.accelerationStructureSize];

	MTLAccelerationStructureInstanceDescriptor instance = {};
	instance.transformationMatrix.columns[0] = { 1, 0, 0 };
	instance.transformationMatrix.columns[1] = { 0, 1, 0 };
	instance.transformationMatrix.columns[2] = { 0, 0, 1 };
	instance.transformationMatrix.columns[3] = { 0, 0, 0 };
	instance.options = MTLAccelerationStructureInstanceOptionOpaque;
	instance.mask = 0xff;
	instance.accelerationStructureIndex = 0;
	id<MTLBuffer> instanceBuffer = [device newBufferWithBytes:&instance length:sizeof(instance) options:MTLResourceStorageModeShared];

	MTLInstanceAccelerationStructureDescriptor *instances = [MTLInstanceAccelerationStructureDescriptor descriptor];
	instances.instancedAccelerationStructures = @[ BottomLevel ];
	instances.instanceCount = 1;
	instances.instanceDescriptorBuffer = instanceBuffer;
	MTLAccelerationStructureSizes instanceSizes = [device accelerationStructureSizesWithDescriptor:instances];
	TopLevel = [device newAccelerationStructureWithSize:instanceSizes.accelerationStructureSize];

	id<MTLBuffer> scratch = [device newBufferWithLength:std::max(primitiveSizes.buildScratchBufferSize, instanceSizes.buildScratchBufferSize) options:MTLResourceStorageModePrivate];

	// Built once per level, so a separate command buffer that is waited for is fine.
	id<MTLCommandBuffer> commands = [fb->queue commandBuffer];
	id<MTLAccelerationStructureCommandEncoder> encoder = [commands accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:BottomLevel descriptor:primitive scratchBuffer:scratch scratchBufferOffset:0];
	[encoder endEncoding];
	encoder = [commands accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:TopLevel descriptor:instances scratchBuffer:scratch scratchBufferOffset:0];
	[encoder endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
}

/////////////////////////////////////////////////////////////////////////////
// MetalFX temporal upscaling

bool MetalRenderDevice::SupportsPlatformUpscaler()
{
#ifdef HAVE_METALFX
	if (@available(macOS 13.0, iOS 16.0, *))
		return [MTLFXTemporalScalerDescriptor supportsDevice:device];
#endif
	return false;
}

void MetalRenderDevice::PlatformTemporalUpscale(PPTexture *color, PPTexture *depth, PPTexture *motion, PPTexture *output, float jitterX, float jitterY, bool reset)
{
#ifdef HAVE_METALFX
	if (@available(macOS 13.0, iOS 16.0, *))
	{
		auto textures = GetTextureManager();
		id<MTLTexture> colorTex = textures->GetTexture(PPTextureType::PPTexture, color)->Texture;
		id<MTLTexture> depthTex = textures->GetTexture(PPTextureType::PPTexture, depth)->Texture;
		id<MTLTexture> motionTex = textures->GetTexture(PPTextureType::PPTexture, motion)->Texture;
		id<MTLTexture> outputTex = textures->GetTexture(PPTextureType::PPTexture, output)->Texture;

		id<MTLFXTemporalScaler> scaler = mTemporalScaler;
		if (!scaler || scaler.inputWidth != colorTex.width || scaler.inputHeight != colorTex.height || scaler.outputWidth != outputTex.width || scaler.outputHeight != outputTex.height)
		{
			MTLFXTemporalScalerDescriptor *desc = [MTLFXTemporalScalerDescriptor new];
			desc.colorTextureFormat = colorTex.pixelFormat;
			desc.depthTextureFormat = depthTex.pixelFormat;
			desc.motionTextureFormat = motionTex.pixelFormat;
			desc.outputTextureFormat = outputTex.pixelFormat;
			desc.inputWidth = colorTex.width;
			desc.inputHeight = colorTex.height;
			desc.outputWidth = outputTex.width;
			desc.outputHeight = outputTex.height;
			desc.autoExposureEnabled = YES;	// the input is HDR, before tone mapping
			scaler = [desc newTemporalScalerWithDevice:device];
			mTemporalScaler = scaler;
			reset = true;
			if (!scaler)
				return;
		}

		mRenderState->EndRenderPass();
		scaler.colorTexture = colorTex;
		scaler.depthTexture = depthTex;
		scaler.motionTexture = motionTex;
		scaler.outputTexture = outputTex;
		scaler.inputContentWidth = colorTex.width;
		scaler.inputContentHeight = colorTex.height;
		scaler.jitterOffsetX = jitterX;
		scaler.jitterOffsetY = -jitterY;
		scaler.motionVectorScaleX = colorTex.width;
		scaler.motionVectorScaleY = colorTex.height;
		scaler.depthReversed = NO;
		scaler.reset = reset;
		[scaler encodeToCommandBuffer:GetDrawCommands()];
	}
#endif
}
