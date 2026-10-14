#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure policy: estimates are starting points, not device benchmarks. No Apple
// framework dependencies, so old and future hardware can be tested off-device.
namespace AppleGraphics
{
struct Capabilities
{
    int appleFamily;
    uint64_t memoryBytes;
    int refreshRate;
    bool simulator;
    bool television = false;
    bool tablet = false;
    bool desktop = false;
    bool raytracingFromRender = false;
    bool temporalUpscaler = false;
    uint64_t gpuWorkingSetBytes = 0;
};

inline bool HardwareRaytracing(const Capabilities &c)
{
    // The ray tracing API also exists on GPUs with software traversal.
    // Dedicated intersection hardware starts at Apple9 (A17 Pro / M3).
    return c.appleFamily >= 9 && c.raytracingFromRender && !c.simulator;
}

struct Settings
{
    int fps;
    int maxRenderHeight; // short edge, independent of display orientation
    int msaa;
    int anisotropy;
    int ssao;
    bool bloom;
    int preset; // 1 low .. 4 ultra
    float outputScale = 1.0f;
    float initialScale = 1.0f;
    bool raytracing = false;
    int fxaa = 0;
};

inline Settings Select(const Capabilities &c, int thermalState, bool lowPower)
{
    constexpr uint64_t GiB = 1024ull * 1024 * 1024;
    Settings s{60, 720, 1, 4, 0, false, 1};
    if (c.appleFamily >= 6 && c.memoryBytes >= 3 * GiB)
        s = {60, 1080, 2, 8, 1, true, 2};
    if (c.appleFamily >= 7 && c.memoryBytes >= 5 * GiB)
        s = {120, 1440, 1, 16, 2, true, 3};
    if (c.appleFamily >= 9 && c.memoryBytes >= 7 * GiB)
    {
        // A17 Pro mini / phones start at High. Pro iPads and newer phone GPUs
        // have more headroom for Ultra; GPU timings correct the estimate.
        s = {120, 1200, 1, 16, 2, true, 3};
        if ((c.tablet && c.refreshRate > 60) || c.appleFamily >= 10)
            s = {120, c.tablet ? 1600 : 1440, 1, 16, 3, true, 4};
    }
    if (c.desktop)
    {
        // RAM alone is not a GPU class. Include its recommended working set.
        s = {120, 1440, 1, 16, 2, true, 3};
        if (c.appleFamily >= 7 && c.memoryBytes >= 16 * GiB && c.gpuWorkingSetBytes >= 10 * GiB)
            s = {120, 1800, 1, 16, 3, true, 4};
        if (!c.appleFamily) s = {60, 1080, 2, 8, 1, true, 2};
        // M1/M2 at Retina output: favor Ultra at a sustainable 60 over
        // discarding effects in pursuit of an optimistic 120-FPS estimate.
        if (c.appleFamily < 9)
        {
            s.fps = 60;
            s.maxRenderHeight = std::min(s.maxRenderHeight, 1600);
        }
    }
    if (c.television)
    {
        // Extrapolated, not TV benchmarks. A 4K HDMI mode is not a 4K render
        // budget: full-output effects also proved costly on the A17 Pro mini.
        // GPU family is a feature level, not a speed score: budget A10X and
        // A12 alike. Avoid MSAA's extra render-target footprint.
        s = {60, 720, 1, 4, 0, false, 1};
        if (c.appleFamily >= 3 && c.memoryBytes >= 2 * GiB)
            s = {60, 1080, 1, 8, 1, true, 2};
        if (c.appleFamily >= 8 && c.memoryBytes >= 3 * GiB)
            s = {60, 1080, 1, 16, 2, true, 3};
        // Newer TV GPUs can earn a larger ceiling without assuming Ultra or
        // native 4K. Hardware RT still needs the actual render-stage query.
        if (c.appleFamily >= 9 && c.memoryBytes >= 5 * GiB)
            s = {60, 1440, 1, 16, 2, true, 3};
    }
    if (c.simulator) s = {60, 900, 1, 4, 0, false, 2};
    // Fidelity-first mobile estimate. Sustained 120 FPS with RT and the full
    // postprocess stack proved too optimistic on the reference A19 Pro.
    // Retain traced shadows and surface detail at a stable display-paced 60.
    if (!c.desktop && !c.television && !c.simulator) s.fps = std::min(s.fps, 60);
    s.fps = std::clamp(c.refreshRate, 35, s.fps);
    s.raytracing = HardwareRaytracing(c) && s.preset >= 3;
    if (lowPower || thermalState >= 1)
    {
        s.fps = std::min(s.fps, 60);
        s.maxRenderHeight = std::min(s.maxRenderHeight, 1080);
        s.msaa = 1;
        s.ssao = 0;
        s.bloom = false;
        s.preset = std::min(s.preset, 2);
        s.raytracing = false;
    }
    if (thermalState >= 2)
    {
        s.fps = 35;
        s.maxRenderHeight = 720;
        s.anisotropy = 4;
        s.preset = 1;
    }
    if (s.preset >= 3)
    {
        s.msaa = 1;
        // Extra output pixels are useful with MetalFX. With the TAA fallback,
        // retain the same 3D budget but avoid an oversized postprocess chain.
        s.outputScale = c.temporalUpscaler ? 1.25f : 1.f;
        s.initialScale = s.raytracing ? 0.85f : 0.9f;
    }
    // Low/Medium TV profiles avoid multisampled render targets but still
    // smooth edges on the large display with the cheaper FXAA pass.
    if (c.television && s.preset <= 2) s.fxaa = 1;
    return s;
}

struct Resolution { float outputScale, sceneScale; };
inline Resolution SelectResolution(const Settings &s, int width, int height, float adaptiveScale, float presentationScale = 1.f)
{
    int edge = std::max(1, std::min(width, height));
    float output = std::min(float(edge), s.maxRenderHeight * s.outputScale) * presentationScale;
    if (s.preset >= 3)
        return {output / edge, std::clamp(std::min(float(s.maxRenderHeight), output) * adaptiveScale / output, 0.5f, 1.f)};
    return {output / edge * adaptiveScale, 1.f};
}

// Pixel-art presentation of a reduced output: nearest-neighbour instead of a
// bilinear stretch, and a scale just above 1/n snaps down to 1/n so the stretch
// is an exact integer multiple. A texel already spans several screen pixels at
// these resolutions, so a non-integer nearest stretch is far less visible than blur.
struct Presentation { float outputScale; bool linear; };
inline Presentation SelectPresentation(float outputScale, bool pixelArt)
{
    if (!pixelArt || outputScale >= 1.f) return {outputScale, !pixelArt};
    float n = std::ceil(1.f / outputScale - 0.001f);
    if (n >= 2.f && 1.f / n <= outputScale && 1.f / n >= outputScale * 0.94f) outputScale = 1.f / n;
    return {outputScale, false};
}

// Respond to sustained load, ignoring startup, map loads, menus and isolated
// spikes. GPU time excludes the frame limiter and the presentation wait.
class AdaptiveQuality
{
public:
    float scale = 1.f;
    float presentationScale = 1.f;
    int reduction = 0;
    int fps = 60;
    double averageMs = 0;
    double frameAverageMs = 0;
    void Reset(const Settings &s, bool manualRaytracing = false)
    {
        scale = s.initialScale;
        presentationScale = 1.f;
        reduction = 0;
        fps = s.fps;
        guardRaytracingRecovery = s.raytracing || manualRaytracing;
        RestartMeasurements();
    }
    void RestartMeasurements()
    {
        averageMs = 0;
        frameAverageMs = 0;
        warmup = 3;
        over = under = 0;
        samples = 0;
    }
    bool Observe(double gpuMs, double seconds, int requestedFps, bool gameplay)
    {
        if (!gameplay || !std::isfinite(seconds) || seconds <= 0 || seconds > 0.25)
        {
            RestartMeasurements();
            return false;
        }
        if (!std::isfinite(gpuMs) || gpuMs <= 0 || gpuMs > 200) return false;
        averageMs = averageMs == 0 ? gpuMs : averageMs + std::min(seconds * 3, 0.25) * (gpuMs - averageMs);
        frameAverageMs = frameAverageMs == 0 ? seconds * 1000 : frameAverageMs + std::min(seconds * 3, 0.25) * (seconds * 1000 - frameAverageMs);
        ++samples;
        warmup -= seconds;
        if (warmup > 0 || samples < 60) return false;
        double budget = 1000.0 / fps;
        bool missingFrames = frameAverageMs > budget * 1.06 && averageMs > budget * 0.60;
        over = averageMs > budget * 0.90 || missingFrames ? over + seconds : 0;
        under = averageMs < budget * 0.65 && frameAverageMs <= budget * 1.03 ? under + seconds : 0;
        if (over >= 1.5)
        {
            // Once a modest pixel reduction stops helping, remove the most
            // expensive full-output effects before sacrificing more detail.
            if (scale > 0.821f) scale = std::max(0.82f, scale - 0.06f);
            else if (reduction == 0) reduction = 1;
            else if (scale > 0.701f) scale = std::max(0.7f, scale - 0.06f);
            else if (reduction < 3) ++reduction;
            else if (fps > 60) fps = 60;
            // TAA, tone mapping and other passes run at output resolution.
            // A scene-only scale cannot fix a bottleneck in those passes.
            else if (presentationScale > 0.501f) presentationScale = std::max(0.5f, presentationScale - 0.1f);
            else return false;
            Settle(2);
            return true;
        }
        if (under >= 8)
        {
            if (fps < requestedFps && averageMs < 1000.0 / requestedFps * 0.65)
            {
                fps = requestedFps;
            }
            else if (presentationScale < 0.999f) presentationScale = std::min(1.f, presentationScale + 0.1f);
            // Restoring RT can cost much more than the raster fallback. Do
            // not bounce between them just because raster fits comfortably.
            else if (guardRaytracingRecovery && reduction == 3 && averageMs >= budget * 0.35) return false;
            else if (reduction > 1) --reduction;
            else if (reduction == 1 && scale < 0.819f) scale = std::min(0.82f, scale + 0.03f);
            else if (reduction == 1) reduction = 0;
            else if (scale < 0.999f) scale = std::min(1.f, scale + 0.03f);
            else return false;
            Settle(3);
            return true;
        }
        return false;
    }
private:
    bool guardRaytracingRecovery = false;
    double warmup = 3, over = 0, under = 0;
    int samples = 0;
    void Settle(double seconds) { warmup = seconds; over = under = 0; samples = 0; }
};
}
