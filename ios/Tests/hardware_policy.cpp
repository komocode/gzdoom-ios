#include "ios_hardware_policy.h"
#include <cassert>
using namespace IOSHardware;
int main()
{
    constexpr uint64_t GiB = 1024ull * 1024 * 1024;
    // Oldest supported iPad class (A10) and iPhone class (A12), through M-series.
    auto a10 = Select({3, 3*GiB, 60, false}, 0, false);
    assert(a10.fps == 60 && a10.maxRenderHeight == 720 && a10.msaa == 1);
    auto a12 = Select({5, 3*GiB, 60, false}, 0, false);
    assert(a12.ssao == 0 && !a12.bloom);
    auto a15 = Select({8, 4*GiB, 60, false}, 0, false);
    assert(a15.fps == 60 && a15.msaa == 2 && a15.maxRenderHeight == 1080);
    auto m1 = Select({7, 8*GiB, 120, false}, 0, false);
    assert(m1.fps == 120 && m1.ssao == 1 && m1.msaa == 2);
    auto m4 = Select({9, 16*GiB, 120, false}, 0, false);
    assert(m4.fps == 120 && m4.msaa == 4 && m4.maxRenderHeight == 1600);
    assert(Select({8, 37*GiB/10, 60, false}, 0, false).maxRenderHeight == 1080);
    assert(Select({7, 55*GiB/10, 120, false}, 0, false).fps == 120);
    assert(Select({9, 74*GiB/10, 120, false}, 0, false).msaa == 4);
    auto low = Select({9, 16*GiB, 120, false}, 0, true);
    assert(low.fps == 60 && low.msaa == 1 && low.ssao == 0 && !low.bloom);
    auto hot = Select({9, 16*GiB, 120, false}, 2, false);
    assert(hot.fps == 35 && hot.maxRenderHeight == 720);
    auto sim = Select({9, 32*GiB, 120, true}, 0, false);
    assert(sim.fps == 60 && sim.maxRenderHeight == 900);
    auto tvA8 = Select({2, 2*GiB, 60, false, true}, 0, false);
    assert(tvA8.maxRenderHeight == 720 && tvA8.msaa == 1 && tvA8.ssao == 0);
    auto tvA10X = Select({3, 3*GiB, 60, false, true}, 0, false);
    assert(tvA10X.maxRenderHeight == 1080 && tvA10X.msaa == 2);
    auto tvA12 = Select({5, 3*GiB, 60, false, true}, 0, false);
    assert(tvA12.maxRenderHeight == 1440 && tvA12.ssao == 1);
    auto tvA15 = Select({8, 4*GiB, 60, false, true}, 0, false);
    assert(tvA15.maxRenderHeight == 2160 && tvA15.fps == 60);
    assert(Select({8, 4*GiB, 60, false, true}, 2, false).maxRenderHeight == 720);
    for (int family=3; family<=12; ++family)
        for (int mem=2; mem<=32; ++mem)
            for (int heat=0; heat<4; ++heat)
                for (int hz : {60,120})
                {
                    auto s = Select({family, mem*GiB, hz, false}, heat, false);
                    assert(s.fps >= 35 && s.fps <= hz);
                    assert(s.maxRenderHeight >= 720 && s.maxRenderHeight <= 1600);
                    assert(s.msaa == 1 || s.msaa == 2 || s.msaa == 4);
                }
}
