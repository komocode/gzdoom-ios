# GZDoom for Apple platforms

An Apple port of [GZDoom](https://github.com/ZDoom/gzdoom), with native iPhone,
iPad, Mac, and Apple TV builds, plus an Apple Watch rendering preview.

| Platform | Minimum OS | Requirements |
| --- | --- | --- |
| iPhone / iPad | iOS / iPadOS 18 | arm64 and Metal; no hardware ray tracing required |
| Mac | macOS 12 | Apple Silicon or Intel; universal builds available |
| Apple TV | tvOS 18 | Apple TV HD or compatible Apple TV 4K; game controller |
| Apple Watch | watchOS 26 | arm64 app support; rendering preview without gameplay controls or audio |

## Major changes

- Added a single build and install script for Apple devices and simulators, with
  checksum-pinned dependencies and development signing.
- Added an iPhone/iPad game library, WAD import, multitouch controls, controller
  and keyboard input, a native text-entry keyboard, and iPad rotation/resizing.
- Added a native Metal renderer (`vid_preferbackend 4`, the default on Apple). It
  runs the Vulkan backend's GLSL, translated to Metal Shading Language with
  SPIRV-Cross, and adds ray traced dynamic-light shadows on GPUs with Metal ray
  tracing and MetalFX temporal upscaling.
- Added GPU quality profiles based on Metal capabilities, memory, display refresh
  rate, thermal state, and Low Power Mode. Unsupported renderer features are
  disabled.
- Added Vulkan portability support (MoltenVK remains selectable), supported
  sample-count and anisotropy limits, texture upload synchronization fixes, and a
  base-vertex fallback for older GPU families and simulators.
- Added native tvOS packaging, controller input, and Apple TV quality profiles.
- Added Watch software rendering at the panel's native resolution with filtered
  textures, coloured dynamic lights and a light CPU post-process, plus a SwiftUI
  game picker and power/thermal-aware frame pacing. Watch builds exclude GPU
  effects, audio, and JIT execution.
- Added an optional modern rendering pipeline: baked irradiance-probe GI, soft
  dynamic-light shadows, derived normal/roughness/metallic/height detail, parallax
  occlusion mapping, screen-space reflections and GI, volumetric light, ACES tone
  mapping, progressive bloom, TAA with temporal upscaling, and GPU impact and
  explosion particles. Quality presets (`r_preset`, with Classic
  keeping the original look) are chosen per device. See
  [Apple rendering](docs/apple-rendering.md).
- Added portable Apple audio dependencies, fixed OpenAL effect/filter allocation,
  and packaged universal Mac apps without runtime Homebrew dependencies.
- Moved app data into writable platform directories and made local test WAD
  bundling an explicit build option.

## Build and run

Install Xcode and its command line tools, CMake, Ninja, and Python 3. Run these
commands from the repository root:

```sh
# Build and open the universal Mac app.
./scripts/apple.py macos --universal --test-wads --run

# Install on a trusted device with Developer Mode enabled.
xcrun devicectl list devices
./scripts/apple.py ios --test-wads --run --device DEVICE_UDID
./scripts/apple.py tvos --test-wads --run --device TV_UDID
./scripts/apple.py watchos --test-wads --run --device WATCH_UDID

# Install on an already-booted simulator.
./scripts/apple.py simulator --test-wads --run --device SIMULATOR_UDID
```

`--test-wads` bundles local files from `vendor/` for development. Omit it to build
without those assets. `vendor/`, build output, downloaded dependencies, caches,
and signing material are ignored by Git. The Mac launcher reuses a running app
instance; no server is required.

See [Apple build details](ios/README.md) for signing, simulator targets, hardware
profiles, controls, and validation commands.

## Development status

iPhone/iPad, Mac, and Apple TV gameplay and Watch rendering have been exercised.
Apple TV texture rendering is still under validation. Watch gameplay controls,
original release assets, and App Store preparation remain separate work.
Quality profiles are starting budgets; older-device performance, sustained thermal
behavior, and custom mod compatibility need broader testing.

## License and credits

GZDoom is licensed under [GPLv3](LICENSE). See [third-party licenses](docs/licenses)
and the bundled asset notices for additional terms. Replacing game assets does not
change the engine's license.

Copyright © 1998–2025 ZDoom / GZDoom teams and contributors. Doom source © 1997 id
Software, Raven Software, and contributors.

[GZDoom](https://zdoom.org/) · [Documentation](https://zdoom.org/wiki/) ·
[Community](https://forum.zdoom.org/)
