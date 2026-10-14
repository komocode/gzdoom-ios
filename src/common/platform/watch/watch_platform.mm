#define TimeScale AppleTimeScale
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#undef TimeScale
#include "../../../../watchos/WatchBridge.h"
#include "watch_bridge_internal.h"
#include "cmdlib.h"
#include "m_argv.h"
#include "i_system.h"
#include "i_time.h"
#include "m_joy.h"
#include "st_start.h"
#include "engineerrors.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "gamestate.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>
#include <clocale>
#include <unistd.h>

extern int GameMain();
FString sys_ostype;
FArgs *Args;
double PerfToSec = 1.e-9, PerfToMillisec = 1.e-6;
bool GUICapture;
int WaitingForKey;
CVAR(String, queryiwad_key, "shift", CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, con_printansi, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, con_4bitansi, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, use_mouse, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)

static std::mutex frameMutex, activeMutex, errorMutex;
static std::condition_variable activeCondition;
static NSData *latestFrame;
static int frameWidth, frameHeight;
static std::atomic<uint64_t> frames;
static std::atomic<bool> active{true};
static std::once_flag startOnce;
static std::string lastError;
static bool renderTest;
static std::atomic<int> screenWidth{0}, screenHeight{0};

extern "C" void GZDWatchSetScreen(int width, int height)
{
    screenWidth = width;
    screenHeight = height;
}

void WatchScreenSize(int &width, int &height)
{
    width = screenWidth;
    height = screenHeight;
}

void WatchPresent(const uint8_t *pixels, int width, int height, int pitch)
{
    @autoreleasepool
    {
        NSMutableData *data = [NSMutableData dataWithLength:size_t(width) * height * 4];
        for (int y = 0; y < height; ++y)
            memcpy((uint8_t *)data.mutableBytes + size_t(y) * width * 4, pixels + size_t(y) * pitch * 4, size_t(width) * 4);
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame = data;
            frameWidth = width;
            frameHeight = height;
        }
        ++frames;
    }
}

void WatchWaitForActive()
{
    if (active) return;
    I_FreezeTime(true);
    std::unique_lock<std::mutex> lock(activeMutex);
    activeCondition.wait(lock, [] { return active.load(); });
    I_FreezeTime(false);
    I_ResetFrameTime();
}

int WatchFrameLimit()
{
    NSProcessInfo *process = NSProcessInfo.processInfo;
    if (process.thermalState >= NSProcessInfoThermalStateSerious) return 18;
    if (process.lowPowerModeEnabled || process.thermalState == NSProcessInfoThermalStateFair) return 25;
    return 35;
}

extern "C" void GZDWatchSetActive(bool value)
{
    { std::lock_guard<std::mutex> lock(activeMutex); active = value; }
    if (value) activeCondition.notify_one();
}
extern "C" uint64_t GZDWatchFrameCount() { return frames; }
extern "C" const char *GZDWatchError()
{
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lock(errorMutex);
    copy = lastError;
    return copy.c_str();
}
static void SetError(const char *error)
{
    std::lock_guard<std::mutex> lock(errorMutex);
    lastError = error;
}
extern "C" CGImageRef GZDWatchCopyImage()
{
    NSData *data;
    int width, height;
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        data = latestFrame;
        width = frameWidth; height = frameHeight;
    }
    if (!data) return nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, color,
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(color);
    CGDataProviderRelease(provider);
    return image;
}

extern "C" void GZDWatchStart(const char *iwad)
{
    std::string game(iwad);
    std::call_once(startOnce, [game] {
        renderTest = [NSProcessInfo.processInfo.arguments containsObject:@"-watch-render-test"];
        std::thread([game] {
            @autoreleasepool
            {
                setvbuf(stdout, nullptr, _IONBF, 0);
                setvbuf(stderr, nullptr, _IONBF, 0);
                setlocale(LC_ALL, "C");
                setenv("LC_NUMERIC", "C", 1);
                progdir = NSBundle.mainBundle.bundlePath.UTF8String;
                progdir += "/";
                NSURL *docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
                std::vector<std::string> args = {"gzdoom-watch", "-iwad", game, "-nosound", "-nomusic", "-warp", "1",
                    "+queryiwad", "false", "+vid_rendermode", "1", "+r_noaccel", "true", "+vid_maxfps", "35", "+vid_vsync", "false",
                    "+vid_scalemode", "0", "+vid_scalefactor", "1", "+screenblocks", "10",
                    // True-color software renderer quality: filtered and mipmapped
                    // textures, a smooth sky and colored dynamic lights.
                    "+r_magfilter", "true", "+r_minfilter", "true", "+r_mipmap", "true", "+r_linearsky", "true",
                    "+r_dynlights", "true",
                    "+logfile", [docs.path stringByAppendingPathComponent:@"engine.log"].UTF8String};
                // Light definitions for the stock games give lamps, torches,
                // projectiles and explosions real light on the true-color renderer.
                std::string lights = std::string(progdir.GetChars()) + "lights.pk3";
                if (access(lights.c_str(), R_OK) == 0)
                {
                    args.push_back("-file");
                    args.push_back(lights);
                }
                std::vector<char *> raw;
                for (auto &arg : args) raw.push_back(arg.data());
                raw.push_back(nullptr);
                Args = new FArgs((int)args.size(), raw.data());
                try { GameMain(); }
                catch (const std::exception &error) { SetError(error.what()); }
                catch (...) { SetError("The Watch renderer stopped unexpectedly."); }
            }
        }).detach();
    });
}

void I_DetectOS() { sys_ostype = "watchOS"; }
void CalculateCPUSpeed() { }
void I_PrintStr(const char *text) { fputs(text, stdout); }
void I_SetIWADInfo() { }
bool HoldingQueryKey(const char *) { return false; }
bool I_PickIWad(bool, FStartupSelectionInfo &) { return true; }
extern "C" int I_FileAvailable(const char *) { return 0; }
void I_ShowFatalError(const char *error) { SetError(error); NSLog(@"GZDoom Watch: %s", error); }
void I_PutInClipboard(const char *) { }
FString I_GetFromClipboard(bool) { return ""; }
FString I_GetCWD() { char *path = getcwd(nullptr, 0); FString result = path ? path : ""; free(path); return result; }
bool I_ChDir(const char *path) { return chdir(path) == 0; }
unsigned int I_MakeRNGSeed() { return arc4random(); }
void I_OpenShellFolder(const char *) { }
bool I_IsDarkMode() { return true; }
bool I_SetCursor(FGameTexture *) { return false; }
void I_StartFrame() { }
void I_StartTic()
{
    WatchWaitForActive();
    static bool rotating;
    if (renderTest && !rotating && gamestate == GS_LEVEL)
    {
        AddCommandString("+right", 0);
        rotating = true;
    }
}
void I_GetEvent() { WatchWaitForActive(); }
void I_SetWindowTitle(const char *) { }
void I_SetMouseCapture() { }
void I_ReleaseMouseCapture() { }
void I_StartupJoysticks() { }
void I_ShutdownInput() { }
void I_ProcessJoysticks() { }
void I_GetJoysticks(TArray<IJoystickConfig *> &) { }
IJoystickConfig *I_UpdateDeviceList() { return nullptr; }
void I_GetAxes(float axes[NUM_AXIS_CODES]) { memset(axes, 0, NUM_AXIS_CODES * sizeof(float)); }
void I_Rumble(double, double, double, double) { }
FStartupScreen *FStartupScreen::CreateInstance(int maximum) { return new FStartupScreen(maximum); }
bool IsZWidgetAvailable() { return false; }
void InitWidgetResources(const char *) { }
void CloseWidgetResources() { }
