#pragma once

#include "mtl_objects.h"
#include <functional>

class MtlPPRenderState : public PPRenderState
{
public:
	MtlPPRenderState(MetalRenderDevice *fb) : fb(fb) { }

	void PushGroup(const FString &name) override;
	void PopGroup() override;
	void Draw() override;

	bool SupportsStorageData() const override { return true; }
	bool SupportsRayQuery() const override;

private:
	MetalRenderDevice *fb = nullptr;
};

class MtlPostprocess
{
public:
	MtlPostprocess(MetalRenderDevice *fb) : fb(fb) { }

	void SetActiveRenderTarget();
	void PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D);

	void AmbientOccludeScene(float m5);
	void ReflectScene(float m5);
	void BlurScene(float gameinfobluramount);
	void ClearTonemapPalette();
	void UpdateShadowMap();

	void BlitSceneToPostprocess();
	void BlitCurrentToImage(MtlTextureImage *image);
	void DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot);

	int GetCurrentPipelineImage() const { return mCurrentPipelineImage; }

private:
	struct PipelineKey
	{
		MtlPPShader *Shader;
		PPBlendMode BlendMode;
		MTLPixelFormat OutputFormat;
		int StencilTest;
		int Samples;

		bool operator<(const PipelineKey &other) const { return memcmp(this, &other, sizeof(PipelineKey)) < 0; }
	};

	id<MTLRenderPipelineState> GetPipeline(const PipelineKey &key);

	MetalRenderDevice *fb = nullptr;
	int mCurrentPipelineImage = 0;
	std::map<PipelineKey, id<MTLRenderPipelineState>> mPipelines;
	id<MTLDepthStencilState> mStencilEqual = nil;

	friend class MtlPPRenderState;
};
