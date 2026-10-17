#pragma once

#include "gl_sysfb.h"
#include "engineerrors.h"
#include "mtl_objects.h"
#include "common/platform/apple/apple_graphics_policy.h"
#include <condition_variable>
#include <mutex>
#include <vector>

class MtlRenderState;
class MtlPostprocess;

// GPU progress, shared with the command buffer completion handlers (which run
// on a Metal thread and may outlive the device).
struct MtlFrameSync
{
	std::mutex Mutex;
	std::condition_variable Changed;
	int Outstanding = 0;		// committed command buffers that have not completed
	uint64_t Completed = 0;		// every frame up to this one has finished
	double LastGpuTime = 0;		// seconds of GPU time of frame Completed

	struct Frame
	{
		uint64_t Serial = 0;
		int Pending = 0;
		bool Ended = false;
		double GpuTime = 0;
	};
	Frame Frames[MTL_MAX_FRAMES_IN_FLIGHT + 2];

	Frame &Get(uint64_t serial);
	void Advance();
};

class MetalRenderDevice : public SystemBaseFrameBuffer
{
	typedef SystemBaseFrameBuffer Super;

public:
	MetalRenderDevice(CAMetalLayer *layer, bool fullscreen);
	~MetalRenderDevice();

	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
	CAMetalLayer *layer = nil;

	MtlBufferManager *GetBufferManager() { return mBufferManager.get(); }
	MtlTextureManager *GetTextureManager() { return mTextureManager.get(); }
	MtlSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
	MtlShaderManager *GetShaderManager() { return mShaderManager.get(); }
	MtlRenderState *GetRenderState() { return mRenderState.get(); }
	MtlPostprocess *GetPostprocess() { return mPostprocess.get(); }
	MtlRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
	MtlRaytrace *GetRaytrace() { return mRaytrace.get(); }
	bool RaytracingEnabled() const { return mRaytracing; }
	void PrintAutoQuality() const;

	// Draw commands run after the uploads queued for the same frame.
	id<MTLCommandBuffer> GetDrawCommands();
	id<MTLBlitCommandEncoder> GetUploadEncoder();
	// Commits the commands recorded so far; wait drains the GPU completely.
	void FlushCommands(bool wait);

	// Frames in flight. The frame being recorded is CurrentFrame(); per-frame
	// buffers use FrameSlot(). DeferRelease keeps an object alive until every
	// frame recorded so far has finished on the GPU.
	uint64_t CurrentFrame() const { return mFrameSerial; }
	int FrameSlot() const { return mFrameSlot; }
	bool GpuBusy();
	void WaitForFrame(uint64_t serial);
	void DeferRelease(id object);
	id<MTLTexture> GetDrawableTexture();
	bool UnifiedMemory() const { return mUnifiedMemory; }
#if TARGET_OS_IPHONE
	MTLStorageMode UploadStorageMode() const { return MTLStorageModeShared; }
#else
	MTLStorageMode UploadStorageMode() const { return mUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged; }
#endif

	FRenderState *RenderState() override;

	bool IsVulkan() override { return true; }	// same GLSL resource model (SSBOs, push constants)
	int Backend() override { return 4; }

	void Update() override;

	void InitializeState() override;
	bool CompileNextShader() override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void UpdatePalette() override;
	const char *DeviceName() const override;
	void SetTextureFilterMode() override;
	void StartPrecaching() override;
	void BeginFrame() override;
	void InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t> &LMTextureData) override;
	void InitIrradianceVolume(int width, int height, int layers, TArray<uint16_t> &rgbaHalfData) override;
	bool InitWeatherMap(int width, int height, TArray<uint16_t> &rgbaHalfData) override;
	bool InitSunShadowMap(int width, int height, const float *depth) override;
	void PrecacheSunShaders(const TArray<int> &userShaders) override;
	bool SceneDepthReadable() override { return true; }
	void BlurScene(float amount) override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D) override;
	void AmbientOccludeScene(float m5) override;
	void ReflectScene(float m5) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetLevelMesh(hwrenderer::LevelMesh *mesh) override;
	bool RaytracedShadowsActive() override { return mRaytracing; }

	bool SupportsPlatformUpscaler() override;
	bool PlatformTemporalUpscale(PPTexture *color, PPTexture *depth, PPTexture *motion, PPTexture *reactive, PPTexture *output, float jitterX, float jitterY, bool reset) override;
	float GetHdrHeadroom() override { return mHdrActive ? mHdrHeadroom : 1.f; }
	bool HdrOutputActive() const { return mHdrActive; }
	void UpdateShadowMap() override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;

	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	FMaterial *CreateMaterial(FGameTexture *tex, int scaleflags) override;
	IVertexBuffer *CreateVertexBuffer() override;
	IIndexBuffer *CreateIndexBuffer() override;
	IDataBuffer *CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize) override;

	FTexture *WipeStartScreen() override;
	FTexture *WipeEndScreen() override;

	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;

	bool GetVSync() const { return mVSync; }
	void SetVSync(bool vsync) override;

	void Draw2D() override;
	void WaitForCommands(bool finish) override;

private:
	void RenderTextureView(FCanvasTexture *tex, std::function<void(IntRect &)> renderFunc) override;
	void CopyScreenToBuffer(int w, int h, uint8_t *data) override;
	void Present();
	void Commit(id<MTLCommandBuffer> commands);
	void BeginNextFrame();
	void QueuePipelines(bool both);
	void UpdateHdrOutput();
	void UpdateAutoQuality(double gpuMs, double seconds);
	void ApplyAutoQuality();

	std::unique_ptr<MtlBufferManager> mBufferManager;
	std::unique_ptr<MtlSamplerManager> mSamplerManager;
	std::unique_ptr<MtlTextureManager> mTextureManager;
	std::unique_ptr<MtlShaderManager> mShaderManager;
	std::unique_ptr<MtlShaderManager> mAlternateShaderManager;
	std::unique_ptr<MtlRenderBuffers> mScreenBuffers;
	std::unique_ptr<MtlRenderBuffers> mSaveBuffers;
	std::unique_ptr<MtlPostprocess> mPostprocess;
	std::unique_ptr<MtlRenderState> mRenderState;
	std::unique_ptr<MtlRaytrace> mRaytrace;
	MtlRenderBuffers *mActiveRenderBuffers = nullptr;

	id<MTLCommandBuffer> mDrawCommands = nil;
	id<MTLCommandBuffer> mUploadCommands = nil;
	id<MTLBlitCommandEncoder> mUploadEncoder = nil;
	id<CAMetalDrawable> mDrawable = nil;
	id mTemporalScaler = nil;	// MetalFX, created for the current input and output sizes
	bool mTemporalScalerReactive = false;	// created with a reactive mask input

	bool mVSync = false;
	bool mUnifiedMemory = true;
	bool mRaytracing = false;
	bool mRaytracingConfigured = false;
	bool mPrimaryShadersDone = false, mShadersReady = false;
	bool mUpscalerFailed = false;
	int mPrewarmSignature[4] = { -1, -1, -1, -1 };
	bool mSunPrewarm = false;	// a sunlit level was loaded: prewarm the sun variants too

	std::shared_ptr<MtlFrameSync> mSync = std::make_shared<MtlFrameSync>();
	uint64_t mFrameSerial = 1;
	uint64_t mLastTimedFrame = 0;
	int mFrameSlot = 0;
	int mFramesInFlight = 1;
	double mGpuStatTime = 0.0;	// seconds of GPU time of the last finished frame
	struct DeferredRelease { id Object; uint64_t Frame; };
	std::vector<DeferredRelease> mDeferredReleases;

	bool mHdrActive = false;	// EDR swapchain (vk_hdr): RGBA16Float in extended linear sRGB
	float mHdrHeadroom = 1.f;
	AppleGraphics::Capabilities mAutoCaps{};
	AppleGraphics::Settings mAutoSettings{};
	AppleGraphics::AdaptiveQuality mAutoQuality;
	bool mAutoWasEnabled = false;
	bool mAdaptiveWasEnabled = false;
	int mAutoThermal = -1, mAutoWidth = 0, mAutoHeight = 0;
	int mAutoTargetFps = -1;
	bool mAutoLowPower = false;
	double mLastPresentTime = 0, mLastPolicyTime = 0, mAverageFrameMs = 0;
	double mLastQualityLogTime = 0;
};

class CMetalError : public CEngineError
{
public:
	CMetalError() : CEngineError() {}
	CMetalError(const char *message) : CEngineError(message) {}
};
