#pragma once

// Metal backend resources: buffers, textures, samplers, shaders and render targets.
//
// The scene and postprocess shaders are GZDoom's GLSL, compiled to SPIR-V with
// glslang and translated to MSL with SPIRV-Cross. The Vulkan (set, binding)
// layout is mapped onto fixed Metal argument table slots (below), so the C++
// side binds resources the same way for every shader.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "hwrenderer/data/buffers.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_ihwtexture.h"
#include "hw_material.h"
#include "hw_renderstate.h"
#include "tarray.h"
#include "zstring.h"

#include <list>
#include <map>
#include <memory>
#include <vector>

class MetalRenderDevice;
class FTexture;
struct FMaterialState;

enum
{
	// Scene shaders, both stages
	MTLBUF_VIEWPOINT = 0,
	MTLBUF_MATRICES = 1,
	MTLBUF_STREAM = 2,
	MTLBUF_LIGHTS = 3,
	MTLBUF_BONES = 4,
	MTLBUF_PUSHCONSTANTS = 5,
	MTLBUF_VERTEX0 = 6,		// vertex buffer binding points 0 and 1
	MTLBUF_VERTEX1 = 7,
	MTLBUF_ACCELERATION = 8,	// ray traced shadows: top level acceleration structure

	MTLTEX_MATERIAL_COUNT = 12,	// tex, texture2 .. texture12
	MTLTEX_SHADOWMAP = 12,
	MTLTEX_LIGHTMAP = 13,
	MTLTEX_IRRADIANCE = 14,

	// Postprocess shaders, fragment stage
	MTLBUF_PP_UNIFORMS = 0,
	MTLBUF_PP_STORAGE = 1,	// + shader storage binding point
	MTLBUF_PP_VERTEX = 16,
};

struct MtlTextureImage
{
	id<MTLTexture> Texture = nil;

	void Reset() { Texture = nil; }
	int Width() const { return Texture ? (int)Texture.width : 0; }
	int Height() const { return Texture ? (int)Texture.height : 0; }
};

/////////////////////////////////////////////////////////////////////////////
// Buffers

class MtlHardwareBuffer : virtual public IBuffer
{
public:
	MtlHardwareBuffer(MetalRenderDevice *fb);
	~MtlHardwareBuffer();

	void Reset();

	void SetData(size_t size, const void *data, BufferUsageType usage) override;
	void SetSubData(size_t offset, size_t size, const void *data) override;
	void Resize(size_t newsize) override;

	void Map() override;
	void Unmap() override;

	void *Lock(unsigned int size) override;
	void Unlock() override;

	MetalRenderDevice *fb = nullptr;
	std::list<MtlHardwareBuffer*>::iterator it;
	id<MTLBuffer> mBuffer = nil;

protected:
	void Allocate(size_t size, const void *data);
};

class MtlHardwareVertexBuffer : public IVertexBuffer, public MtlHardwareBuffer
{
public:
	MtlHardwareVertexBuffer(MetalRenderDevice *fb) : MtlHardwareBuffer(fb) { }
	void SetFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs) override;

	int VertexFormat = -1;
};

class MtlHardwareIndexBuffer : public IIndexBuffer, public MtlHardwareBuffer
{
public:
	MtlHardwareIndexBuffer(MetalRenderDevice *fb) : MtlHardwareBuffer(fb) { }
};

class MtlHardwareDataBuffer : public IDataBuffer, public MtlHardwareBuffer
{
public:
	MtlHardwareDataBuffer(MetalRenderDevice *fb, int bindingpoint) : MtlHardwareBuffer(fb), bindingpoint(bindingpoint) { }
	void BindRange(FRenderState *state, size_t start, size_t length) override;

	int bindingpoint;
};

// A ring of uniform blocks; the scene shaders index into the current block.
class MtlStreamBuffer
{
public:
	MtlStreamBuffer(MetalRenderDevice *fb, size_t structSize, size_t count);
	~MtlStreamBuffer();

	uint32_t NextStreamDataBlock();
	void Reset() { mStreamDataOffset = 0; }

	MtlHardwareDataBuffer *UniformBuffer = nullptr;

private:
	uint32_t mBlockSize = 0;
	uint32_t mStreamDataOffset = 0;
};

struct MtlVertexFormat
{
	int NumBindingPoints;
	size_t Stride;
	std::vector<FVertexBufferAttribute> Attrs;
	int UseVertexData;
};

class MtlBufferManager
{
public:
	MtlBufferManager(MetalRenderDevice *fb);
	~MtlBufferManager();

	void Init();
	void Deinit();

	IVertexBuffer *CreateVertexBuffer();
	IIndexBuffer *CreateIndexBuffer();
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

	void AddBuffer(MtlHardwareBuffer *buffer);
	void RemoveBuffer(MtlHardwareBuffer *buffer);

	int GetVertexFormat(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute *attrs);
	MtlVertexFormat *GetVertexFormat(int index) { return &VertexFormats[index]; }

	MtlHardwareDataBuffer *ViewpointUBO = nullptr;
	MtlHardwareDataBuffer *LightBufferSSO = nullptr;
	MtlHardwareDataBuffer *LightNodes = nullptr;
	MtlHardwareDataBuffer *LightLines = nullptr;
	MtlHardwareDataBuffer *LightList = nullptr;
	MtlHardwareDataBuffer *BoneBufferSSO = nullptr;

	std::unique_ptr<MtlStreamBuffer> MatrixBuffer;
	std::unique_ptr<MtlStreamBuffer> StreamBuffer;
	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;

private:
	MetalRenderDevice *fb = nullptr;
	std::list<MtlHardwareBuffer*> Buffers;
	std::vector<MtlVertexFormat> VertexFormats;
};

/////////////////////////////////////////////////////////////////////////////
// Textures

class MtlHardwareTexture : public IHardwareTexture
{
	friend class MtlMaterial;
public:
	MtlHardwareTexture(MetalRenderDevice *fb, int numchannels);
	~MtlHardwareTexture();

	void Reset();

	// Software renderer output
	void AllocateBuffer(int w, int h, int texelsize) override;
	uint8_t *MapBuffer() override;
	unsigned int CreateTexture(unsigned char *buffer, int w, int h, int texunit, bool mipmap, const char *name) override;

	void CreateWipeTexture(int w, int h, const char *name);

	MtlTextureImage *GetImage(FTexture *tex, int translation, int flags);
	MtlTextureImage *GetDepthStencil(FTexture *tex);

	MetalRenderDevice *fb = nullptr;
	std::list<MtlHardwareTexture*>::iterator it;

private:
	void CreateImage(FTexture *tex, int translation, int flags);
	void CreateTexture(int w, int h, int pixelsize, MTLPixelFormat format, const void *pixels, bool mipmap);

	MtlTextureImage mImage;
	MtlTextureImage mDepthStencil;
	id<MTLBuffer> mLinearBuffer = nil;	// backs the software renderer's output texture
	int mTexelsize = 4;
};

class MtlMaterial : public FMaterial
{
public:
	MtlMaterial(MetalRenderDevice *fb, FGameTexture *tex, int scaleflags);
	~MtlMaterial();

	struct Binding
	{
		int generation;
		int clampmode;
		intptr_t remap;
		id<MTLTexture> textures[MTLTEX_MATERIAL_COUNT];
		id<MTLSamplerState> sampler;
	};

	const Binding *GetBinding(const FMaterialState &state);
	void DeleteDescriptors() override;

	MetalRenderDevice *fb = nullptr;
	std::list<MtlMaterial*>::iterator it;

private:
	std::vector<std::unique_ptr<Binding>> mBindings;
};

class MtlSamplerManager
{
public:
	MtlSamplerManager(MetalRenderDevice *fb);

	void ResetHWSamplers();

	// Material bindings cache textures and samplers; a new generation rebuilds them.
	int Generation = 0;

	id<MTLSamplerState> Get(int no) const { return mSamplers[no]; }
	id<MTLSamplerState> Get(PPFilterMode filter, PPWrapMode wrap);

	id<MTLSamplerState> ShadowmapSampler = nil;
	id<MTLSamplerState> LightmapSampler = nil;

private:
	void CreateHWSamplers();

	MetalRenderDevice *fb = nullptr;
	id<MTLSamplerState> mSamplers[NUMSAMPLERS] = {};
	id<MTLSamplerState> mPPSamplers[4] = {};
};

class MtlPPTexture : public PPTextureBackend
{
public:
	MtlPPTexture(MetalRenderDevice *fb, PPTexture *texture);
	~MtlPPTexture();

	void Reset() { TexImage.Reset(); }

	MetalRenderDevice *fb = nullptr;
	std::list<MtlPPTexture*>::iterator it;
	MtlTextureImage TexImage;
	MTLPixelFormat Format = MTLPixelFormatRGBA8Unorm;
};

class MtlTextureManager
{
public:
	MtlTextureManager(MetalRenderDevice *fb);
	~MtlTextureManager();

	void Deinit();
	void BeginFrame();

	void SetLightmap(int size, int count, const TArray<uint16_t> &data);
	void SetIrradianceVolume(int width, int height, int layers, const TArray<uint16_t> &rgbaHalfData);

	MtlTextureImage *GetTexture(PPTextureType type, PPTexture *tex);
	MTLPixelFormat GetTextureFormat(PPTexture *tex);

	void AddTexture(MtlHardwareTexture *texture);
	void RemoveTexture(MtlHardwareTexture *texture);
	void AddPPTexture(MtlPPTexture *texture);
	void RemovePPTexture(MtlPPTexture *texture);

	id<MTLTexture> NullTexture = nil;
	MtlTextureImage Shadowmap;
	MtlTextureImage Lightmap;
	MtlTextureImage Irradiance;

private:
	void CreateShadowmap();
	void UploadHalfArray(MtlTextureImage &image, int w, int h, int count, const uint16_t *src, int srcChannels);
	MtlPPTexture *GetMtlTexture(PPTexture *texture);

	MetalRenderDevice *fb = nullptr;
	std::list<MtlHardwareTexture*> Textures;
	std::list<MtlPPTexture*> PPTextures;
};

class MtlRenderBuffers
{
public:
	MtlRenderBuffers(MetalRenderDevice *fb);

	void BeginFrame(int width, int height, int sceneWidth, int sceneHeight);

	int GetWidth() const { return mWidth; }
	int GetHeight() const { return mHeight; }
	int GetSceneWidth() const { return mSceneWidth; }
	int GetSceneHeight() const { return mSceneHeight; }
	int GetSceneSamples() const { return mSamples; }

	MtlTextureImage SceneColor;
	MtlTextureImage SceneDepthStencil;
	MtlTextureImage SceneNormal;
	MtlTextureImage SceneFog;

	static const int NumPipelineImages = 2;
	MtlTextureImage PipelineDepthStencil;
	MtlTextureImage PipelineImage[NumPipelineImages];

	static const MTLPixelFormat DepthStencilFormat = MTLPixelFormatDepth32Float_Stencil8;
	static const MTLPixelFormat SceneNormalFormat = MTLPixelFormatRGB10A2Unorm;

private:
	id<MTLTexture> CreateTarget(int width, int height, MTLPixelFormat format, int samples = 1);
	int GetBestSampleCount();

	MetalRenderDevice *fb = nullptr;
	int mWidth = 0;
	int mHeight = 0;
	int mSamples = 1;
	int mSceneWidth = 0;
	int mSceneHeight = 0;
};

/////////////////////////////////////////////////////////////////////////////
// Ray tracing

namespace hwrenderer { class LevelMesh; }

// Acceleration structures over the level mesh, for the ray traced shadows of
// the scene shaders (SUPPORTS_RAYTRACING), on GPUs with Metal ray tracing.
class MtlRaytrace
{
public:
	MtlRaytrace(MetalRenderDevice *fb);
	~MtlRaytrace();

	void SetLevelMesh(hwrenderer::LevelMesh *mesh);

	id<MTLAccelerationStructure> TopLevel = nil;
	id<MTLAccelerationStructure> BottomLevel = nil;

private:
	void Build(const hwrenderer::LevelMesh *mesh);

	MetalRenderDevice *fb = nullptr;
	hwrenderer::LevelMesh *Mesh = nullptr;
	hwrenderer::LevelMesh *NullMesh = nullptr;	// shaders always need a structure bound
	bool Built = false;
};

/////////////////////////////////////////////////////////////////////////////
// Shaders

struct MtlShaderProgram
{
	id<MTLFunction> vert = nil;
	id<MTLFunction> frag = nil;
};

class MtlPPShader : public PPShaderBackend
{
public:
	MtlPPShader(MetalRenderDevice *fb, PPShader *shader);

	id<MTLFunction> VertexFunction = nil;
	id<MTLFunction> FragmentFunction = nil;
};

enum class MtlShaderLayout { Scene, PostProcess };

class MtlShaderManager
{
public:
	MtlShaderManager(MetalRenderDevice *fb, bool raytracing);

	MtlShaderProgram *GetEffect(int effect, EPassType passType);
	MtlShaderProgram *Get(unsigned int eff, bool alphateston, EPassType passType);
	bool CompileNextShader();

	MtlPPShader *GetPPShader(PPShader *shader);

	// GLSL -> SPIR-V (glslang) -> MSL (SPIRV-Cross) -> MTLFunction
	id<MTLFunction> Compile(const char *name, const FString &glsl, bool vertex, MtlShaderLayout layout);

private:
	MtlShaderProgram LoadProgram(const char *name, const char *vert_lump, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass);

	MetalRenderDevice *fb = nullptr;
	bool mRaytracing = false;

	std::vector<MtlShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
	std::vector<MtlShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
	std::vector<MtlShaderProgram> mEffectShaders[MAX_PASS_TYPES];
	uint8_t compilePass = 0, compileState = 0;
	int compileIndex = 0;
};
