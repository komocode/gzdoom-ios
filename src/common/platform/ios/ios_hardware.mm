#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <SDL2/SDL.h>
#include "ios_hardware_policy.h"
#include "ios_platform.h"
#include "c_cvars.h"
#include "v_video.h"
#include "printf.h"
#include "hw_modernrender.h"

EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Int, vid_preferbackend)
EXTERN_CVAR(Int, gl_multisample)
EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Float, gl_texture_filter_anisotropic)
EXTERN_CVAR(Float, vid_scalefactor)
EXTERN_CVAR(Int, vid_scalemode)
EXTERN_CVAR(Bool, vid_scale_linear)
EXTERN_CVAR(Bool, vid_vsync)
static IOSHardware::Capabilities caps;
static int lastThermal = -1;
static bool lastLowPower;
static int lastHeight;
static bool initialized;

bool IOS_SupportsBaseVertex()
{
    return caps.appleFamily >= 3;
}

void IOS_UpdateHardware()
{
    if (!initialized) return;
    NSProcessInfo *info = NSProcessInfo.processInfo;
    int state = (int)info.thermalState;
#ifdef GZDOOM_TVOS
    bool low = false;
#else
    bool low = info.lowPowerModeEnabled;
#endif
    int height = screen ? screen->GetClientHeight() : 0;
    if (state == lastThermal && low == lastLowPower && height == lastHeight) return;
    auto settings = IOSHardware::Select(caps, state, low);
    // The preset writes gl_ssao and gl_bloom too; the thermal values below win.
    R_SetAutoPreset(settings.preset);
    vid_maxfps = settings.fps;
    gl_multisample = settings.msaa;
    gl_ssao = settings.ssao;
    gl_bloom = settings.bloom;
    gl_texture_filter_anisotropic = settings.anisotropy;
    vid_scalemode = 0;
    if (height > 0)
    {
        // With TAA the scene renders at maxRenderHeight and is upscaled to a
        // sharper output; otherwise the output is the render resolution.
        float output = std::min(float(height), settings.maxRenderHeight * settings.outputScale);
        vid_scalefactor = output / height;
        r_upscale = std::clamp(settings.maxRenderHeight / output, 0.5f, 1.f);
    }
    vid_scale_linear = true;
    lastThermal = state;
    lastLowPower = low;
    lastHeight = height;
}
void IOS_ApplyHardwareDefaults()
{
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    int family = 0;
    // Capability queries also cover future devices without a model-name whitelist.
    for (int f = 10; f >= 1; --f)
        if ([gpu supportsFamily:(MTLGPUFamily)(1000 + f)]) { family = f; break; }
    caps = {family, NSProcessInfo.processInfo.physicalMemory, (int)UIScreen.mainScreen.maximumFramesPerSecond, bool(TARGET_OS_SIMULATOR)};
#ifdef GZDOOM_TVOS
    caps.television = true;
#endif
    // On Tier 1 argument buffer GPUs (A10X Apple TV 4K, etc.) MoltenVK's argument
    // buffer path drops our sampler states: REPEAT samples as CLAMP and filters are
    // ignored, so walls and flats smear their edge texels. Use discrete bindings.
    // Must be set before MoltenVK is loaded; an explicit environment value wins.
    if (gpu.argumentBuffersSupport < MTLArgumentBuffersTier2)
        setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);
    initialized = true;
    // Vulkan through MoltenVK (1) or the native Metal renderer (4).
    if (vid_preferbackend != 4) vid_preferbackend = 1;
    vid_vsync = true;
    IOS_UpdateHardware();
    Printf("Apple GPU: %s, family %d, %.1f GiB memory, %d Hz display; Metal/Vulkan renderer\n", gpu.name.UTF8String, family, caps.memoryBytes / double(1024ull*1024*1024), caps.refreshRate);
}
