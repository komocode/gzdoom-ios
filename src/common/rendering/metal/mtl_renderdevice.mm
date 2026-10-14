/*
** mtl_renderdevice.mm
** Native Metal renderer.
**
** The device mirrors the Vulkan backend's structure (and shares its shaders and
** shader interface), so both stay interchangeable for the hardware renderer.
**
** Each frame's command buffers are committed at Present without waiting, so
** the CPU records the next frame while the GPU renders this one. Before a new
** frame starts, the device waits until no more than r_framesinflight frames
** are unfinished; that frame's per-frame buffers are then free to rewrite.
** GPU timings are collected by the completion handlers.
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
#include "hw_modernrender.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include "menustate.h"
#include "i_interface.h"
#include <chrono>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <Cocoa/Cocoa.h>
#endif

EXTERN_CVAR(Bool, vk_raytrace)
EXTERN_CVAR(Bool, vk_hdr)
EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Int, gl_multisample)
EXTERN_CVAR(Int, gl_fxaa)
EXTERN_CVAR(Float, gl_texture_filter_anisotropic)
EXTERN_CVAR(Float, vid_scalefactor)
EXTERN_CVAR(Int, vid_scalemode)
EXTERN_CVAR(Bool, vid_scale_linear)

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;
extern bool AppActive;

// 1 waits for the GPU at the end of every frame (the old behaviour, for
// debugging). Above 2 it takes effect after a restart.
CUSTOM_CVARD(Int, r_framesinflight, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Metal renderer: frames the CPU may record ahead of the GPU (1-3)")
{
	if (self < 1) self = 1;
	else if (self > MTL_MAX_FRAMES_IN_FLIGHT) self = MTL_MAX_FRAMES_IN_FLIGHT;
}

extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);

static void *framePool;

static double AutoQualityTime()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
	delete mWeatherVertices;
	delete mWeatherIndices;
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

	// Compile traced and shadow-map shaders at startup when RT is selected.
	// Automatic quality can then switch between them during gameplay.
	for (int f = 10; f >= 1; --f)
		if ([device supportsFamily:(MTLGPUFamily)(1000 + f)]) { mAutoCaps.appleFamily = f; break; }
	mAutoCaps.memoryBytes = NSProcessInfo.processInfo.physicalMemory;
	mAutoCaps.simulator = TARGET_OS_SIMULATOR;
	mAutoCaps.raytracingFromRender = device.supportsRaytracingFromRender;
	mAutoCaps.temporalUpscaler = SupportsPlatformUpscaler();
	mAutoCaps.gpuWorkingSetBytes = device.recommendedMaxWorkingSetSize;
#if TARGET_OS_IPHONE
	mAutoCaps.television = TARGET_OS_TV;
	mAutoCaps.tablet = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
#else
	mAutoCaps.desktop = true;
#endif
	// Manual RT preferences take effect on restart; automatic selection also
	// requires dedicated intersection hardware, not software traversal alone.
	mRaytracing = (r_preset_auto && r_raytrace_auto ? AppleGraphics::HardwareRaytracing(mAutoCaps) : bool(vk_raytrace))
		&& mAutoCaps.raytracingFromRender;
	mRaytracingConfigured = mRaytracing;
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
	mShaderManager.reset(new MtlShaderManager(this, mRaytracing));
	if (mRaytracing)
		mAlternateShaderManager.reset(new MtlShaderManager(this, false));

	// The flat vertex buffer is large (static level geometry plus every frame's
	// dynamic vertices), so it gets one copy per frame in flight only up to
	// the startup setting, and at least 2 so that 1 and 2 can be switched live.
	// The per-frame uniform buffers are ringed by MtlHardwareBuffer instead.
	mPipelineNbr = std::clamp(std::max(int(r_framesinflight), 2), 1, HW_MAX_PIPELINE_BUFFERS);
	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight(), mPipelineNbr);
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mRenderState.reset(new MtlRenderState(this));

	Printf("Metal device: %s%s\n", device.name.UTF8String, mRaytracing ? ", ray traced shadows" : "");
	UpdateAutoQuality(0, 0);
}

void MetalRenderDevice::ApplyAutoQuality()
{
	if (!r_preset_auto) return;
	const auto &s = mAutoSettings;
	int reduction = r_adaptive_quality ? mAutoQuality.reduction : 0;
	if (mShadersReady && mRaytracingConfigured)
	{
		bool trace = (!r_raytrace_auto || s.raytracing) && reduction < 3;
		if (trace != mRaytracing)
		{
			mShaderManager.swap(mAlternateShaderManager);
			mRaytracing = trace;
		}
	}
	R_ApplyRenderPreset(s.preset);
	vid_maxfps = r_adaptive_quality ? mAutoQuality.fps : s.fps;
	gl_multisample = s.msaa;
	gl_fxaa = s.fxaa;
	gl_ssao = s.ssao;
	gl_bloom = s.bloom;
	gl_texture_filter_anisotropic = s.anisotropy;
	// Mobile keeps height-correct traced shadows with one ray. Desktop starts
	// with nine, rather than assuming Ultra's 27 rays fit every GPU.
	if (mRaytracing)
	{
		gl_shadowmap_filter = mAutoCaps.desktop ? 1 : 0;
		if (r_raytrace_auto && !s.raytracing) gl_light_shadowmap = false;
	}
	if (reduction >= 1)
	{
		r_ssgi = 0;
		r_volumetric = std::min(int(r_volumetric), 1);
		r_volumetric_shadows = std::min(int(r_volumetric_shadows), 2);
		gl_shadowmap_filter = mRaytracing ? 0 : 1;
		gl_ssao = std::min(int(gl_ssao), 2);
	}
	if (reduction >= 2)
	{
		r_weather_quality = std::min(int(r_weather_quality), 1); // no distant rain layer
		r_ssr = std::min(int(r_ssr), 1);
		r_parallax = false;
		gl_ssao = std::min(int(gl_ssao), 1);
	}
	if (reduction >= 3)
	{
		r_ssr = 0;
		r_volumetric = 0;
		r_gpuparticles = 1;
		// The precompiled raster variant removes RT register pressure as well
		// as ray work, and keeps inexpensive shadow maps instead of no shadows.
		gl_light_shadowmap = true;
		gl_shadowmap_filter = 1;
		gl_shadowmap_quality = 256;
	}
	if (GetClientWidth() > 0 && GetClientHeight() > 0)
	{
		auto resolution = AppleGraphics::SelectResolution(s, GetClientWidth(), GetClientHeight(),
			r_adaptive_quality ? mAutoQuality.scale : 1.f, r_adaptive_quality ? mAutoQuality.presentationScale : 1.f);
		// Pixel art is stretched with nearest filtering, at an integer factor when close.
		auto presentation = AppleGraphics::SelectPresentation(resolution.outputScale, r_pixelart);
		vid_scalemode = 0;
		vid_scalefactor = presentation.outputScale;
		r_upscale = resolution.sceneScale;
		vid_scale_linear = presentation.linear;
	}
}

void MetalRenderDevice::UpdateAutoQuality(double gpuMs, double seconds)
{
	bool enabled = r_preset_auto;
	if (!enabled)
	{
		mAutoWasEnabled = false;
		mAutoQuality.RestartMeasurements();
		return;
	}
	double now = AutoQualityTime();
	if (!mAutoWasEnabled || now - mLastPolicyTime >= 1 || mAdaptiveWasEnabled != bool(r_adaptive_quality) || mAutoTargetFps != int(r_adaptive_targetfps))
	{
		mLastPolicyTime = now;
		NSProcessInfo *info = NSProcessInfo.processInfo;
		int thermal = (int)info.thermalState;
#if TARGET_OS_TV
		bool low = false;
#else
		bool low = info.lowPowerModeEnabled;
#endif
#if TARGET_OS_IPHONE
		int hz = (int)UIScreen.mainScreen.maximumFramesPerSecond;
#else
		NSScreen *display = [(NSWindow *)m_window screen] ?: NSScreen.mainScreen;
		int hz = 60;
		if (@available(macOS 12.0, *)) hz = (int)display.maximumFramesPerSecond;
#endif
		bool changed = !mAutoWasEnabled || thermal != mAutoThermal || low != mAutoLowPower || hz != mAutoCaps.refreshRate
			|| mAdaptiveWasEnabled != bool(r_adaptive_quality) || mAutoTargetFps != int(r_adaptive_targetfps);
		bool resized = GetClientWidth() != mAutoWidth || GetClientHeight() != mAutoHeight;
		mAutoCaps.refreshRate = std::max(35, hz);
		if (changed)
		{
			mAutoSettings = AppleGraphics::Select(mAutoCaps, thermal, low);
			mAutoTargetFps = r_adaptive_targetfps;
			if (mAutoTargetFps > 0 && !low && thermal == 0)
				mAutoSettings.fps = std::min(mAutoCaps.refreshRate, std::clamp(mAutoTargetFps, 35, 120));
			mAutoQuality.Reset(mAutoSettings, !r_raytrace_auto && mRaytracingConfigured);
			mAutoThermal = thermal;
			mAutoLowPower = low;
			mAutoWasEnabled = true;
			mAdaptiveWasEnabled = r_adaptive_quality;
		}
		if (changed || resized)
		{
			mAutoWidth = GetClientWidth();
			mAutoHeight = GetClientHeight();
			mAutoQuality.RestartMeasurements();
			ApplyAutoQuality();
			PrintAutoQuality();
		}
	}
	bool gameplay = AppActive && (gamestate == GS_LEVEL || gamestate == GS_TITLELEVEL) && menuactive == MENU_Off && !paused && !pauseext;
	if (gameplay && seconds > 0 && seconds < 0.25)
		mAverageFrameMs = mAverageFrameMs == 0 ? seconds * 1000 : mAverageFrameMs + 0.05 * (seconds * 1000 - mAverageFrameMs);
	if (r_adaptive_quality && mAutoQuality.Observe(gpuMs, seconds, mAutoSettings.fps, gameplay))
	{
		ApplyAutoQuality();
		PrintAutoQuality();
	}
	if (r_adaptive_debug && now - mLastQualityLogTime >= 5)
	{
		mLastQualityLogTime = now;
		PrintAutoQuality();
	}
}

void MetalRenderDevice::PrintAutoQuality() const
{
	Printf("Apple auto quality: %s, preset %d, target %d FPS, 3D scale %.2f, output scale %.2f, effects reduction %d, RT %s, upscaler %s; GPU %.2f ms, observed %.1f FPS; thermal %d, paused %d\n",
		r_preset_auto ? "on" : "manual", int(r_preset), int(vid_maxfps), double(r_upscale), double(vid_scalefactor), mAutoQuality.reduction,
		mRaytracing && gl_light_shadowmap ? "on" : "off",
		r_taa && r_upscale < 1 && r_upscale_metalfx && mAutoCaps.temporalUpscaler ? "MetalFX" : "TAA/native",
		mAutoQuality.averageMs, mAverageFrameMs > 0 ? 1000 / mAverageFrameMs : 0, mAutoThermal, !AppActive || paused || pauseext || menuactive != MENU_Off);
}

CCMD(r_apple_status)
{
	if (screen && screen->Backend() == 4) static_cast<MetalRenderDevice *>(screen)->PrintAutoQuality();
	else Printf("Apple adaptive quality requires the native Metal renderer.\n");
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

MtlFrameSync::Frame &MtlFrameSync::Get(uint64_t serial)
{
	Frame &frame = Frames[serial % (MTL_MAX_FRAMES_IN_FLIGHT + 2)];
	if (frame.Serial != serial)
		frame = { serial, 0, false, 0.0 };
	return frame;
}

void MtlFrameSync::Advance()
{
	while (true)
	{
		const Frame &frame = Frames[(Completed + 1) % (MTL_MAX_FRAMES_IN_FLIGHT + 2)];
		if (frame.Serial != Completed + 1 || !frame.Ended || frame.Pending > 0)
			break;
		Completed = frame.Serial;
		LastGpuTime = frame.GpuTime;
	}
	Changed.notify_all();
}

void MetalRenderDevice::Commit(id<MTLCommandBuffer> commands)
{
	std::shared_ptr<MtlFrameSync> sync = mSync;
	uint64_t serial = mFrameSerial;
	{
		std::lock_guard<std::mutex> lock(sync->Mutex);
		sync->Get(serial).Pending++;
		sync->Outstanding++;
	}
	[commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
		double gpuTime = buffer.GPUEndTime > buffer.GPUStartTime ? buffer.GPUEndTime - buffer.GPUStartTime : 0.0;
		std::lock_guard<std::mutex> lock(sync->Mutex);
		auto &frame = sync->Get(serial);
		frame.Pending--;
		frame.GpuTime += gpuTime;
		sync->Outstanding--;
		sync->Advance();
	}];
	[commands commit];
}

void MetalRenderDevice::FlushCommands(bool wait)
{
	if (mRenderState)
		mRenderState->EndRenderPass();

	// Uploads first: the draws may use what they upload.
	if (mUploadCommands)
	{
		if (mUploadEncoder)
			[mUploadEncoder endEncoding];
		mUploadEncoder = nil;
		Commit(mUploadCommands);
		mUploadCommands = nil;
	}
	if (mDrawCommands)
	{
		Commit(mDrawCommands);
		mDrawCommands = nil;
	}
	if (wait)
	{
		std::unique_lock<std::mutex> lock(mSync->Mutex);
		mSync->Changed.wait(lock, [&] { return mSync->Outstanding == 0; });
		lock.unlock();
		mDeferredReleases.clear();
	}
}

void MetalRenderDevice::WaitForCommands(bool finish)
{
	FlushCommands(true);
}

bool MetalRenderDevice::GpuBusy()
{
	std::lock_guard<std::mutex> lock(mSync->Mutex);
	return mSync->Outstanding > 0;
}

void MetalRenderDevice::WaitForFrame(uint64_t serial)
{
	std::unique_lock<std::mutex> lock(mSync->Mutex);
	mSync->Changed.wait(lock, [&] { return mSync->Completed >= serial; });
}

void MetalRenderDevice::DeferRelease(id object)
{
	if (object)
		mDeferredReleases.push_back({ object, mFrameSerial });
}

void MetalRenderDevice::BeginNextFrame()
{
	{
		std::lock_guard<std::mutex> lock(mSync->Mutex);
		mSync->Get(mFrameSerial).Ended = true;
		mSync->Advance();
	}

	int frames = std::clamp(int(r_framesinflight), 1, mVertexData->mPipelineNbr);
	if (frames != mFramesInFlight)
	{
		// Slots are assigned modulo the frame count: start from an idle GPU.
		FlushCommands(true);
		mFramesInFlight = frames;
	}

	mFrameSerial++;
	if (mFrameSerial > (uint64_t)frames)
		WaitForFrame(mFrameSerial - frames);
	mFrameSlot = int(mFrameSerial % frames);

	uint64_t completed;
	{
		std::lock_guard<std::mutex> lock(mSync->Mutex);
		completed = mSync->Completed;
		if (completed > mLastTimedFrame)
		{
			mLastTimedFrame = completed;
			mGpuStatTime = mSync->LastGpuTime;
		}
	}
	mDeferredReleases.erase(std::remove_if(mDeferredReleases.begin(), mDeferredReleases.end(),
		[=](const DeferredRelease &item) { return item.Frame <= completed; }), mDeferredReleases.end());

	// Everything the CPU writes from here on belongs to the new frame.
	mBufferManager->SetFrameSlot(mFrameSlot);
	mVertexData->NextPipelineBuffer();
	mRenderState->SetVertexBuffer(mVertexData);
}

// HDR output (vk_hdr): an extended dynamic range swapchain whose 1.0 is SDR
// white. The tone mapper extends highlights up to the display's current EDR
// headroom; the HUD and menus stay at SDR white.
void MetalRenderDevice::UpdateHdrOutput()
{
	bool want = vk_hdr;
	float headroom = 1.f;
#if TARGET_OS_TV
	want = false;
#elif TARGET_OS_IPHONE
	if (@available(iOS 16.0, *))
		headroom = (float)UIScreen.mainScreen.currentEDRHeadroom;
	else
		want = false;
#else
	NSScreen *display = NSApp.keyWindow.screen ? NSApp.keyWindow.screen : NSScreen.mainScreen;
	headroom = display ? (float)display.maximumExtendedDynamicRangeColorComponentValue : 1.f;
#endif
	mHdrHeadroom = std::clamp(headroom, 1.f, 16.f);

	if (want == mHdrActive || mDrawable)
		return;
	mHdrActive = want;
	layer.pixelFormat = want ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
	CGColorSpaceRef colorspace = want ? CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB) : nullptr;
	layer.colorspace = colorspace;
	if (colorspace) CGColorSpaceRelease(colorspace);
#if TARGET_OS_TV
#elif TARGET_OS_IPHONE
	if (@available(iOS 16.0, *))
		layer.wantsExtendedDynamicRangeContent = want;
#else
	layer.wantsExtendedDynamicRangeContent = want;
#endif
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

	UpdateHdrOutput();
	mPostprocess->DrawPresentTexture(mOutputLetterbox, true, false);
	if (mDrawable)
		[GetDrawCommands() presentDrawable:mDrawable];

	FlushCommands(false);
	mDrawable = nil;
	// VSync controls tearing/presentation; it does not enforce our thermal or
	// adaptive FPS cap. Submit the GPU work before the CPU limiter sleeps.
	FPSLimit();
	uint64_t timedFrame = mLastTimedFrame;
	BeginNextFrame();
	// GPU time of the newest finished frame, once per frame (0 skips the sample).
	double gpuMs = mLastTimedFrame != timedFrame ? mGpuStatTime * 1000 : 0;
	double now = AutoQualityTime();
	UpdateAutoQuality(gpuMs, mLastPresentTime > 0 ? now - mLastPresentTime : 0);
	mLastPresentTime = now;

	if (gpuStatActive)
		gpuStatOutput.Format("Metal GPU frame=%04.2f ms, %d in flight\n", mGpuStatTime * 1000.0, mFramesInFlight);
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
	if (mShadersReady) return true;
	if (!mPrimaryShadersDone)
	{
		mPrimaryShadersDone = mShaderManager->CompileNextShader();
		if (!mPrimaryShadersDone) return false;
	}
	if (mAlternateShaderManager && !mAlternateShaderManager->CompileNextShader()) return false;
	mShadersReady = true;
	// Compile the active shaders' common pipelines while the startup screen is up;
	// the alternate (traced or shadow-map) set follows in the background.
	ApplyAutoQuality();
	QueuePipelines(false);
	mRenderState->FinishPrewarm();
	QueuePipelines(true);
	return true;
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

	if (mShadersReady)
	{
		QueuePipelines(true);
		// A couple at a time, so a pipeline needed right now never waits long for the compiler.
		mRenderState->PumpPrewarm(2);
	}
}

void MetalRenderDevice::QueuePipelines(bool both)
{
	// Again whenever the vertex layouts or the sample count change.
	auto vertexFormat = [](IVertexBuffer *buffer) { return buffer ? static_cast<MtlHardwareVertexBuffer*>(buffer)->VertexFormat : -1; };
	MtlRenderState::PrewarmFormats formats;
	formats.flat = vertexFormat(mVertexData->GetBufferObjects().first);
	formats.sky = vertexFormat(mSkyData->GetBufferObjects().first);
	formats.particles = vertexFormat(mFXParticleVertices);
	int samples = mScreenBuffers->GetBestSampleCount();
	int signature[] = { formats.flat, formats.sky, formats.particles, samples };
	if (!both)
	{
		mRenderState->QueueScenePipelines(mShaderManager.get(), formats, samples);
		return;
	}
	if (memcmp(signature, mPrewarmSignature, sizeof(signature)) == 0)
		return;
	memcpy(mPrewarmSignature, signature, sizeof(signature));
	for (auto shaders : { mShaderManager.get(), mAlternateShaderManager.get() })
	{
		if (shaders)
			mRenderState->QueueScenePipelines(shaders, formats, samples);
	}
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

bool MetalRenderDevice::InitWeatherMap(int width, int height, TArray<uint16_t> &rgbaHalfData)
{
	mTextureManager->SetWeatherMap(width, height, rgbaHalfData);
	return true;
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

void MetalRenderDevice::ReflectScene(float m5)
{
	mPostprocess->ReflectScene(m5);
}

void MetalRenderDevice::SetLevelMesh(hwrenderer::LevelMesh *mesh)
{
	if (mRaytracingConfigured)
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
	{
		NullMesh->MeshElements.Push(i);
		NullMesh->ShadowElements.Push(i);
	}
}

MtlRaytrace::~MtlRaytrace()
{
	delete NullMesh;
}

void MtlRaytrace::SetLevelMesh(hwrenderer::LevelMesh *mesh)
{
	if (!mesh || mesh->ShadowElements.Size() < 3)
		mesh = NullMesh;
	// Keyed on the generation, not the address: a new level's mesh can be
	// allocated where the previous one was.
	if (mesh->Generation == BuiltGeneration)
		return;
	BuiltGeneration = mesh->Generation;
	// Frames in flight may still trace against the old level.
	fb->DeferRelease(TopLevel);
	fb->DeferRelease(BottomLevel);
	TopLevel = nil;
	BottomLevel = nil;
	Build(mesh);
}

void MtlRaytrace::Build(const hwrenderer::LevelMesh *mesh)
{
	id<MTLDevice> device = fb->device;
	id<MTLBuffer> vertices = [device newBufferWithBytes:mesh->MeshVertices.Data() length:mesh->MeshVertices.Size() * sizeof(FVector3) options:MTLResourceStorageModeShared];
	id<MTLBuffer> indices = [device newBufferWithBytes:mesh->ShadowElements.Data() length:mesh->ShadowElements.Size() * sizeof(uint32_t) options:MTLResourceStorageModeShared];

	MTLAccelerationStructureTriangleGeometryDescriptor *geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geometry.vertexBuffer = vertices;
	geometry.vertexStride = sizeof(FVector3);
	geometry.indexBuffer = indices;
	geometry.indexType = MTLIndexTypeUInt32;
	geometry.triangleCount = mesh->ShadowElements.Size() / 3;
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

	// Built once per level, and again (throttled) while sectors move, so a
	// separate command buffer that is waited for is fine.
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
	if (mUpscalerFailed) return false;
#if TARGET_OS_IPHONE && !TARGET_OS_TV
	// iOS/iPadOS 27.0.x advertises support, but the temporal scaler's MPSGraph
	// ANE prewarm aborts inside LLVM on the reference A19 Pro. It is asynchronous
	// and cannot be caught or handled by a nil return. Use the GPU TAA resolve
	// on this affected OS line; do not disable hardware RT with the upscaler.
	NSOperatingSystemVersion os = NSProcessInfo.processInfo.operatingSystemVersion;
	if (os.majorVersion == 27 && os.minorVersion == 0) return false;
#endif
#ifdef HAVE_METALFX
	if (@available(macOS 13.0, iOS 16.0, *))
		return [MTLFXTemporalScalerDescriptor supportsDevice:device];
#endif
	return false;
}

bool MetalRenderDevice::PlatformTemporalUpscale(PPTexture *color, PPTexture *depth, PPTexture *motion, PPTexture *reactive, PPTexture *output, float jitterX, float jitterY, bool reset)
{
#ifdef HAVE_METALFX
	if (@available(macOS 13.0, iOS 16.0, *))
	{
		auto textures = GetTextureManager();
		id<MTLTexture> colorTex = textures->GetTexture(PPTextureType::PPTexture, color)->Texture;
		id<MTLTexture> depthTex = textures->GetTexture(PPTextureType::PPTexture, depth)->Texture;
		id<MTLTexture> motionTex = textures->GetTexture(PPTextureType::PPTexture, motion)->Texture;
		id<MTLTexture> outputTex = textures->GetTexture(PPTextureType::PPTexture, output)->Texture;
		// The reactive mask (macOS 14.4 / iOS 17.4) keeps MetalFX from smearing
		// sprites, particles and liquids, which have no motion vectors.
		id<MTLTexture> reactiveTex = nil;
		if (@available(macOS 14.4, iOS 17.4, *))
		{
			if (reactive) reactiveTex = textures->GetTexture(PPTextureType::PPTexture, reactive)->Texture;
		}

		id<MTLFXTemporalScaler> scaler = mTemporalScaler;
		if (!scaler || scaler.inputWidth != colorTex.width || scaler.inputHeight != colorTex.height || scaler.outputWidth != outputTex.width || scaler.outputHeight != outputTex.height || mTemporalScalerReactive != (reactiveTex != nil))
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
			// Its internal history may still be in use by frames in flight.
			DeferRelease(mTemporalScaler);
			if (@available(macOS 14.4, iOS 17.4, *))
			{
				if (reactiveTex)
				{
					desc.reactiveMaskTextureEnabled = YES;
					desc.reactiveMaskTextureFormat = reactiveTex.pixelFormat;
				}
			}
			scaler = [desc newTemporalScalerWithDevice:device];
			mTemporalScaler = scaler;
			mTemporalScalerReactive = reactiveTex != nil;
			reset = true;
			if (!scaler)
			{
				mUpscalerFailed = true;
				mAutoCaps.temporalUpscaler = false;
				Printf("MetalFX could not allocate an upscaler; using TAA.\n");
				return false;
			}
		}

		mRenderState->EndRenderPass();
		scaler.colorTexture = colorTex;
		scaler.depthTexture = depthTex;
		scaler.motionTexture = motionTex;
		scaler.outputTexture = outputTex;
		if (@available(macOS 14.4, iOS 17.4, *))
		{
			if (reactiveTex) scaler.reactiveMaskTexture = reactiveTex;
		}
		scaler.inputContentWidth = colorTex.width;
		scaler.inputContentHeight = colorTex.height;
		scaler.jitterOffsetX = jitterX;
		scaler.jitterOffsetY = -jitterY;
		scaler.motionVectorScaleX = colorTex.width;
		scaler.motionVectorScaleY = colorTex.height;
		scaler.depthReversed = NO;
		scaler.reset = reset;
		[scaler encodeToCommandBuffer:GetDrawCommands()];
		return true;
	}
#endif
	return false;
}
