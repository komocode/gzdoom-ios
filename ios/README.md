# Apple build details

See the [project README](../README.md) for supported platforms and major changes.

## Toolchain and dependencies

Install Xcode and its command line tools, CMake, Ninja, and Python 3. The build
script downloads and verifies SDL 2.32.10, MoltenVK 1.4.2, SPIRV-Cross
(vulkan-sdk-1.4.309.0), OpenAL Soft 1.25.2, and libvpx 1.15.2 in ignored build
directories. A tracked OpenAL allocator patch
fixes small-alignment allocations on Apple platforms.

The portable Apple builds exclude FluidSynth's desktop GLib dependency. OPL is
the mobile music default; the Mac default is SF2-capable TiMidity++. Standard
desktop builds can still enable `ZMUSIC_WITH_FLUIDSYNTH`.

## Targets and signing

```sh
./scripts/apple.py ios --test-wads
./scripts/apple.py simulator --test-wads
./scripts/apple.py macos --universal --test-wads --run
./scripts/apple.py tvos --test-wads
./scripts/apple.py tvos-simulator --test-wads
./scripts/apple.py watchos --test-wads
./scripts/apple.py watchos-simulator --test-wads
```

Add `--run --device UDID` to install and launch on a physical device or an
already-booted simulator. List devices with `xcrun devicectl list devices` or
`xcrun simctl list devices`. Physical devices must trust the Mac and have Developer
Mode enabled. Installation uses a valid wildcard development provisioning profile
covering the device and its matching Apple Development identity in Xcode.
Specify `--profile /path/to/profile` to select a profile explicitly.

`--arch arm64` or `--arch x86_64` selects a Mac/simulator architecture; physical
devices use arm64. `--jobs N` controls build concurrency. `--universal` builds both
Mac architectures into `build-macos-universal/gzdoom.app`. Other apps are written
under `build-TARGET-ARCH/engine/`: `gzdoom.app` for iOS, Mac, and TV, or
`gzdoom-watch.app` for Watch.

Watch requires the arm64 app ABI available on compatible newer Watches running
watchOS 26+, including Series 9 and Ultra 2. GZDoom requires 64-bit pointers;
older Watches restricted to arm64_32 applications are unsupported.

## Games, controls, and storage

On iPhone/iPad, select an IWAD in the native game library or use **Import WAD**.
Files app / Finder file sharing exposes Documents; game archives go in **Games**.
Savegames, screenshots, demos, and `engine.log` use Documents. Settings and caches
use Application Support / Caches.

The left stick moves; dragging the right side turns the view. Fire, Use, Jump,
Weapon, Menu, and Save support simultaneous touches. In menus, the stick navigates
and Fire / OK selects. SDL handles external controllers and keyboards. Native
text entry supports save names, settings, and console commands. Input is released
and the game paused when the app loses focus. iPad windows support rotation and
resizing.

Apple TV provides a game library and extended-gamepad input. Data uses tvOS's
purgeable Caches directory; persistent asset delivery and save backup remain release
work.

Watch selects a game and displays its first level using GZDoom's true-color
software renderer at the panel's native width, with filtered textures, dynamic
lights and a small CPU post-process (see [Apple rendering](../docs/apple-rendering.md)). Gameplay controls, audio, and GPU effects
are unavailable in this preview. Rendering pauses in the background and keeps
only the latest frame. The frame budget is 35 FPS normally, 25 in Low Power Mode
or fair heat, and 18 at serious/critical heat. Custom mod UI remains unvalidated.

## Hardware profiles

The optional modern rendering pipeline (baked GI, SSR, TAA and the `r_preset`
quality presets) is described in [Apple rendering](../docs/apple-rendering.md).

iPhone/iPad settings use the Metal GPU family, reported RAM, display refresh rate,
Low Power Mode, and thermal state.

| Hardware class | Starting budget | Effects |
| --- | --- | --- |
| A10/A12 or less than 3 GiB RAM | 60 FPS, up to 720p | 1x samples, 4x anisotropy |
| Apple family 6+ in the 4 GB RAM class | 60 FPS, up to 1080p | 2x samples, 8x anisotropy |
| Apple family 7+ in the 6 GB RAM class | Up to 120 Hz, 1440p 3D | TAA with temporal upscaling to up to 1.25x, 16x anisotropy, SSAO and bloom |
| Apple family 9+ in the 8 GB RAM class | Up to 120 Hz, 1600p 3D | TAA with temporal upscaling to up to 1.25x, 16x anisotropy, SSAO and bloom |

Apple TV budgets range from 720p / 1x samples on A8 to 1080p / 2x on A10X,
1440p 3D with TAA on A12, and 2160p with TAA on A15 or newer, at up to 60 FPS. Rendering is
limited by drawable resolution, supported sample counts, and GPU anisotropy limits.
Simulators use reduced budgets. Low Power Mode and heat lower quality; normal
settings return when conditions improve. These are quality budgets, not guaranteed
frame rates.

The native Metal renderer is the default (`vid_preferbackend 4`); Vulkan through
MoltenVK remains selectable. On GPUs with Metal ray tracing, Metal
traces dynamic-light shadows when `vk_raytrace` is on. MetalFX performs temporal
upscaling where available (not on tvOS). Mobile builds also exclude desktop
OpenGL, executable-memory JIT, desktop Discord integration, and process
launching.

## Validation

Run the hardware-policy checks without building the app:

```sh
xcrun clang++ -std=c++17 -Isrc/common/platform/ios \
  ios/Tests/hardware_policy.cpp -o /tmp/gzdoom-hardware-policy
/tmp/gzdoom-hardware-policy
```

The checks cover GPU/memory tiers, 60/120 Hz displays, Apple TV generations,
simulator reporting, Low Power Mode, and thermal states. See the project README
for runtime coverage and remaining validation work. Test WADs are included only
with `--test-wads`; an ordinary build removes previously bundled test WADs.
