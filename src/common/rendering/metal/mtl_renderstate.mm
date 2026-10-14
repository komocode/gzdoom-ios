/*
** mtl_renderstate.mm
** FRenderState for the Metal backend.
*/

#include "mtl_renderstate.h"
#include "mtl_renderdevice.h"
#include "hw_viewpointuniforms.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "textures.h"

EXTERN_CVAR(Bool, r_skipmats)

MtlStreamBufferWriter::MtlStreamBufferWriter(MetalRenderDevice *fb)
{
	mBuffer = fb->GetBufferManager()->StreamBuffer.get();
}

bool MtlStreamBufferWriter::Write(const StreamData &data)
{
	mDataIndex++;
	if (mDataIndex == MAX_STREAM_DATA)
	{
		mDataIndex = 0;
		mStreamDataOffset = mBuffer->NextStreamDataBlock();
		if (mStreamDataOffset == 0xffffffff)
			return false;
	}
	uint8_t *ptr = (uint8_t*)mBuffer->UniformBuffer->Memory();
	memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &data, sizeof(StreamData));
	return true;
}

void MtlStreamBufferWriter::Reset()
{
	mDataIndex = MAX_STREAM_DATA - 1;
	mStreamDataOffset = 0;
	mBuffer->Reset();
}

MtlMatrixBufferWriter::MtlMatrixBufferWriter(MetalRenderDevice *fb)
{
	mBuffer = fb->GetBufferManager()->MatrixBuffer.get();
	mIdentityMatrix.loadIdentity();
}

static void BufferedSet(bool &modified, VSMatrix &dst, const VSMatrix &src)
{
	if (memcmp(dst.get(), src.get(), sizeof(FLOATTYPE) * 16) == 0)
		return;
	dst = src;
	modified = true;
}

bool MtlMatrixBufferWriter::Write(const VSMatrix &modelMatrix, bool modelMatrixEnabled, const VSMatrix &textureMatrix, bool textureMatrixEnabled)
{
	bool modified = (mOffset == 0); // always modified first call

	if (modelMatrixEnabled)
	{
		BufferedSet(modified, mMatrices.ModelMatrix, modelMatrix);
		if (modified)
			mMatrices.NormalModelMatrix.computeNormalMatrix(modelMatrix);
	}
	else
	{
		BufferedSet(modified, mMatrices.ModelMatrix, mIdentityMatrix);
		BufferedSet(modified, mMatrices.NormalModelMatrix, mIdentityMatrix);
	}

	if (textureMatrixEnabled)
		BufferedSet(modified, mMatrices.TextureMatrix, textureMatrix);
	else
		BufferedSet(modified, mMatrices.TextureMatrix, mIdentityMatrix);

	if (modified)
	{
		mOffset = mBuffer->NextStreamDataBlock();
		if (mOffset == 0xffffffff)
			return false;

		uint8_t *ptr = (uint8_t*)mBuffer->UniformBuffer->Memory();
		memcpy(ptr + mOffset, &mMatrices, sizeof(MatricesUBO));
	}
	return true;
}

void MtlMatrixBufferWriter::Reset()
{
	mOffset = 0;
	mBuffer->Reset();
}

/////////////////////////////////////////////////////////////////////////////

void MtlSetBlendMode(MTLRenderPipelineColorAttachmentDescriptor *attachment, const FRenderStyle &style)
{
	if (style.BlendOp == STYLEOP_Add && style.SrcAlpha == STYLEALPHA_One && style.DestAlpha == STYLEALPHA_Zero && style.Flags == 0)
	{
		attachment.blendingEnabled = NO;
		return;
	}

	static const MTLBlendFactor blendstyles[] =
	{
		MTLBlendFactorZero,
		MTLBlendFactorOne,
		MTLBlendFactorSourceAlpha,
		MTLBlendFactorOneMinusSourceAlpha,
		MTLBlendFactorSourceColor,
		MTLBlendFactorOneMinusSourceColor,
		MTLBlendFactorDestinationColor,
		MTLBlendFactorOneMinusDestinationColor,
		MTLBlendFactorDestinationAlpha,
		MTLBlendFactorOneMinusDestinationAlpha,
	};

	MTLBlendFactor srcblend = blendstyles[style.SrcAlpha % STYLEALPHA_MAX];
	MTLBlendFactor dstblend = blendstyles[style.DestAlpha % STYLEALPHA_MAX];
	MTLBlendOperation op;
	switch (style.BlendOp & 15)
	{
	case STYLEOP_Add: op = MTLBlendOperationAdd; break;
	case STYLEOP_Sub: op = MTLBlendOperationSubtract; break;
	case STYLEOP_RevSub: op = MTLBlendOperationReverseSubtract; break;
	default:
		// fuzz styles
		srcblend = MTLBlendFactorDestinationColor;
		dstblend = MTLBlendFactorOneMinusSourceAlpha;
		op = MTLBlendOperationAdd;
		break;
	}

	attachment.blendingEnabled = YES;
	attachment.rgbBlendOperation = op;
	attachment.alphaBlendOperation = op;
	attachment.sourceRGBBlendFactor = srcblend;
	attachment.sourceAlphaBlendFactor = srcblend;
	attachment.destinationRGBBlendFactor = dstblend;
	attachment.destinationAlphaBlendFactor = dstblend;
}

static MTLVertexFormat ToMtlVertexFormat(int format)
{
	switch (format)
	{
	case VFmt_Float4: return MTLVertexFormatFloat4;
	case VFmt_Float3: return MTLVertexFormatFloat3;
	case VFmt_Float2: return MTLVertexFormatFloat2;
	case VFmt_Float: return MTLVertexFormatFloat;
	case VFmt_Byte4: return MTLVertexFormatUChar4Normalized;
	case VFmt_Packed_A2R10G10B10: return MTLVertexFormatInt1010102Normalized;
	case VFmt_Byte4_UInt: return MTLVertexFormatUChar4;
	}
	return MTLVertexFormatFloat4;
}

static MTLColorWriteMask ToMtlWriteMask(int mask)
{
	MTLColorWriteMask result = MTLColorWriteMaskNone;
	if (mask & 1) result |= MTLColorWriteMaskRed;
	if (mask & 2) result |= MTLColorWriteMaskGreen;
	if (mask & 4) result |= MTLColorWriteMaskBlue;
	if (mask & 8) result |= MTLColorWriteMaskAlpha;
	return result;
}

/////////////////////////////////////////////////////////////////////////////

MtlRenderState::MtlRenderState(MetalRenderDevice *fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb)
{
	Reset();
}

MTLRenderPipelineDescriptor *MtlRenderState::CreatePipelineDescriptor(const MtlPipelineKey &key)
{
	MtlShaderProgram *program = key.Program;
	const MtlVertexFormat &vfmt = *fb->GetBufferManager()->GetVertexFormat(key.VertexFormat);

	MTLVertexDescriptor *vertexDesc = [MTLVertexDescriptor vertexDescriptor];
	for (int i = 0; i < vfmt.NumBindingPoints; i++)
		vertexDesc.layouts[MTLBUF_VERTEX0 + i].stride = vfmt.Stride;

	bool inputLocations[VATTR_MAX] = {};
	for (const auto &attr : vfmt.Attrs)
	{
		vertexDesc.attributes[attr.location].format = ToMtlVertexFormat(attr.format);
		vertexDesc.attributes[attr.location].offset = attr.offset;
		vertexDesc.attributes[attr.location].bufferIndex = MTLBUF_VERTEX0 + attr.binding;
		inputLocations[attr.location] = true;
	}
	// The scene shaders declare every attribute; unused ones read zeroed data.
	for (int i = 0; i < VATTR_MAX; i++)
	{
		if (!inputLocations[i])
		{
			vertexDesc.attributes[i].format = i != VATTR_BONESELECTOR ? MTLVertexFormatFloat3 : MTLVertexFormatUChar4;
			vertexDesc.attributes[i].offset = 0;
			vertexDesc.attributes[i].bufferIndex = MTLBUF_VERTEX0;
		}
	}

	MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
	desc.vertexFunction = program->vert;
	desc.fragmentFunction = program->frag;
	desc.vertexDescriptor = vertexDesc;

	static const MTLPixelFormat gbufferFormats[] = { MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm, MtlRenderBuffers::SceneNormalFormat };
	for (int i = 0; i < key.DrawBuffers; i++)
	{
		auto attachment = desc.colorAttachments[i];
		attachment.pixelFormat = i == 0 ? key.ColorFormat : gbufferFormats[i];
		attachment.writeMask = ToMtlWriteMask(key.ColorMask);
		MtlSetBlendMode(attachment, key.RenderStyle);
	}
	if (key.DepthStencil)
	{
		desc.depthAttachmentPixelFormat = MtlRenderBuffers::DepthStencilFormat;
		desc.stencilAttachmentPixelFormat = MtlRenderBuffers::DepthStencilFormat;
	}
	desc.rasterSampleCount = key.Samples;
	return desc;
}

id<MTLRenderPipelineState> MtlRenderState::GetPipeline(const MtlPipelineKey &key)
{
	auto &item = mPipelines[key];
	if (item)
		return item;

	// Take what the prewarm has finished since the last miss.
	{
		std::lock_guard<std::mutex> lock(mPrewarm->Mutex);
		for (auto &done : mPrewarm->Done)
		{
			auto &slot = mPipelines[done.first];
			if (!slot)
				slot = done.second;
		}
		mPrewarm->Done.clear();
	}
	if (item)
		return item;

	NSError *error = nil;
	item = [fb->device newRenderPipelineStateWithDescriptor:CreatePipelineDescriptor(key) error:&error];
	if (!item)
		I_FatalError("Metal: could not create pipeline for %s: %s", key.Program->frag.label.UTF8String, error.localizedDescription.UTF8String);
	return item;
}

// Creating a pipeline compiles its shaders for the vertex layout, attachments
// and blend state. Until the system shader cache has it, that takes up to
// hundreds of milliseconds, so the first wall, sky, sprite or particle drawn
// with a new combination would stall its frame. The common scene combinations
// are queued here and compiled on Metal's compiler threads instead.
void MtlRenderState::QueueScenePipelines(MtlShaderManager *shaders, const PrewarmFormats &formats, int samples)
{
	FRenderStyle opaque;
	opaque.BlendOp = STYLEOP_Add;
	opaque.SrcAlpha = STYLEALPHA_One;
	opaque.DestAlpha = STYLEALPHA_Zero;
	opaque.Flags = 0;
	FRenderStyle opaqueAlpha1 = opaque;
	opaqueAlpha1.Flags = STYLEF_Alpha1;
	const FRenderStyle translucent = LegacyRenderStyles[STYLE_Translucent], normal = LegacyRenderStyles[STYLE_Normal], additive = LegacyRenderStyles[STYLE_Add];

	auto queue = [&](MtlShaderProgram *program, int format, const FRenderStyle &style, int drawBuffers, int colorMask = 15)
	{
		if (!program || !program->vert || format < 0)
			return;
		MtlPipelineKey key;
		memset(&key, 0, sizeof(key));
		key.Program = program;
		key.RenderStyle = style;
		key.VertexFormat = format;
		key.ColorMask = colorMask;
		key.DrawBuffers = drawBuffers;
		key.ColorFormat = MTLPixelFormatRGBA16Float;
		key.DepthStencil = 1;
		key.Samples = samples;
		if (!mPipelines.count(key) && mPrewarmRequested.insert(key).second)
			mPrewarmQueue.push_back(key);
	};

	// The scene, with and without the G-buffer targets that SSAO and the modern effects use.
	for (int drawBuffers : { 3, 1 })
	{
		EPassType passType = drawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS;
		for (bool alphatest : { false, true })
		{
			for (const auto &style : { opaque, opaqueAlpha1, translucent, normal })
				queue(shaders->Get(0, alphatest, passType), formats.flat, style, drawBuffers);
		}
		// Warped (liquid) flats
		for (int effect = 1; effect < SHADER_NoTexture; effect++)
			queue(shaders->Get(effect, false, passType), formats.flat, opaque, drawBuffers);
		queue(shaders->GetEffect(EFF_STENCIL, passType), formats.flat, opaque, drawBuffers, 8);
		queue(shaders->Get(SHADER_NoTexture, true, passType), formats.sky, translucent, drawBuffers);
		for (bool alphatest : { false, true })
		{
			queue(shaders->Get(0, alphatest, passType), formats.sky, translucent, drawBuffers);
			queue(shaders->Get(0, alphatest, passType), formats.particles, translucent, drawBuffers);
			queue(shaders->Get(0, alphatest, passType), formats.particles, additive, drawBuffers);
		}
	}
}

void MtlRenderState::PumpPrewarm(int maxPending)
{
	while (!mPrewarmQueue.empty())
	{
		{
			std::lock_guard<std::mutex> lock(mPrewarm->Mutex);
			if (mPrewarm->Pending >= maxPending)
				return;
			mPrewarm->Pending++;
		}
		MtlPipelineKey key = mPrewarmQueue.front();
		mPrewarmQueue.pop_front();
		if (mPipelines.count(key))
		{
			std::lock_guard<std::mutex> lock(mPrewarm->Mutex);
			mPrewarm->Pending--;
			continue;
		}
		std::shared_ptr<MtlPipelinePrewarm> prewarm = mPrewarm;
		[fb->device newRenderPipelineStateWithDescriptor:CreatePipelineDescriptor(key) completionHandler:^(id<MTLRenderPipelineState> pipeline, NSError *error) {
			std::lock_guard<std::mutex> lock(prewarm->Mutex);
			if (pipeline)	// failures are reported when the pipeline is used
				prewarm->Done.push_back({ key, pipeline });
			prewarm->Pending--;
			prewarm->Changed.notify_all();
		}];
	}
}

void MtlRenderState::FinishPrewarm()
{
	PumpPrewarm(INT_MAX);
	std::unique_lock<std::mutex> lock(mPrewarm->Mutex);
	mPrewarm->Changed.wait(lock, [&] { return mPrewarm->Pending == 0; });
}

id<MTLDepthStencilState> MtlRenderState::GetDepthStencilState(const MtlDepthStencilKey &key)
{
	auto &item = mDepthStencilStates[key];
	if (item)
		return item;

	static const MTLCompareFunction depthfunc[] = { MTLCompareFunctionLess, MTLCompareFunctionLessEqual, MTLCompareFunctionAlways };
	static const MTLStencilOperation stencilop[] = { MTLStencilOperationKeep, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp };

	MTLDepthStencilDescriptor *desc = [MTLDepthStencilDescriptor new];
	desc.depthCompareFunction = key.DepthTest ? depthfunc[key.DepthFunc] : MTLCompareFunctionAlways;
	desc.depthWriteEnabled = key.DepthWrite ? YES : NO;
	if (key.StencilTest)
	{
		MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
		stencil.stencilCompareFunction = MTLCompareFunctionEqual;
		stencil.stencilFailureOperation = MTLStencilOperationKeep;
		stencil.depthFailureOperation = MTLStencilOperationKeep;
		stencil.depthStencilPassOperation = stencilop[key.StencilOp];
		desc.frontFaceStencil = stencil;
		desc.backFaceStencil = stencil;
	}
	item = [fb->device newDepthStencilStateWithDescriptor:desc];
	return item;
}

void MtlRenderState::ClearScreen()
{
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	Draw(DT_TriangleStrip, FFlatVertexBuffer::FULLSCREEN_INDEX, 4, true);
}

static const MTLPrimitiveType primitiveTypes[] =
{
	MTLPrimitiveTypePoint,
	MTLPrimitiveTypeLine,
	MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypeTriangle,	// fans are drawn indexed
	MTLPrimitiveTypeTriangleStrip,
};

void MtlRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);
	if (!mEncoder)
		return;

	if (dt == DT_TriangleFan)
	{
		if (count < 3)
			return;
		// Shift the vertex binding to the fan's first vertex so the shared index list starts at zero.
		ApplyVertexBuffers(index);
		auto indexBuffer = static_cast<MtlHardwareIndexBuffer*>(fb->GetBufferManager()->FanToTrisIndexBuffer.get());
		[mEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(count - 2) * 3 indexType:MTLIndexTypeUInt32 indexBuffer:indexBuffer->mBuffer indexBufferOffset:0];
		mLastVertexBuffer = nullptr;
	}
	else
	{
		[mEncoder drawPrimitives:primitiveTypes[dt] vertexStart:index vertexCount:count];
	}
}

void MtlRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (apply || mNeedApply)
		Apply(dt);
	if (!mEncoder || !mIndexBuffer)
		return;

	auto indexBuffer = static_cast<MtlHardwareIndexBuffer*>(mIndexBuffer);
	[mEncoder drawIndexedPrimitives:primitiveTypes[dt] indexCount:count indexType:MTLIndexTypeUInt32 indexBuffer:indexBuffer->mBuffer indexBufferOffset:index * sizeof(uint32_t)];
}

bool MtlRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mRasterChanged = true;
	mNeedApply = true;
	return lastValue;
}

void MtlRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void MtlRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void MtlRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void MtlRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	int rr = r, gg = g, bb = b, aa = a;
	mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
	mNeedApply = true;
}

void MtlRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = screen->stencilValue + offs;
	mStencilRefChanged = true;
	mStencilOp = op;

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
		mDepthWrite = !(flags & SF_DepthMaskOff);
	}
	mNeedApply = true;
}

void MtlRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mRasterChanged = true;
	mNeedApply = true;
}

void MtlRenderState::EnableClipDistance(int num, bool state)
{
}

void MtlRenderState::Clear(int targets)
{
	mClearTargets = targets;
	EndRenderPass();
}

void MtlRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void MtlRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
	mNeedApply = true;
}

void MtlRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
	mNeedApply = true;
}

void MtlRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void MtlRenderState::EnableMultisampling(bool on)
{
}

void MtlRenderState::EnableLineSmooth(bool on)
{
}

void MtlRenderState::EnableDrawBuffers(int count, bool apply)
{
	if (mRenderTarget.DrawBuffers != count)
	{
		EndRenderPass();
		mRenderTarget.DrawBuffers = count;
	}
}

void MtlRenderState::Apply(int dt)
{
	drawcalls.Clock();

	// Writing the uniform blocks may have to wait for the GPU, which ends the render pass.
	ApplyStreamData();
	ApplyMatrices();

	if (!mEncoder)
		BeginRenderPass();
	if (mEncoder)
	{
		ApplyPipeline();
		ApplyDynamicState();
		ApplyPushConstants();
		ApplyBuffers();
		ApplyVertexBuffers(0);
		ApplyMaterial();
	}
	mNeedApply = false;

	drawcalls.Unclock();
}

void MtlRenderState::BeginRenderPass()
{
	if (!mRenderTarget.Image || !mRenderTarget.Image->Texture)
		return;

	auto buffers = fb->GetBuffers();
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];

	auto color = pass.colorAttachments[0];
	color.texture = mRenderTarget.Image->Texture;
	color.loadAction = (mClearTargets & CT_Color) ? MTLLoadActionClear : MTLLoadActionLoad;
	color.storeAction = MTLStoreActionStore;
	color.clearColor = MTLClearColorMake(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], screen->mSceneClearColor[3]);

	if (mRenderTarget.DrawBuffers > 1)
	{
		id<MTLTexture> extra[2] = { buffers->SceneFog.Texture, buffers->SceneNormal.Texture };
		for (int i = 1; i < mRenderTarget.DrawBuffers && i < 3; i++)
		{
			auto attachment = pass.colorAttachments[i];
			attachment.texture = extra[i - 1];
			attachment.loadAction = (mClearTargets & CT_Color) ? MTLLoadActionClear : MTLLoadActionLoad;
			attachment.storeAction = MTLStoreActionStore;
			attachment.clearColor = MTLClearColorMake(0, 0, 0, 0);
		}
	}

	if (mRenderTarget.DepthStencil)
	{
		pass.depthAttachment.texture = mRenderTarget.DepthStencil;
		pass.depthAttachment.loadAction = (mClearTargets & CT_Depth) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = 1.0;
		pass.stencilAttachment.texture = mRenderTarget.DepthStencil;
		pass.stencilAttachment.loadAction = (mClearTargets & CT_Stencil) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.clearStencil = 0;
	}

	mEncoder = [fb->GetDrawCommands() renderCommandEncoderWithDescriptor:pass];

	if (fb->RaytracingEnabled() && fb->GetRaytrace()->TopLevel)
	{
		auto raytrace = fb->GetRaytrace();
		[mEncoder setFragmentAccelerationStructure:raytrace->TopLevel atBufferIndex:MTLBUF_ACCELERATION];
		[mEncoder useResource:raytrace->BottomLevel usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
	}

	mScissorChanged = true;
	mViewportChanged = true;
	mStencilRefChanged = true;
	mRasterChanged = true;
	mBias.mChanged = true;
	mHasPipeline = false;
	mHasDepthStencilState = false;
	mLastViewpointOffset = 0xffffffff;
	mLastMatricesOffset = 0xffffffff;
	mLastStreamDataOffset = 0xffffffff;
	mLastLightBuffer = nil;
	mLastBoneBuffer = nil;
	mLastVertexBuffer = nullptr;
	mMaterial.mChanged = true;
	mClearTargets = 0;
}

void MtlRenderState::ApplyPipeline()
{
	EPassType passType = mRenderTarget.DrawBuffers > 1 ? GBUFFER_PASS : NORMAL_PASS;
	auto shaders = fb->GetShaderManager();
	MtlShaderProgram *program;
	if (mSpecialEffect > EFF_NONE)
	{
		program = shaders->GetEffect(mSpecialEffect, passType);
	}
	else
	{
		int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
		effectState = mTextureEnabled ? effectState : SHADER_NoTexture;
		if (r_skipmats && effectState >= 3 && effectState <= 4)
			effectState = 0;
		program = shaders->Get(effectState, mAlphaThreshold >= 0.f, passType);
	}
	if (!program || !program->vert)
		program = shaders->Get(0, true, passType);

	MtlPipelineKey key;
	memset(&key, 0, sizeof(key));
	key.Program = program;
	key.RenderStyle = mRenderStyle;
	key.VertexFormat = static_cast<MtlHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat;
	key.ColorMask = mColorMask;
	key.DrawBuffers = mRenderTarget.DrawBuffers;
	key.ColorFormat = mRenderTarget.Format;
	key.DepthStencil = mRenderTarget.DepthStencil ? 1 : 0;
	key.Samples = mRenderTarget.Samples;
	if (!mHasPipeline || key != mPipelineKey)
	{
		[mEncoder setRenderPipelineState:GetPipeline(key)];
		mPipelineKey = key;
		mHasPipeline = true;
	}

	MtlDepthStencilKey dskey;
	memset(&dskey, 0, sizeof(dskey));
	if (mRenderTarget.DepthStencil)
	{
		dskey.DepthTest = mDepthTest;
		dskey.DepthWrite = mDepthTest && mDepthWrite;
		dskey.DepthFunc = mDepthFunc;
		dskey.StencilTest = mStencilTest;
		dskey.StencilOp = mStencilOp;
	}
	if (!mHasDepthStencilState || dskey != mDepthStencilKey)
	{
		[mEncoder setDepthStencilState:GetDepthStencilState(dskey)];
		mDepthStencilKey = dskey;
		mHasDepthStencilState = true;
	}
}

void MtlRenderState::ApplyDynamicState()
{
	if (mScissorChanged)
	{
		MTLScissorRect scissor;
		if (mScissorWidth >= 0)
		{
			int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
			int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
			int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
			int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);
			scissor = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
		}
		else
		{
			scissor = { 0, 0, (NSUInteger)mRenderTarget.Width, (NSUInteger)mRenderTarget.Height };
		}
		[mEncoder setScissorRect:scissor];
		mScissorChanged = false;
	}

	if (mViewportChanged)
	{
		MTLViewport viewport;
		if (mViewportWidth >= 0)
			viewport = { (double)mViewportX, (double)mViewportY, (double)mViewportWidth, (double)mViewportHeight, mViewportDepthMin, mViewportDepthMax };
		else
			viewport = { 0.0, 0.0, (double)mRenderTarget.Width, (double)mRenderTarget.Height, mViewportDepthMin, mViewportDepthMax };
		[mEncoder setViewport:viewport];
		mViewportChanged = false;
	}

	if (mStencilRefChanged)
	{
		[mEncoder setStencilReferenceValue:mStencilRef];
		mStencilRefChanged = false;
	}

	if (mBias.mChanged)
	{
		[mEncoder setDepthBias:mBias.mUnits slopeScale:mBias.mFactor clamp:0.0f];
		mBias.mChanged = false;
	}

	if (mRasterChanged)
	{
		[mEncoder setCullMode:mCullMode == Cull_None ? MTLCullModeNone : MTLCullModeBack];
		[mEncoder setFrontFacingWinding:mCullMode == Cull_CW ? MTLWindingCounterClockwise : MTLWindingClockwise];
		[mEncoder setDepthClipMode:mDepthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
		mRasterChanged = false;
	}
}

void MtlRenderState::ApplyStreamData()
{
	auto vbuf = static_cast<MtlHardwareVertexBuffer*>(mVertexBuffer);
	mStreamData.useVertexData = vbuf ? fb->GetBufferManager()->GetVertexFormat(vbuf->VertexFormat)->UseVertexData : 0;

	if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
		mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	else
		mStreamData.timer = 0.0f;

	if (!mStreamBufferWriter.Write(mStreamData))
	{
		WaitForStreamBuffers();
		mStreamBufferWriter.Write(mStreamData);
	}
}

void MtlRenderState::ApplyMatrices()
{
	if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled))
	{
		WaitForStreamBuffers();
		mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled);
	}
}

void MtlRenderState::ApplyPushConstants()
{
	int fogset = 0;
	if (mFogEnabled)
	{
		if (mFogEnabled == 2)
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		else if ((GetFogColor() & 0xffffff) == 0)
			fogset = gl_fogmode;
		else
			fogset = -gl_fogmode;
	}

	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
		tempTM = TM_OPAQUE;

	mPushConstants.uFogEnabled = fogset;
	mPushConstants.uTextureMode = GetTextureModeAndFlags(tempTM);
	mPushConstants.uLightDist = mLightParms[0];
	mPushConstants.uLightFactor = mLightParms[1];
	mPushConstants.uFogDensity = mLightParms[2];
	mPushConstants.uLightLevel = mLightParms[3];
	mPushConstants.uAlphaThreshold = mAlphaThreshold;
	mPushConstants.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	if (mMaterial.mMaterial)
	{
		auto source = mMaterial.mMaterial->Source();
		mPushConstants.uSpecularMaterial = { source->GetGlossiness(), source->GetSpecularLevel() };
	}

	mPushConstants.uLightIndex = mLightIndex;
	mPushConstants.uBoneIndexBase = mBoneIndexBase;
	mPushConstants.uDataIndex = mStreamBufferWriter.DataIndex();

	[mEncoder setVertexBytes:&mPushConstants length:sizeof(PushConstants) atIndex:MTLBUF_PUSHCONSTANTS];
	[mEncoder setFragmentBytes:&mPushConstants length:sizeof(PushConstants) atIndex:MTLBUF_PUSHCONSTANTS];
}

void MtlRenderState::ApplyBuffers()
{
	auto buffers = fb->GetBufferManager();

	if (mViewpointOffset != mLastViewpointOffset && buffers->ViewpointUBO)
	{
		id<MTLBuffer> buffer = buffers->ViewpointUBO->mBuffer;
		[mEncoder setVertexBuffer:buffer offset:mViewpointOffset atIndex:MTLBUF_VIEWPOINT];
		[mEncoder setFragmentBuffer:buffer offset:mViewpointOffset atIndex:MTLBUF_VIEWPOINT];
		mLastViewpointOffset = mViewpointOffset;
	}

	uint32_t matrixOffset = mMatrixBufferWriter.Offset();
	if (matrixOffset != mLastMatricesOffset)
	{
		id<MTLBuffer> buffer = buffers->MatrixBuffer->UniformBuffer->mBuffer;
		[mEncoder setVertexBuffer:buffer offset:matrixOffset atIndex:MTLBUF_MATRICES];
		[mEncoder setFragmentBuffer:buffer offset:matrixOffset atIndex:MTLBUF_MATRICES];
		mLastMatricesOffset = matrixOffset;
	}

	uint32_t streamOffset = mStreamBufferWriter.StreamDataOffset();
	if (streamOffset != mLastStreamDataOffset)
	{
		id<MTLBuffer> buffer = buffers->StreamBuffer->UniformBuffer->mBuffer;
		[mEncoder setVertexBuffer:buffer offset:streamOffset atIndex:MTLBUF_STREAM];
		[mEncoder setFragmentBuffer:buffer offset:streamOffset atIndex:MTLBUF_STREAM];
		mLastStreamDataOffset = streamOffset;
	}

	if (buffers->LightBufferSSO && buffers->LightBufferSSO->mBuffer != mLastLightBuffer)
	{
		mLastLightBuffer = buffers->LightBufferSSO->mBuffer;
		[mEncoder setVertexBuffer:mLastLightBuffer offset:0 atIndex:MTLBUF_LIGHTS];
		[mEncoder setFragmentBuffer:mLastLightBuffer offset:0 atIndex:MTLBUF_LIGHTS];
	}

	if (buffers->BoneBufferSSO && buffers->BoneBufferSSO->mBuffer != mLastBoneBuffer)
	{
		mLastBoneBuffer = buffers->BoneBufferSSO->mBuffer;
		[mEncoder setVertexBuffer:mLastBoneBuffer offset:0 atIndex:MTLBUF_BONES];
		[mEncoder setFragmentBuffer:mLastBoneBuffer offset:0 atIndex:MTLBUF_BONES];
	}
}

void MtlRenderState::ApplyVertexBuffers(int extraVertexOffset)
{
	if (!mVertexBuffer)
		return;
	if (extraVertexOffset == 0 && mVertexBuffer == mLastVertexBuffer && mVertexOffsets[0] == mLastVertexOffsets[0] && mVertexOffsets[1] == mLastVertexOffsets[1])
		return;

	auto vbuf = static_cast<MtlHardwareVertexBuffer*>(mVertexBuffer);
	const MtlVertexFormat *format = fb->GetBufferManager()->GetVertexFormat(vbuf->VertexFormat);
	[mEncoder setVertexBuffer:vbuf->mBuffer offset:(mVertexOffsets[0] + extraVertexOffset) * format->Stride atIndex:MTLBUF_VERTEX0];
	[mEncoder setVertexBuffer:vbuf->mBuffer offset:(mVertexOffsets[1] + extraVertexOffset) * format->Stride atIndex:MTLBUF_VERTEX1];

	mLastVertexBuffer = mVertexBuffer;
	mLastVertexOffsets[0] = mVertexOffsets[0];
	mLastVertexOffsets[1] = mVertexOffsets[1];
}

void MtlRenderState::ApplyMaterial()
{
	if (!mMaterial.mChanged)
		return;

	auto textures = fb->GetTextureManager();
	auto samplers = fb->GetSamplerManager();

	id<MTLTexture> bound[MTLTEX_SCENE_COUNT];
	id<MTLSamplerState> boundSamplers[MTLTEX_IRRADIANCE + 1];

	if (mMaterial.mMaterial)
	{
		if (mMaterial.mMaterial->Source()->isHardwareCanvas())
			static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();

		auto binding = static_cast<MtlMaterial*>(mMaterial.mMaterial)->GetBinding(mMaterial);
		for (int i = 0; i < MTLTEX_MATERIAL_COUNT; i++)
		{
			bound[i] = binding->textures[i] ? binding->textures[i] : textures->NullTexture;
			boundSamplers[i] = binding->sampler;
		}
	}
	else
	{
		for (int i = 0; i < MTLTEX_MATERIAL_COUNT; i++)
		{
			bound[i] = textures->NullTexture;
			boundSamplers[i] = samplers->Get(CLAMP_XY_NOMIP);
		}
	}

	bound[MTLTEX_SHADOWMAP] = textures->Shadowmap.Texture;
	boundSamplers[MTLTEX_SHADOWMAP] = samplers->ShadowmapSampler;
	bound[MTLTEX_LIGHTMAP] = textures->Lightmap.Texture;
	boundSamplers[MTLTEX_LIGHTMAP] = samplers->LightmapSampler;
	bound[MTLTEX_IRRADIANCE] = textures->Irradiance.Texture;
	boundSamplers[MTLTEX_IRRADIANCE] = samplers->LightmapSampler;
	bound[MTLTEX_WEATHER] = textures->WeatherMap.Texture;
	bound[MTLTEX_SCENEDEPTH] = textures->SceneDepth ? textures->SceneDepth : textures->NullTexture;

	[mEncoder setFragmentTextures:bound withRange:NSMakeRange(0, MTLTEX_SCENE_COUNT)];
	[mEncoder setFragmentSamplerStates:boundSamplers withRange:NSMakeRange(0, MTLTEX_IRRADIANCE + 1)];
	// GPU particles and weather sample the irradiance and sky exposure in the vertex stage.
	[mEncoder setVertexTextures:&bound[MTLTEX_IRRADIANCE] withRange:NSMakeRange(MTLTEX_IRRADIANCE, 2)];
	[mEncoder setVertexSamplerState:boundSamplers[MTLTEX_IRRADIANCE] atIndex:MTLTEX_IRRADIANCE];
	mMaterial.mChanged = false;
}

void MtlRenderState::WaitForStreamBuffers()
{
	fb->WaitForCommands(false);
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

void MtlRenderState::Bind(int bindingpoint, uint32_t offset)
{
	if (bindingpoint == VIEWPOINT_BINDINGPOINT)
	{
		mViewpointOffset = offset;
		mNeedApply = true;
	}
}

void MtlRenderState::BeginFrame()
{
	mMaterial.Reset();
}

void MtlRenderState::EndRenderPass()
{
	if (mEncoder)
	{
		[mEncoder endEncoding];
		mEncoder = nil;
	}
}

void MtlRenderState::EndFrame()
{
	mMatrixBufferWriter.Reset();
	mStreamBufferWriter.Reset();
}

void MtlRenderState::SetRenderTarget(MtlTextureImage *image, id<MTLTexture> depthStencil, int width, int height, MTLPixelFormat format, int samples)
{
	EndRenderPass();
	mRenderTarget.Samples = samples;
	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencil;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
}
