#pragma once
#include <algorithm>
#include <cstdint>

namespace IOSHardware
{
struct Capabilities
{
    int appleFamily;
    uint64_t memoryBytes;
    int refreshRate;
    bool simulator;
    bool television = false;
};
struct Settings
{
    int fps;
    int maxRenderHeight;
    int msaa;
    int anisotropy;
    int ssao;
    bool bloom;
    int preset; // r_preset: 1 low .. 4 ultra (see hw_modernrender.h)
};
inline Settings Select(const Capabilities &caps, int thermalState, bool lowPower)
{
    constexpr uint64_t GiB = 1024ull * 1024 * 1024;
    Settings s{60, 720, 1, 4, 0, false, 1};
    // iOS reports usable physical RAM after firmware reservations, so a
    // marketed 4/6/8 GB device can report less than 4/6/8 GiB here.
    if (caps.appleFamily >= 6 && caps.memoryBytes >= 3 * GiB)
        s = {60, 1080, 2, 8, 0, false, 2};
    if (caps.appleFamily >= 7 && caps.memoryBytes >= 5 * GiB)
        s = {std::min(caps.refreshRate, 120), 1440, 2, 16, 1, true, 3};
    if (caps.appleFamily >= 9 && caps.memoryBytes >= 7 * GiB)
        s = {std::min(caps.refreshRate, 120), 1600, 4, 16, 1, true, 4};
    if (caps.television)
    {
        // TV power and cooling permit larger images than a handheld budget.
        if (caps.appleFamily >= 3 && caps.memoryBytes >= 2 * GiB) s = {60, 1080, 2, 8, 0, false, 1};
        // A12 (family 5) holds high at 1440p on mains power; A15 runs ultra.
        if (caps.appleFamily >= 5) s = {60, 1440, 2, 16, 1, true, 3};
        if (caps.appleFamily >= 8) s = {60, 2160, 2, 16, 1, true, 4};
    }
    if (caps.simulator)
        s = {60, 900, 1, 4, 0, false, 2};
    s.fps = std::max(35, std::min(s.fps, caps.refreshRate));
    if (lowPower || thermalState >= 1)
    {
        s.fps = std::min(s.fps, 60);
        s.maxRenderHeight = std::min(s.maxRenderHeight, 1080);
        s.msaa = 1;
        s.ssao = 0;
        s.bloom = false;
        s.preset = std::min(s.preset, 2);
    }
    if (thermalState >= 2)
    {
        s.fps = 35; // The engine's simulation rate is 35 Hz.
        s.maxRenderHeight = 720;
        s.anisotropy = 4;
        s.preset = 1; // Baked GI and relief cost almost nothing per pixel.
    }
    return s;
}
}
