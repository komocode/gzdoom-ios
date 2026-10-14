#include "ios_hardware_policy.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace AppleGraphics;
constexpr uint64_t GiB = 1024ull * 1024 * 1024;

static void Feed(AdaptiveQuality &q, double ms, double seconds, int fps = 120, bool gameplay = true)
{
    for (int i = 0; i < int(seconds * 120); ++i) q.Observe(ms, 1.0/120, fps, gameplay);
}

int main()
{
    Capabilities phone{10, 114*GiB/10, 120, false, false, false, false, true, true};
    Capabilities mini{9, 74*GiB/10, 60, false, false, true, false, true, true};
    Capabilities m4{9, 74*GiB/10, 120, false, false, true, false, true, true};
    Capabilities mac{7, 32*GiB, 120, false, false, false, true, true, true, 21*GiB};
    auto p = Select(phone, 0, false), i = Select(mini, 0, false);
    auto pro = Select(m4, 0, false), max = Select(mac, 0, false);
    assert(p.preset == 4 && p.fps == 60 && p.raytracing && p.ssao == 3);
    assert(i.preset == 3 && i.fps == 60 && i.raytracing);
    assert(pro.preset == 4 && pro.fps == 60 && pro.raytracing && pro.maxRenderHeight == 1600);
    assert(max.preset == 4 && max.fps == 60 && !max.raytracing);
    assert(max.maxRenderHeight == 1600);
    auto taaOnly = mini;
    taaOnly.temporalUpscaler = false;
    auto taaProfile = Select(taaOnly, 0, false);
    assert(taaProfile.outputScale == 1 && taaProfile.preset == 3 && taaProfile.raytracing);
    assert(taaProfile.maxRenderHeight == i.maxRenderHeight);
    auto unsupported = phone;
    unsupported.raytracingFromRender = false;
    assert(!Select(unsupported, 0, false).raytracing);
    unsupported = phone;
    unsupported.simulator = true;
    assert(!Select(unsupported, 0, false).raytracing);
    assert(Select(unsupported, 0, false).preset == 2);
    auto smallMac = mac;
    smallMac.gpuWorkingSetBytes = 6*GiB;
    assert(Select(smallMac, 0, false).preset == 3);
    assert(Select({3, 3*GiB, 60, false}, 0, false).maxRenderHeight == 720);
    assert(Select({5, 3*GiB, 60, false}, 0, false).preset == 1);
    assert(Select({8, 37*GiB/10, 60, false}, 0, false).maxRenderHeight == 1080);
    assert(Select({7, 55*GiB/10, 120, false}, 0, false).preset == 3);
    // TV estimates use usable-memory ranges, not exact model RAM. Even a 4K
    // display must keep the full postprocess chain within the chip's budget.
    for (auto tv : {Capabilities{2, 18*GiB/10, 60, false, true},
                    Capabilities{3, 28*GiB/10, 60, false, true},
                    Capabilities{5, 28*GiB/10, 60, false, true},
                    Capabilities{8, 37*GiB/10, 60, false, true}})
    {
        auto s = Select(tv, 0, false);
        int preset = tv.appleFamily >= 8 ? 3 : tv.appleFamily >= 3 ? 2 : 1;
        assert(s.preset == preset && s.fps == 60 && s.msaa == 1);
        assert(s.fxaa == (preset < 3 ? 1 : 0));
        assert(s.maxRenderHeight == (preset == 1 ? 720 : 1080));
        assert(!s.raytracing && s.outputScale == 1);
        AdaptiveQuality recovering;
        recovering.Reset(s);
        recovering.reduction = 3;
        Feed(recovering, 8, 30, 60);
        assert(recovering.reduction < 3); // Shadow maps need no extra RT recovery margin.
        for (int edge : {720, 1080, 2160})
        {
            auto r = SelectResolution(s, edge*16/9, edge, s.initialScale);
            assert(r.outputScale <= 1 && edge*r.outputScale <= s.maxRenderHeight + 0.01f);
            AdaptiveQuality feedback;
            feedback.Reset(s);
            Feed(feedback, 30, 40, 60);
            auto reduced = SelectResolution(s, edge*16/9, edge, feedback.scale, feedback.presentationScale);
            assert(feedback.reduction == 3 && feedback.fps == 60);
            assert(reduced.outputScale*reduced.sceneScale < r.outputScale*r.sceneScale);
        }
        tv.refreshRate = 50;
        assert(Select(tv, 0, false).fps == 50); // Match a 50-Hz HDMI mode.
        auto fair = Select(tv, 1, false), hot = Select(tv, 2, false);
        assert(fair.preset <= 2 && !fair.bloom && fair.ssao == 0);
        assert(hot.preset == 1 && hot.fps == 35 && hot.maxRenderHeight == 720);
        tv.simulator = true;
        assert(Select(tv, 0, false).preset == 2 && !Select(tv, 0, false).raytracing);
    }
    assert(Select({8, 28*GiB/10, 60, false, true}, 0, false).preset == 2);
    Capabilities futureTV{10, 8*GiB, 60, false, true, false, false, true, false};
    auto futureTVSettings = Select(futureTV, 0, false);
    assert(futureTVSettings.preset == 3 && futureTVSettings.maxRenderHeight == 1440);
    assert(futureTVSettings.raytracing && futureTVSettings.outputScale == 1);
    futureTV.raytracingFromRender = false;
    assert(!Select(futureTV, 0, false).raytracing);
    for (auto c : {phone, mini, m4, mac})
    {
        auto low = Select(c, 0, true), fair = Select(c, 1, false), hot = Select(c, 2, false);
        assert(low.fps <= 60 && !low.raytracing && low.ssao == 0 && !low.bloom);
        assert(fair.preset <= 2 && !fair.raytracing);
        assert(hot.fps == 35 && hot.maxRenderHeight == 720 && hot.preset == 1);
    }
    // Orientation must not change the pixel budget; high presets preserve HUD
    // resolution while scaling only 3D. Smaller displays must not supersample.
    auto landscape = SelectResolution(p, 2868, 1320, p.initialScale);
    auto portrait = SelectResolution(p, 1320, 2868, p.initialScale);
    assert(landscape.outputScale == portrait.outputScale && landscape.sceneScale == portrait.sceneScale);
    assert(landscape.outputScale == 1 && landscape.sceneScale < 1);
    auto small = SelectResolution(p, 800, 600, 1);
    assert(small.outputScale == 1 && small.sceneScale == 1);

    AdaptiveQuality q;
    // Exercise an optional 120-Hz profile independently of mobile defaults.
    p.fps = 120;
    q.Reset(p);
    Feed(q, 30, 2); // Startup and one slow frame cannot change quality.
    assert(q.scale == p.initialScale && q.reduction == 0);
    q.Observe(150, 1.0/120, 120, true);
    Feed(q, 4, 2);
    assert(q.scale == p.initialScale && q.reduction == 0);
    Feed(q, 30, 6);
    assert(q.scale < p.initialScale);
    Feed(q, 30, 50);
    assert(q.scale >= 0.7f && q.reduction == 3 && q.fps == 60);
    Feed(q, 30, 30);
    assert(q.presentationScale < 0.6f);
    auto outputBound = SelectResolution(p, 2868, 1320, q.scale, q.presentationScale);
    assert(outputBound.outputScale < landscape.outputScale);
    float old = q.scale;
    Feed(q, 1, 30, 120, false); // Menus do not falsely restore quality.
    assert(q.scale == old && q.reduction == 3 && q.fps == 60);
    q.Observe(1, 2, 120, true); // Background/map-load gap is ignored.
    assert(q.scale == old);
    q.Observe(std::numeric_limits<double>::quiet_NaN(), 1.0/120, 120, true);
    assert(std::isfinite(q.averageMs));
    Feed(q, 2, 240);
    assert(q.scale > 0.99f && q.reduction == 0 && q.fps == 120);
    assert(q.presentationScale > 0.99f);
    q.reduction = 3;
    q.fps = 60;
    q.RestartMeasurements();
    Feed(q, 9, 30, 60);
    assert(q.reduction == 3); // Raster headroom alone must not flap RT on/off.
    q.Reset(max, true);
    q.reduction = 3;
    Feed(q, 9, 30, 60);
    assert(q.reduction == 3); // The guard also covers explicitly requested software RT.
    q.Reset(i);
    Feed(q, 100, 90, 60);
    assert(q.fps == 60); // Display rate and 60-FPS floor are respected.
    q.Reset(Select(phone, 2, false));
    Feed(q, 100, 90, 35);
    assert(q.fps == 35);
    q.Reset(p);
    Feed(q, 8, 30); // Near budget: do not oscillate on minor fluctuations.
    float settled = q.scale;
    for (int n=0; n<300; ++n) q.Observe(n%2 ? 7 : 8, 1.0/120, 120, true);
    assert(q.scale == settled);
    q.Reset(i);
    for (int n=0; n<700; ++n) q.Observe(13.5, 0.0185, 60, true);
    assert(q.scale < i.initialScale || q.reduction > 0); // GPU + CPU miss the frame deadline.
    q.Reset(i);
    for (int n=0; n<700; ++n) q.Observe(3, 1.0/30, 60, true);
    assert(q.scale == i.initialScale && q.reduction == 0); // CPU-bound: fewer pixels won't help.

    for (int family=2; family<=12; ++family)
        for (int mem=2; mem<=32; ++mem)
            for (int heat=0; heat<4; ++heat)
                for (int hz : {60,120})
                    for (bool low : {false,true})
                    {
                        auto s = Select({family, mem*GiB, hz, false}, heat, low);
                        assert(s.fps >= 35 && s.fps <= hz);
                        assert(s.maxRenderHeight >= 720 && s.maxRenderHeight <= 1600);
                        assert(s.msaa == 1 || s.msaa == 2);
                        assert(s.ssao >= 0 && s.ssao <= 3);
                        assert(s.preset < 3 ? s.outputScale == 1 : s.msaa == 1);
                        for (int edge : {480, 1080, 2160})
                        {
                            auto r = SelectResolution(s, edge*2, edge, 0.7f);
                            assert(r.outputScale > 0 && r.outputScale <= 1);
                            assert(r.sceneScale >= 0.5f && r.sceneScale <= 1);
                        }
                    }
    puts("Apple capability profiles and adaptive GPU tuning checks passed.");
}
