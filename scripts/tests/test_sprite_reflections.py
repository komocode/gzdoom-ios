#!/usr/bin/env python3
"""Run the actual CPU reflection baker against small synthetic rooms.

Extracts the renderer's self-contained bake types, omitting only main-thread
TexMan capture. This exercises production BVH, shading, octahedral sampling and
roughness filtering without starting the game or requiring an IWAD.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/rendering/hwrenderer/hw_irradiance.cpp').read_text()
code = source[source.index('struct Vec3\n'):source.index('std::unique_ptr<BakeJob> job;')]
start = code.index('\tint CaptureReflectionTexture(')
end = code.index('\n\tstd::atomic<int> nextSlice', start)
code = code[:start] + code[end:]
upload = source[source.index('void Upload()\n'):]
atlas = upload[upload.index('\tbool hasReflections'):upload.index('\tTArray<uint16_t> data;')]
code += '\nstruct AtlasDimensions { int width, height, layers; };\nAtlasDimensions TestAtlas(BakeJob &b) {\n' + atlas + '\nreturn {width,height,layers}; }\n'
includes = '''#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef __APPLE__
#include <pthread.h>
#endif
'''
harness = r'''
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Quad(BakeJob &job, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal, Vec3 color) {
    Material material{}; material.albedo = color; material.light = {1, 1, 1};
    int index = int(job.materials.size()); job.materials.push_back(material);
    job.triangles.push_back({a, b-a, c-a, normal, index});
    job.triangles.push_back({a, c-a, d-a, normal, index});
}
void Room(BakeJob &job, bool colored) {
    Vec3 white(0.5f, 0.5f, 0.5f), red = colored ? Vec3(1,0,0) : white, blue = colored ? Vec3(0,0,1) : white;
    Quad(job, {-64,-64,0}, {-64,64,0}, {-64,64,128}, {-64,-64,128}, {1,0,0}, red);
    Quad(job, {64,-64,0}, {64,64,0}, {64,64,128}, {64,-64,128}, {-1,0,0}, blue);
    Quad(job, {-64,-64,0}, {64,-64,0}, {64,-64,128}, {-64,-64,128}, {0,1,0}, white);
    Quad(job, {-64,64,0}, {64,64,0}, {64,64,128}, {-64,64,128}, {0,-1,0}, white);
    Quad(job, {-64,-64,0}, {64,-64,0}, {64,64,0}, {-64,64,0}, {0,0,1}, white);
    Quad(job, {-64,-64,128}, {64,-64,128}, {64,64,128}, {-64,64,128}, {0,0,-1}, white);
    job.bvh.Build(job.triangles);
    job.reflectionX = job.reflectionY = job.reflectionZ = 1;
    job.reflectionOrigin = {0,0,64}; job.reflectionFloor = {0}; job.reflectionCeiling = {128};
    job.reflectionProbes.resize(1);
}
float Variance(const ReflectionProbe &probe, int level) {
    const int count = ReflectionResolution * ReflectionResolution;
    Vec3 mean; float variance = 0;
    for (int i=0; i<count; ++i) mean += probe.pixels[level*count+i] * (1.f/count);
    for (int i=0; i<count; ++i) { Vec3 d=probe.pixels[level*count+i]-mean; variance += Dot(d,d)/count; }
    return variance;
}
int main() {
    BakeJob uniform; Room(uniform,false); uniform.BakeReflection(0);
    require(uniform.reflectionProbes[0].valid, "interior probe must be valid");
    float expected = std::pow(0.5f,2.2f);
    for (const Vec3 &p:uniform.reflectionProbes[0].pixels)
        require(std::abs(p.x-expected)<1e-5f && std::abs(p.y-expected)<1e-5f && std::abs(p.z-expected)<1e-5f,
                "uniform radiance must conserve energy at every roughness");
    BakeJob colored; Room(colored,true); colored.BakeReflection(0);
    const ReflectionProbe &probe = colored.reflectionProbes[0];
    require(probe.valid,"colored-room probe valid");
    float first = Variance(probe,0), last = Variance(probe,ReflectionLevels-1);
    require(first > 0.05f && last < first*0.5f,"roughness must blur room features");
    bool red=false, blue=false;
    for (int i=0; i<ReflectionResolution*ReflectionResolution; ++i) {
        const Vec3 &p=probe.pixels[i]; red |= p.x>0.9f && p.z<0.1f; blue |= p.z>0.9f && p.x<0.1f;
    }
    require(red && blue,"sharp radiance must preserve distinct colored walls");
    BakeJob textured; Room(textured,false);
    ReflectionTexture texture; texture.width=2; texture.height=1; texture.linearPixels={{1,0,0},{0,1,0}};
    textured.reflectionTextures.push_back(texture);
    Material &m=textured.materials[0]; m.reflectionTexture=0; m.textureU={0,1.f/128.f,0};
    Vec3 a=textured.ShadeHit({-64,16,64},textured.triangles[0],true), b=textured.ShadeHit({-64,-16,64},textured.triangles[0],true);
    require(a.x==1.f && b.y==1.f,"hit-coordinate texture sampling and negative UV wrapping");
    BakeJob invalid; Room(invalid,false); invalid.reflectionOrigin.x=256; invalid.BakeReflection(0);
    require(!invalid.reflectionProbes[0].valid,"outside/backface probes must be rejected");
    BakeJob cancelled; Room(cancelled,false); cancelled.cancel=true; cancelled.BakeReflection(0);
    require(!cancelled.reflectionProbes[0].valid,"cancelled bake must never publish a probe");
    for(int y=0;y<ReflectionResolution;y++) for(int x=0;x<ReflectionResolution;x++) {
        Vec3 d=OctaDirection((x+.5f)*2/ReflectionResolution-1,(y+.5f)*2/ReflectionResolution-1);
        require(std::abs(Length(d)-1)<1e-5f,"octahedral rays must be unit length");
    }
    BakeJob atlas;
    atlas.reflectionProbes.resize(MaxReflectionProbes);
    for (int z=2; z<=33; z++) for (int y=2; y<=193; y+=7) for (int x=2; x<=193; x+=7) {
        atlas.sizeX=x; atlas.sizeY=y; atlas.sizeZ=z;
        AtlasDimensions d=TestAtlas(atlas);
        require(d.width>=x && d.height>=y && d.layers<=256, "atlas must retain all GI texels and stay within 256 layers");
        require(size_t(d.width)*d.height*d.layers*8 < 96*1024*1024, "combined atlas memory must remain bounded");
    }
    std::printf("PASS: uniform energy, directional room features, roughness blur (variance %.4f -> %.4f), texture UVs, invalid-probe rejection, cancellation, normalized rays, atlas layer/memory bounds\n",first,last);
}
'''
with tempfile.TemporaryDirectory(prefix='gzdoom-reflection-test-') as temporary:
    path = Path(temporary)
    (path/'test.cpp').write_text(includes+code+harness)
    subprocess.run(['c++','-std=c++17','-O2','-pthread',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
