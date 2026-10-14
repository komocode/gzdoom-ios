/*
** mtl_postprocess.mm
** Postprocess passes for the Metal backend. Each PPRenderState::Draw is one
** fullscreen triangle into its own render pass.
*/

#include "mtl_postprocess.h"
#include "mtl_renderdevice.h"
#include "mtl_renderstate.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hw_vrmodes.h"
#include "flatvertices.h"
#include "r_videoscale.h"

EXTERN_CVAR(Int, gl_dither_bpc)

void MtlPPRenderState::PushGroup(const FString &name)
{
}

void MtlPPRenderState::PopGroup()
{
}

bool MtlPPRenderState::SupportsRayQuery() const
{
	return fb->RaytracingEnabled() && fb->GetRaytrace()->TopLevel;
}

void MtlPPRenderState::Draw()
{
	auto pp = fb->GetPostprocess();
	auto textures = fb->GetTextureManager();
	fb->GetRenderState()->EndRenderPass();

	// Output target
	id<MTLTexture> output = nil;
	MTLPixelFormat format;
	if (Output.Type == PPTextureType::SwapChain)
	{
		output = fb->GetDrawableTexture();
		if (!output)
			return;
		format = output.pixelFormat;
	}
	else
	{
		MtlTextureImage *image = textures->GetTexture(Output.Type, Output.Texture);
		output = image ? image->Texture : nil;
		if (!output)
			return;
		format = output.pixelFormat;
	}

	bool stencilTest = Output.Type == PPTextureType::SceneColor;

	MtlPostprocess::PipelineKey key;
	memset(&key, 0, sizeof(key));
	key.Shader = fb->GetShaderManager()->GetPPShader(Shader);
	key.BlendMode = BlendMode;
	key.OutputFormat = format;
	key.StencilTest = stencilTest;
	key.Samples = (int)output.sampleCount;
	id<MTLRenderPipelineState> pipeline = pp->GetPipeline(key);

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = output;
	pass.colorAttachments[0].loadAction = Output.Type == PPTextureType::SwapChain ? MTLLoadActionClear : MTLLoadActionLoad;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	if (stencilTest)
	{
		id<MTLTexture> depthStencil = fb->GetBuffers()->SceneDepthStencil.Texture;
		pass.depthAttachment.texture = depthStencil;
		pass.depthAttachment.loadAction = MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.texture = depthStencil;
		pass.stencilAttachment.loadAction = MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
	}

	id<MTLRenderCommandEncoder> encoder = [fb->GetDrawCommands() renderCommandEncoderWithDescriptor:pass];
	[encoder setRenderPipelineState:pipeline];
	if (stencilTest)
	{
		[encoder setDepthStencilState:pp->mStencilEqual];
		[encoder setStencilReferenceValue:screen->stencilValue];
	}

	MTLViewport viewport = { (double)Viewport.left, (double)Viewport.top, (double)Viewport.width, (double)Viewport.height, 0.0, 1.0 };
	[encoder setViewport:viewport];

	for (unsigned int index = 0; index < Textures.Size(); index++)
	{
		const PPTextureInput &input = Textures[index];
		MtlTextureImage *tex = textures->GetTexture(input.Type, input.Texture);
		[encoder setFragmentTexture:tex ? tex->Texture : textures->NullTexture atIndex:index];
		[encoder setFragmentSamplerState:fb->GetSamplerManager()->Get(input.Filter, input.Wrap) atIndex:index];
	}

	if (ShadowMapBuffers)
	{
		auto buffers = fb->GetBufferManager();
		if (buffers->LightNodes) [encoder setFragmentBuffer:buffers->LightNodes->mBuffer offset:0 atIndex:MTLBUF_PP_STORAGE + LIGHTNODES_BINDINGPOINT];
		if (buffers->LightLines) [encoder setFragmentBuffer:buffers->LightLines->mBuffer offset:0 atIndex:MTLBUF_PP_STORAGE + LIGHTLINES_BINDINGPOINT];
		if (buffers->LightList) [encoder setFragmentBuffer:buffers->LightList->mBuffer offset:0 atIndex:MTLBUF_PP_STORAGE + LIGHTLIST_BINDINGPOINT];
	}

	if (Uniforms.Data.Size() > 0)
		[encoder setFragmentBytes:Uniforms.Data.Data() length:Uniforms.Data.Size() atIndex:MTLBUF_PP_UNIFORMS];

	// Small per-draw data needs no buffer of its own (Metal allows up to 4 KB).
	if (StorageData && StorageSize > 0)
	{
		assert(StorageSize <= 4096);
		[encoder setFragmentBytes:StorageData length:std::min(StorageSize, 4096) atIndex:MTLBUF_PP_STORAGE + PP_STORAGE_BINDINGPOINT];
	}

	if (RayQuery && SupportsRayQuery())
	{
		auto raytrace = fb->GetRaytrace();
		[encoder setFragmentAccelerationStructure:raytrace->TopLevel atBufferIndex:MTLBUF_PP_STORAGE + PP_ACCELERATION_BINDINGPOINT];
		[encoder useResource:raytrace->BottomLevel usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
	}

	auto vertexBuffer = static_cast<MtlHardwareVertexBuffer*>(screen->mVertexData->GetBufferObjects().first);
	[encoder setVertexBuffer:vertexBuffer->mBuffer offset:0 atIndex:MTLBUF_PP_VERTEX];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:FFlatVertexBuffer::PRESENT_INDEX vertexCount:3];
	[encoder endEncoding];

	// Advance to the next pipeline image if our output was sent there
	if (Output.Type == PPTextureType::NextPipelineTexture)
		pp->mCurrentPipelineImage = (pp->mCurrentPipelineImage + 1) % MtlRenderBuffers::NumPipelineImages;
}

/////////////////////////////////////////////////////////////////////////////

id<MTLRenderPipelineState> MtlPostprocess::GetPipeline(const PipelineKey &key)
{
	auto &item = mPipelines[key];
	if (item)
		return item;

	MTLVertexDescriptor *vertexDesc = [MTLVertexDescriptor vertexDescriptor];
	vertexDesc.layouts[MTLBUF_PP_VERTEX].stride = sizeof(FFlatVertex);
	vertexDesc.attributes[0].format = MTLVertexFormatFloat3;
	vertexDesc.attributes[0].offset = offsetof(FFlatVertex, x);
	vertexDesc.attributes[0].bufferIndex = MTLBUF_PP_VERTEX;
	vertexDesc.attributes[1].format = MTLVertexFormatFloat2;
	vertexDesc.attributes[1].offset = offsetof(FFlatVertex, u);
	vertexDesc.attributes[1].bufferIndex = MTLBUF_PP_VERTEX;

	MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
	desc.vertexFunction = key.Shader->VertexFunction;
	desc.fragmentFunction = key.Shader->FragmentFunction;
	desc.vertexDescriptor = vertexDesc;
	desc.colorAttachments[0].pixelFormat = key.OutputFormat;
	MtlSetBlendMode(desc.colorAttachments[0], key.BlendMode);
	if (key.StencilTest)
	{
		desc.depthAttachmentPixelFormat = MtlRenderBuffers::DepthStencilFormat;
		desc.stencilAttachmentPixelFormat = MtlRenderBuffers::DepthStencilFormat;
	}
	desc.rasterSampleCount = key.Samples;

	NSError *error = nil;
	item = [fb->device newRenderPipelineStateWithDescriptor:desc error:&error];
	if (!item)
		I_FatalError("Metal: could not create postprocess pipeline for %s: %s", key.Shader->FragmentFunction.label.UTF8String, error.localizedDescription.UTF8String);

	if (key.StencilTest && !mStencilEqual)
	{
		MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
		stencil.stencilCompareFunction = MTLCompareFunctionEqual;
		MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
		ds.frontFaceStencil = stencil;
		ds.backFaceStencil = stencil;
		mStencilEqual = [fb->device newDepthStencilStateWithDescriptor:ds];
	}
	return item;
}

void MtlPostprocess::SetActiveRenderTarget()
{
	auto buffers = fb->GetBuffers();
	fb->GetRenderState()->SetRenderTarget(&buffers->PipelineImage[mCurrentPipelineImage], buffers->PipelineDepthStencil.Texture, buffers->GetWidth(), buffers->GetHeight(), MTLPixelFormatRGBA16Float);
}

void MtlPostprocess::PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	MtlPPRenderState renderstate(fb);

	hw_postprocess.Pass1(&renderstate, fixedcm, sceneWidth, sceneHeight, [&]() {
		SetActiveRenderTarget();
		afterBloomDrawEndScene2D();
	});
	hw_postprocess.Pass2(&renderstate, fixedcm, flash, sceneWidth, sceneHeight);
}

static void CopyTexture(MetalRenderDevice *fb, id<MTLTexture> src, id<MTLTexture> dst)
{
	if (!src || !dst)
		return;
	fb->GetRenderState()->EndRenderPass();
	id<MTLBlitCommandEncoder> blit = [fb->GetDrawCommands() blitCommandEncoder];
	NSUInteger w = std::min(src.width, dst.width);
	NSUInteger h = std::min(src.height, dst.height);
	[blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
}

void MtlPostprocess::BlitSceneToPostprocess()
{
	mCurrentPipelineImage = 0;
	auto buffers = fb->GetBuffers();
	if (buffers->GetSceneSamples() > 1)
	{
		// Resolve with an empty render pass.
		fb->GetRenderState()->EndRenderPass();
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = buffers->SceneColor.Texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
		pass.colorAttachments[0].resolveTexture = buffers->PipelineImage[mCurrentPipelineImage].Texture;
		[[fb->GetDrawCommands() renderCommandEncoderWithDescriptor:pass] endEncoding];
	}
	else
	{
		CopyTexture(fb, buffers->SceneColor.Texture, buffers->PipelineImage[mCurrentPipelineImage].Texture);
	}
}

void MtlPostprocess::BlitCurrentToImage(MtlTextureImage *image)
{
	CopyTexture(fb, fb->GetBuffers()->PipelineImage[mCurrentPipelineImage].Texture, image->Texture);
}

void MtlPostprocess::DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot)
{
	MtlPPRenderState renderstate(fb);

	if (!screenshot) // Already applied as we are actually copying the last frame here (GetScreenshotBuffer is called after swap)
		hw_postprocess.customShaders.Run(&renderstate, "screen");

	PresentUniforms uniforms;
	if (!applyGamma)
	{
		uniforms.InvGamma = 1.0f;
		uniforms.Contrast = 1.0f;
		uniforms.Brightness = 0.0f;
		uniforms.Saturation = 1.0f;
	}
	else
	{
		uniforms.InvGamma = 1.0f / clamp<float>(vid_gamma, 0.1f, 4.f);
		uniforms.Contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		uniforms.Brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
		uniforms.Saturation = clamp<float>(vid_saturation, -15.0f, 15.f);
		uniforms.GrayFormula = static_cast<int>(gl_satformula);
	}
	// The EDR swapchain is half float: no dither, and the present pass decodes to linear.
	bool hdr = !screenshot && fb->HdrOutputActive();
	uniforms.ColorScale = hdr ? 0.0f : (gl_dither_bpc == -1) ? 255.0f : (float)((1 << gl_dither_bpc) - 1);
	uniforms.HdrMode = hdr ? 2 : 0;

	if (screenshot)
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 0.0f };
	}
	else
	{
		uniforms.Scale = { screen->mScreenViewport.width / (float)fb->GetBuffers()->GetWidth(), -screen->mScreenViewport.height / (float)fb->GetBuffers()->GetHeight() };
		uniforms.Offset = { 0.0f, 1.0f };
	}

	renderstate.Clear();
	renderstate.Shader = &hw_postprocess.present.Present;
	renderstate.Uniforms.Set(uniforms);
	renderstate.Viewport = box;
	renderstate.SetInputCurrent(0, ViewportLinearScale() ? PPFilterMode::Linear : PPFilterMode::Nearest);
	renderstate.SetInputTexture(1, &hw_postprocess.present.Dither, PPFilterMode::Nearest, PPWrapMode::Repeat);
	if (screenshot)
		renderstate.SetOutputNext();
	else
		renderstate.SetOutputSwapChain();
	renderstate.SetNoBlend();
	renderstate.Draw();
}

void MtlPostprocess::AmbientOccludeScene(float m5)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	MtlPPRenderState renderstate(fb);
	hw_postprocess.ssao.Render(&renderstate, m5, sceneWidth, sceneHeight);

	// Soft particles read the linear depth during the rest of the scene pass;
	// the scene render pass restarts after this and rebinds the textures.
	if (PPAmbientOcclusion::GBufferNeeded())
	{
		MtlTextureImage *depth = fb->GetTextureManager()->GetTexture(PPTextureType::PPTexture, hw_postprocess.ssao.GetLinearDepth());
		fb->GetTextureManager()->SceneDepth = depth ? depth->Texture : nil;
	}
}

void MtlPostprocess::ReflectScene(float m5)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	MtlPPRenderState renderstate(fb);
	hw_postprocess.sceneEffects.RenderReflections(&renderstate, m5, sceneWidth, sceneHeight);
}

void MtlPostprocess::BlurScene(float gameinfobluramount)
{
	int sceneWidth = fb->GetBuffers()->GetSceneWidth();
	int sceneHeight = fb->GetBuffers()->GetSceneHeight();

	MtlPPRenderState renderstate(fb);
	hw_postprocess.bloom.RenderBlur(&renderstate, sceneWidth, sceneHeight, gameinfobluramount);
}

void MtlPostprocess::ClearTonemapPalette()
{
	hw_postprocess.tonemap.ClearTonemapPalette();
}

void MtlPostprocess::UpdateShadowMap()
{
	// The light list still assigns the shadow indices that mark lights as
	// shadowed; traced shadows don't need the 1D maps themselves.
	if (screen->mShadowMap.PerformUpdate())
	{
		if (!fb->RaytracingEnabled())
		{
			MtlPPRenderState renderstate(fb);
			hw_postprocess.shadowmap.Update(&renderstate);
		}
		screen->mShadowMap.FinishUpdate();
	}
}
