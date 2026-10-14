#pragma once

#include "mtl_objects.h"
#include "renderstyle.h"
#include "vulkan/shaders/vk_shader.h"	// PushConstants, MatricesUBO, StreamUBO: the shader interface is shared

class MtlStreamBufferWriter
{
public:
	MtlStreamBufferWriter(MetalRenderDevice *fb);

	bool Write(const StreamData &data);
	void Reset();

	uint32_t DataIndex() const { return mDataIndex; }
	uint32_t StreamDataOffset() const { return mStreamDataOffset; }

private:
	MtlStreamBuffer *mBuffer;
	uint32_t mDataIndex = MAX_STREAM_DATA - 1;
	uint32_t mStreamDataOffset = 0;
};

class MtlMatrixBufferWriter
{
public:
	MtlMatrixBufferWriter(MetalRenderDevice *fb);

	bool Write(const VSMatrix &modelMatrix, bool modelMatrixEnabled, const VSMatrix &textureMatrix, bool textureMatrixEnabled);
	void Reset();

	uint32_t Offset() const { return mOffset; }

private:
	MtlStreamBuffer *mBuffer;
	MatricesUBO mMatrices = {};
	VSMatrix mIdentityMatrix;
	uint32_t mOffset = 0;
};

struct MtlPipelineKey
{
	MtlShaderProgram *Program;
	FRenderStyle RenderStyle;
	int VertexFormat;
	int ColorMask;
	int DrawBuffers;
	MTLPixelFormat ColorFormat;
	int DepthStencil;
	int Samples;

	bool operator<(const MtlPipelineKey &other) const { return memcmp(this, &other, sizeof(MtlPipelineKey)) < 0; }
	bool operator!=(const MtlPipelineKey &other) const { return memcmp(this, &other, sizeof(MtlPipelineKey)) != 0; }
};

struct MtlDepthStencilKey
{
	int DepthTest;
	int DepthWrite;
	int DepthFunc;
	int StencilTest;
	int StencilOp;

	bool operator<(const MtlDepthStencilKey &other) const { return memcmp(this, &other, sizeof(MtlDepthStencilKey)) < 0; }
	bool operator!=(const MtlDepthStencilKey &other) const { return memcmp(this, &other, sizeof(MtlDepthStencilKey)) != 0; }
};

void MtlSetBlendMode(MTLRenderPipelineColorAttachmentDescriptor *attachment, const FRenderStyle &style);

class MtlRenderState : public FRenderState
{
public:
	MtlRenderState(MetalRenderDevice *fb);
	virtual ~MtlRenderState() = default;

	// Draw commands
	void ClearScreen() override;
	void Draw(int dt, int index, int count, bool apply = true) override;
	void DrawIndexed(int dt, int index, int count, bool apply = true) override;

	// Immediate render state change commands
	bool SetDepthClamp(bool on) override;
	void SetDepthMask(bool on) override;
	void SetDepthFunc(int func) override;
	void SetDepthRange(float min, float max) override;
	void SetColorMask(bool r, bool g, bool b, bool a) override;
	void SetStencil(int offs, int op, int flags = -1) override;
	void SetCulling(int mode) override;
	void EnableClipDistance(int num, bool state) override;
	void Clear(int targets) override;
	void EnableStencil(bool on) override;
	void SetScissor(int x, int y, int w, int h) override;
	void SetViewport(int x, int y, int w, int h) override;
	void EnableDepthTest(bool on) override;
	void EnableMultisampling(bool on) override;
	void EnableLineSmooth(bool on) override;
	void EnableDrawBuffers(int count, bool apply) override;

	void BeginFrame();
	void SetRenderTarget(MtlTextureImage *image, id<MTLTexture> depthStencil, int width, int height, MTLPixelFormat format, int samples = 1);
	void Bind(int bindingpoint, uint32_t offset);
	void EndRenderPass();
	void EndFrame();

private:
	void Apply(int dt);
	void BeginRenderPass();
	void ApplyPipeline();
	void ApplyDynamicState();
	void ApplyBuffers();
	void ApplyVertexBuffers(int extraVertexOffset);
	void ApplyMaterial();
	void ApplyStreamData();
	void ApplyMatrices();
	void ApplyPushConstants();
	void WaitForStreamBuffers();

	id<MTLRenderPipelineState> GetPipeline(const MtlPipelineKey &key);
	id<MTLDepthStencilState> GetDepthStencilState(const MtlDepthStencilKey &key);

	MetalRenderDevice *fb = nullptr;
	id<MTLRenderCommandEncoder> mEncoder = nil;

	bool mDepthClamp = true;
	int mClearTargets = 0;
	bool mNeedApply = true;

	int mScissorX = 0, mScissorY = 0, mScissorWidth = -1, mScissorHeight = -1;
	int mViewportX = 0, mViewportY = 0, mViewportWidth = -1, mViewportHeight = -1;
	float mViewportDepthMin = 0.0f, mViewportDepthMax = 1.0f;
	bool mScissorChanged = true;
	bool mViewportChanged = true;

	bool mDepthTest = false;
	bool mDepthWrite = false;
	bool mStencilTest = false;
	bool mStencilRefChanged = true;
	int mStencilRef = 0;
	int mStencilOp = 0;
	int mDepthFunc = 0;
	int mColorMask = 15;
	int mCullMode = 0;
	bool mRasterChanged = true;

	MtlPipelineKey mPipelineKey = {};
	MtlDepthStencilKey mDepthStencilKey = {};
	bool mHasPipeline = false;
	bool mHasDepthStencilState = false;

	PushConstants mPushConstants = {};

	uint32_t mViewpointOffset = 0;
	uint32_t mLastViewpointOffset = 0xffffffff;
	uint32_t mLastMatricesOffset = 0xffffffff;
	uint32_t mLastStreamDataOffset = 0xffffffff;
	id<MTLBuffer> mLastLightBuffer = nil;
	id<MTLBuffer> mLastBoneBuffer = nil;

	MtlStreamBufferWriter mStreamBufferWriter;
	MtlMatrixBufferWriter mMatrixBufferWriter;

	IVertexBuffer *mLastVertexBuffer = nullptr;
	int mLastVertexOffsets[2] = { -1, -1 };

	std::map<MtlPipelineKey, id<MTLRenderPipelineState>> mPipelines;
	std::map<MtlDepthStencilKey, id<MTLDepthStencilState>> mDepthStencilStates;

	struct RenderTarget
	{
		MtlTextureImage *Image = nullptr;
		id<MTLTexture> DepthStencil = nil;
		int Width = 0;
		int Height = 0;
		MTLPixelFormat Format = MTLPixelFormatRGBA16Float;
		int Samples = 1;
		int DrawBuffers = 1;
	} mRenderTarget;
};
