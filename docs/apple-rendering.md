# Modern rendering on Apple platforms

This port adds an optional modern lighting pipeline to GZDoom's hardware
renderer, a native Metal renderer for it (with ray traced shadows and MetalFX
upscaling), and a better-looking software path for Apple Watch. Everything sits behind independent toggles. `r_preset 0` keeps
the classic GZDoom presentation, and no feature changes gameplay or map data.

The approach favours what pays off most per unit of GPU time on tile-based
Apple GPUs:

- Bake what doesn't move.
- Reuse the passes GZDoom already has.
- Keep screen-space effects at half resolution.
- Avoid extra full-resolution render targets.

## Features

| Feature | CVARs | How it works | Cost |
|---|---|---|---|
| Baked GI (indirect bounce, colour bleeding) | `r_gi`, `r_gi_strength`, `r_gi_saturation`, `r_gi_quality`, `r_gi_rebake` | Irradiance probe grid. Each probe holds an ambient cube (6 colours), baked once per level on background threads by ray tracing the level mesh with a BVH. Hits return albedo × (sector light + static dynamic lights, with shadow rays). The scene shader adds part of the bounce above the sector light level and tints by its chroma. Doom light levels stay the floor. | E1M1: 97×60×10 probes in about 0.12 s on M1 Max. One 3D texture fetch ×3 per pixel. |
| Soft dynamic-light shadows | `gl_light_shadowmap`, `gl_shadowmap_filter`, `gl_shadowmap_quality` | Contact-hardening filter on GZDoom's 1D shadow maps. A blocker search estimates the occluder distance, then a jittered, tent-weighted PCF widens the penumbra with it. Used in modern mode when the filter is above 0. | 6 + 4–10 taps per shadowed light |
| Texture relief ("auto normal") | `r_autonormal`, `r_autonormal_strength` | Normals from the luminance gradient of the diffuse texture, used only for dynamic lights. Classic light intensities are kept: relief modulates them. | ALU only |
| Specular | `r_specular`, `r_specular_strength` | Normalized Blinn-Phong from dynamic lights. Gloss (inverse roughness) is estimated from texel brightness and saturation; liquids and floors raise it. Metalness is estimated too: mid-to-bright, low-saturation texels (steel, brass, copper) tint their highlights with their own colour, as PBR metals do. Highlights are unclamped so they feed bloom. | ALU only |
| PBR material maps | existing GZDoom `normal`, `specular`, `metallic`, `roughness`, `ao` in GLDEFS | Authored maps still take priority. The automatic texture-map generation is done in the shader rather than as files: normal (relief), roughness (gloss), metallic and height (parallax) are all derived from the diffuse texture. | — |
| Parallax occlusion mapping | `r_parallax` | 10-step steep parallax from the luminance height. Only solid walls and opaque flats opt in (`uMaterialParams.z`), so sprites and masked mid textures keep their exact shapes. Fades out between 256 and 640 units. | ≤ 10 texture taps, near surfaces only |
| Screen-space reflections | `r_ssr` (1 liquids, 2 + glossy floors), `r_ssr_strength` | Reflectivity is packed into the spare alpha of the existing SceneFog G-buffer target, so no new target is needed. Half-resolution view-space ray march with binary refinement, then composited into the scene. Doom's animated liquids are recognised by flat name, since Doom's TERRAIN lump doesn't mark them. | Half res, 24–40 steps |
| Screen-space GI | `r_ssgi`, `r_ssgi_strength` | 4 cosine-distributed rays × 8 steps per half-resolution pixel gather nearby lit surfaces, tinted by the receiver's hue and added to the scene. Complements the baked probes with bounce from moving lights, muzzle flashes and explosions. Shares inputs with SSR and needs no temporal history. | Half res, 32 taps |
| Ambient occlusion | `gl_ssao` | GZDoom's existing HBAO-style SSAO. The presets drive its quality, and it shares linear depth with SSR and volumetrics. | existing |
| Volumetric light | `r_volumetric`, `r_volumetric_strength` | Analytic in-scattering for the 7 most important dynamic lights near the view (closed-form integral along each view ray, clipped by scene depth). No froxel grid. | Half res, ≤ 7 lights |
| HDR / tone mapping | `gl_tonemap 6` (ACES filmic) | ACES (Narkowicz fit) added to the existing tonemap choices, together with the existing auto-exposure. | existing pass |
| Contact shadows | `r_actorspriteshadow 2`, `r_actorspriteshadowfadeheight` | GZDoom's projected sprite shadows, always on from medium up and faded with height, plus AO. | existing |
| Temporal anti-aliasing | `r_taa` | The 3D projection is jittered by a subpixel Halton(2,3) offset each frame. The resolve reprojects last frame's HDR result through the depth buffer and camera motion, clips it to the current 3x3 neighbourhood in YCoCg (rejecting stale history from moving sprites and disocclusions, which have no motion vectors) and blends with luminance weights. Runs before bloom and tone mapping. History resets on teleports, menus and resolution changes. Replaces FXAA from High up. | 1 full-res pass + copy |
| Temporal upscaling | `r_upscale` (0.5–1, 1 = off; needs `r_taa`), `r_upscale_metalfx` | The 3D scene renders into a smaller part of the scene viewport and is reconstructed to the full viewport. On the Metal renderer this is MetalFX's temporal upscaler, fed with the HDR colour, depth and camera motion vectors computed from depth and the previous camera. Elsewhere (or with `r_upscale_metalfx 0`) the TAA resolve does it: each output pixel is a Gaussian-weighted sum of the nearby jittered input samples, and the history counts for more where no sample landed near the pixel. More jitter phases are used while upscaling (16, or 32 below 0.7). Exposure, bloom, tone mapping, the weapon sprite and the HUD all run at output resolution, unlike `vid_scalefactor`, which scales the HUD too. | Scene cost × scale²; resolve replaces TAA |
| Ray traced shadows | `vk_raytrace` (Metal renderer; restart) | On GPUs with Metal ray tracing, the scene shaders trace shadow rays for dynamic lights against an acceleration structure over the level mesh, instead of GZDoom's 2D shadow maps. Unlike the shadow maps, they respect height, 3D floors and overhangs. `gl_shadowmap_filter` sets the soft-shadow sample count. Auto mode enables this only on Apple9+ GPUs with render-stage ray tracing (A17 Pro / M3 and later). Manual mode remains available. | 1–27 rays per shadowed light and pixel |
| Modern bloom | `gl_bloom`, `r_bloom_modern` | 13-tap downsample chain (6 levels) with a soft-knee threshold, then a tent-filtered additive upsample. Fewer passes than the classic Gaussian bloom and no flicker on small highlights. | ~12 small passes |
| Sharpen, vignette, grain | `r_sharpen`, `r_vignette`, `r_grain` | One pass: contrast-adaptive sharpening (CAS), a vignette, and triangular grain that also removes banding. | one full-screen pass |
| GPU particles | `r_gpuparticles` (1 normal, 2 dense) | Visual-only and stateless. Hitscan shots draw a tracer, eject a brass casing that lies on the floor for a few seconds and leave a wisp of gun smoke. Bullet impacts throw sparks, chips and a lingering dust cloud in the colour of the surface that was hit, sprayed along its normal and faded softly where they meet that surface and the floor. Blood throws droplets, explosions throw a fireball, embers, debris and rising smoke, and glowing projectiles burst in their light's colour. Each particle is written once into a ring of persistent vertex memory; the vertex shader evaluates the motion in closed form (gravity, drag, resting on the floor, sparks stretched along their velocity), so the CPU simulates nothing per frame. Game particles, RNG, savegames and netplay are untouched. | Draws only the live part of a 16384-particle ring; 2 draws |
| Planar reflections | `gl_plane_reflection`, `gl_mirrors` | Already in GZDoom for mirrors and reflective planes; left as is. SSR adds reflections where maps don't author them. | existing |

### Not implemented, and why

- **Per-object motion vectors.** GZDoom doesn't produce them, so TAA and both
  upscalers reproject with depth and camera motion only, and rely on
  neighbourhood clipping (or MetalFX's own rejection) for moving sprites.
- **Ray tracing on Vulkan through MoltenVK.** MoltenVK does not expose ray
  queries; ray traced shadows need the Metal renderer.
- **Nanite-style geometry.** Not useful for Doom-scale geometry.

## Metal renderer

`vid_preferbackend 4` (the default on Apple platforms; **Options → Display →
Video mode → Preferred rendering API**) selects a native Metal renderer. Vulkan
through MoltenVK (`vid_preferbackend 1`) stays available.

- **Same shaders.** GZDoom's GLSL is assembled exactly as for Vulkan,
  compiled to SPIR-V by glslang and translated to Metal Shading Language by
  SPIRV-Cross at startup. The Vulkan descriptor layout maps onto fixed Metal
  buffer, texture and sampler slots, so shader changes (including mod material
  shaders and custom postprocess shaders) work on both renderers.
- **Same structure.** The device, render state, buffers, textures, render
  targets and postprocess passes mirror the Vulkan backend
  (`src/common/rendering/metal/`), so the hardware renderer, the 2D drawer and
  every postprocess pass run unchanged. Like the Vulkan backend, it waits for
  the GPU at the end of each frame.
- **Unified memory.** Buffers and uploaded textures use shared storage: no
  staging copies. Mipmaps are generated on the GPU in a separate upload command
  buffer that runs before the frame.
- **Features only Metal has here:** MetalFX temporal upscaling and ray traced
  shadows (above), and MSAA through Metal's resolve. Simulator SDKs and tvOS
  have no usable MetalFX; there the TAA resolve upscales.
- **Cleared targets.** New render targets and postprocess textures start at
  zero. Stale memory there can hold NaNs, which every blended draw keeps
  (NaN × 0 is NaN); 2D-only screens such as the title then came out magenta
  until something rewrote the target. The Vulkan pipeline images are cleared
  the same way.
- **Checked:** Metal API validation is clean through startup, menus, level
  changes and wipes, savegames, screenshots, MSAA, every preset, particles,
  upscaling and ray tracing. With a point light placed beside an E1M1 pillar,
  the traced shadow falls diagonally across the floor below, where the shadow
  map shades a full-height band. Frames match the Vulkan renderer: Classic is
  pixel-identical; Ultra differs on 0.1–0.3% of pixels (MSAA included), from
  exposure adaptation timing.

Not done yet: HDR output (`vk_hdr`), and memoryless G-buffer attachments,
which need the postprocess passes that read the G-buffer to move into tile
shaders. GPU frame time is shown with `stat gpu`.

## Light definitions

Apple builds default `autoloadlights` and `autoloadbrightmaps` to on (upstream
default: off, enabled from the Windows/Linux startup dialog, which the Apple
port doesn't have). Without them, the stock games have no dynamic lights for
the specular, volumetric and baked-GI passes to use. In E1M1 the GI bake goes
from 0 to 49 static lights.

## Quality presets

`r_preset` sets every feature at once, and each CVAR can be changed
afterwards. Choosing a preset in the menu or console turns `r_preset_auto`
off, which keeps the platform from changing it again.

| Preset | GI | Relief / specular | Parallax | SSAO | SSR | SSGI | Volumetric | Tonemap | Shadow filter / size | Sprite shadows | Particles | AA |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 Classic | – | – | – | off | off | – | off | off | 1 / 128 | default | – | – |
| 1 Low | ✓ | ✓ | – | off | off | – | off | ACES | 1 / 128 | default | normal | – |
| 2 Medium | ✓ | ✓ | – | 1 | liquids | – | half | ACES | 1 / 256 | always | normal | – |
| 3 High | ✓ | ✓ | ✓ | 2 | + floors | – | half | ACES | 2 / 512 | always | dense | TAA |
| 4 Ultra | ✓ | ✓ | ✓ | 3 | + floors | ✓ | full | ACES | 3 / 1024 | always | dense | TAA |

All presets above Classic enable the modern bloom and dynamic-light shadow
maps. The menu is under **Options → Display → Modern Rendering**.

### Automatic per-device choice and GPU tuning

`r_preset_auto` (default on) selects a capability-based starting budget. The
native Metal backend also uses completed command-buffer GPU timings to tune it.
These are estimates for stock maps, **not measured FPS guarantees**:

| Device class | Initial preset | FPS target | Maximum 3D short edge | Automatic RT |
|---|---|---|---|---|
| A10 / A12 | Low | 60 | 720 | off |
| A13+ with at least 3 GiB usable RAM | Medium | 60 | 1080 | off |
| A14 / M1 iPad with at least 5 GiB | High | 60 | 1440 | off |
| A17 Pro iPad mini | High | 60 | 1200 | on |
| A17 Pro / A18 phone class | High | 60 | 1200 | on |
| A19-class phone with at least 7 GiB | Ultra | 60 | 1440 | on |
| M-series Pro iPad with Apple9+ GPU | Ultra | 60 | 1600 | on |
| Apple Silicon Mac | High | 60 on M1/M2; up to 120 on Apple9+ | 1440 | Apple9+ only |
| Mac with at least 16 GiB RAM and 10 GiB GPU working-set recommendation | Ultra | 60 on M1/M2; up to 120 on Apple9+ | 1600 on M1/M2; 1800 on Apple9+ | Apple9+ only |
| Apple TV HD (A8) | Low | 60 | 720 | off |
| Apple TV 4K, 1st / 2nd generation (A10X / A12) | Medium | 60 | 1080 | off |
| Apple TV 4K, 3rd generation (A15) | High | 60 | 1080 | off |
| Newer TV GPU with Apple9+ and at least 5 GiB usable RAM | High | 60 | 1440 | only if render-stage RT is reported |

The display refresh rate caps every target. The Mac uses the active window's
display. GPU family, render-stage ray tracing, usable RAM, recommended GPU
working set, device type, and MetalFX support are queried at runtime. RAM alone
never implies hardware ray tracing. The M1 Max uses shadow maps automatically;
Metal supports software traversal there, but it has no dedicated RT hardware.
See [Apple's capability tables](https://developer.apple.com/metal/feature-sets/).

High and Ultra use TAA instead of MSAA. With MetalFX, output can be up to 1.25
times the 3D budget, capped by the display, while the HUD and post-processing stay
at output resolution. With the TAA fallback, the output is capped at the 3D
budget instead, keeping the same scene detail without an oversized postprocess
chain. The budget uses the **short edge**, so rotating a phone does not
multiply its GPU workload. Initial adaptive 3D scale is 85% of the available
budget with RT and 90% without; the controller can recover to 100%. For example,
on a 1320-pixel short edge the A19 phone starts around 1122 internal pixels.
Medium and Low scale the whole output when needed. MetalFX is queried before
use and falls back to the TAA resolve if creation fails. iOS/iPadOS 27.0.x uses
the GPU TAA upscaler: on the reference A19 Pro, MetalFX's asynchronous MPSGraph
neural-compiler prewarm aborts inside LLVM despite advertising support. This
failure cannot be caught as a nil return. Hardware ray tracing stays enabled;
macOS MetalFX remains available.

`r_adaptive_quality` (default on) waits three seconds and at least 60 gameplay
samples, then smooths GPU and presentation times. Sustained GPU load above 90%
of the frame budget, or missed frame deadlines with substantial GPU load,
reduces 3D scale in six-percentage-point steps to 82% of its ceiling. If that
is insufficient, it first reduces screen-space GI, volumetrics and shadow rays
before lowering pixels further to a 70% floor; then reflection coverage, parallax and AO; then
shadows and the remaining expensive effects. A persistently overloaded 120-Hz
profile can fall back to 60 FPS. If the full-output passes still exceed the
budget, output resolution can fall in 10% steps to 50% of its ceiling as a last
resort, including the HUD; output resolution recovers first. Eight seconds of substantial headroom are
required to recover a step, with settling time between changes. Restoring a
higher FPS target requires headroom at that higher target's budget too.

Menus, paused gameplay, inactive apps, background gaps, invalid timings and
isolated stalls do not drive the controller. A CPU-bound mod can still miss its
target: the controller avoids reducing pixels when GPU load is low, and does not
restore quality while presentation is already missing the target. Mobile RT starts with a single height-correct shadow ray per light; desktop
RT starts with nine and can drop to one, instead of assuming Ultra's 27 rays
fit every GPU.
Both traced and shadow-map scene shaders are compiled during startup. Thermal
and load changes can switch between those caches without recompiling in gameplay
or restarting. This removes RT shader register pressure, rather than merely
skipping shadow rays in the same shader. A larger recovery margin is required
before restoring RT to avoid repeated toggles; shadow-map-only devices use the
normal effect-recovery margin.

Low Power Mode or fair thermal pressure caps at Medium, 60 FPS and 1080 pixels,
with AO, bloom and automatic traced shadows off. Serious/critical pressure caps
at Low, 35 FPS and 720 pixels. Normal conditions restore the capability budget
and restart measurement. The Metal frame limiter also respects these caps when
VSync is enabled. The Vulkan fallback retains static capability/thermal budgets.

Choosing a preset manually disables automatic control. `r_adaptive_quality 0`
keeps the static device budget; `r_preset_auto 0` stops automatic changes.
`r_raytrace_auto 0` uses the existing `vk_raytrace` manual preference at the next
restart. The **Automatic quality FPS target** menu (`r_adaptive_targetfps`) lets
you opt into 120 FPS on a capable display, with more resolution/effect reductions;
0 keeps the fidelity-first estimate. All controls are in **Modern Rendering**. `r_apple_status` prints
the selected preset, target, 3D scale, effect reduction, active shadows, upscaler,
smoothed GPU milliseconds and observed presentation rate. `r_adaptive_debug 1`
logs these measurements every five seconds for validation (off by default, not
saved). A target is not a benchmark result.

The shared policy and feedback controller are in
`src/common/platform/apple/apple_graphics_policy.h`. The portable checks include
all four reference device classes, older GPUs, capability rejection, rotation,
thermal/power limits, sustained overload, recovery, pauses and invalid timings:

```sh
xcrun clang++ -std=c++17 -Isrc/common/platform/ios \
  ios/Tests/hardware_policy.cpp -o /tmp/gzdoom-hardware-policy
/tmp/gzdoom-hardware-policy
```

### Apple TV extrapolation

The TV budgets above are **unmeasured estimates**, revised using the phone/iPad
spot checks. Apple identifies the TV chips as
[A8](https://support.apple.com/en-gb/111928),
[A10X](https://support.apple.com/en-us/111929),
[A12](https://support.apple.com/en-us/111922), and
[A15](https://support.apple.com/en-us/111839). GPU family describes capabilities,
not a performance ranking: A10X should not automatically receive a lower budget
than A12. Both therefore start at Medium / 1080p; A15 adds
High's parallax, glossy reflections and TAA at the same pixel ceiling. TV High
starts at 90% scene scale (about 972p), and can recover to 1080p. Low/Medium
start at their pixel ceiling and use FXAA; none of these TV profiles uses MSAA.

A 3840×2160 render has four times the pixels of 1920×1080 before extra effects.
The measured A17 Pro mini already needed resolution/effect reductions, so the
older A15 is no longer assumed to sustain Ultra / 2160p. The TV display mode
does not raise its GPU budget: the app scales its budgeted output to the
display, including the HUD. tvOS uses the GPU TAA path for High rather than
MetalFX, and A8/A10X/A12/A15 have no hardware RT; they use shadow maps. No cooling
bonus is assumed (the A15 TV has a
[passive thermal design](https://www.apple.com/cl/environment/pdf/products/appletv/Apple_TV_4K_PER_Oct2022.pdf)).

All TV tiers target 60 FPS or the actual HDMI refresh rate, whichever is lower,
and run the same GPU/frame-time feedback and thermal policy as iOS. Simulators
remain on their separate conservative profile. Newer GPU capabilities can raise
the starting ceiling to 1440p; they do not imply native 4K or Ultra. These are
policy and build checks only: the native tvOS arm64, iOS arm64 and macOS arm64
builds passed, along with sanitized and optimized portable policy checks. This
revision was not run on a TV, so frame rate, image quality and sustained thermals
there remain unverified.

### Reference-device checks (2026-10-14)

The Mac arm64 and iOS arm64 builds pass, as do the portable policy/controller
checks with address/undefined-behavior sanitizers and with release optimization.
Short E1M1 runs on the available iPhone 17 Pro, A17 Pro iPad mini, M4 iPad and M1
Max established the starting estimates above. The M4 reached 60 FPS with traced
shadows active; the mini reached about 59 FPS after the raster/output fallback.
The phone reached 60 FPS under its thermal fallback in an earlier run; its final
sample was interrupted when the app was closed. These are local spot checks,
not guarantees for other maps or mods or a full sustained thermal benchmark.
The MetalFX compatibility guard comes from the phone's actual crash report:
MPSGraph's ANE prewarm compiler aborted while advertising temporal support.

## Apple Watch

The watch has no Metal for third-party games, so it runs GZDoom's true-color
software renderer, presented through SwiftUI. Changes:

- **Native resolution.** The frame is 4:3 at the panel's native pixel width
  (up to 480 px wide; `watch_renderscale` scales it down), shown 1:1 instead
  of being stretched up from 320×240.
- **Renderer quality.**
  - Bilinear magnification and mipmapped minification (`r_magfilter`,
    `r_minfilter`, `r_mipmap`).
  - Smooth sky (`r_linearsky`).
  - Coloured dynamic lights (`r_dynlights`) with the stock light definitions
    from `lights.pk3`, now bundled with the watch app.
- **CPU post-processing** (`watch_post`), about 20 operations per pixel:
  quarter-resolution bloom with a soft threshold, a filmic contrast curve with
  a little extra saturation, and a vignette.
- The frame limiter already drops to 25 or 18 fps under thermal pressure.

Minimum hardware: Apple Watch Series 9 / Ultra 2 or later (`arm64`, watchOS 26).
GZDoom needs a 64-bit address space, and Series 4–8 run `arm64_32`
(32-bit pointers), so they are not supported.

## Implementation notes

- `HWViewpointUniforms` gained `mRenderFeatures`, GI parameters and the probe
  volume origin and size. The per-draw `StreamData` uses a spare `vec4` as
  `uMaterialParams` (reflectivity, gloss, parallax).
- The irradiance volume is a `sampler2DArray` at fixed descriptor set
  binding 3, with layer = face × sizeZ + z, interpolated in z in the shader.
- The G-buffer's SceneFog alpha encodes reflectivity. Values above 0.5 mark a
  scene surface, and (1 − a) × 2 is its reflectivity, so cleared sky pixels
  never reflect.
- GPU particles share the scene shaders: `uMaterialParams.w > 0` switches
  `main.vp` and `main.fp` to the particle path (w is the current time, z picks
  the additive or alpha-blended pass). They are drawn after the translucent
  pass of the main view only, so mirrors and portals don't show them. There is
  no depth-buffer soft fade (the scene depth isn't readable during the scene
  pass on this backend); particles fade near the camera, and sparks and dust
  start just in front of the surface they came from.
- Post-process uniforms are Vulkan push constants. The volumetric block is
  kept at 256 bytes (`static_assert`) so it works outside MoltenVK too.
- Platform presets are applied through `R_SetAutoPreset`, which does nothing
  once the user picks a preset.
- Verified: E1M1 on an M1 Max Mac (all presets), the iPhone 16e simulator
  (automatic Medium, GI baked with 49 lights) and the Apple Watch Series 12
  simulator (416×312 with lights and post-processing), and the Apple TV 4K
  (2nd gen) simulator: Medium was chosen automatically (simulators report GPU
  family 2), GI baked in 0.26 s with 49 lights at 2160p, and with Ultra forced
  (SSGI and parallax on) it rendered with sprites intact.
  Ultra forced on the iPhone 16e simulator (TAA and SSGI on, render scale
  0.77) also rendered correctly. With a fresh config the iPhone simulator now
  starts on the Metal renderer, and the title loop, menus and E1M1 render
  correctly; the Apple TV simulator checks above predate it. The iOS, tvOS and
  simulator builds all compile it.
- TAA was checked on the Mac with static and turning captures: edges and
  texture shimmer resolve, and there is no ghosting or doubling while turning.
- GPU particles were checked on E1M1 on the Mac: floor and wall impacts (sparks,
  surface-coloured chips and dust), rocket explosions (fireball, embers, debris,
  smoke) and blood.
- Temporal upscaling was checked on the Mac at `r_upscale 0.5`: the image
  matches native TAA closely (texture detail is slightly softer), the HUD
  stays sharp, and turning shows no ghosting.
- The GI bake no longer depends on when the scene is captured: it uses the base
  texture rather than the current animation frame for albedo, flickering and
  glowing sectors at their average level, pulsing lights at their average
  radius, and a level mesh flag (`bSky`) that some surface types left
  uninitialised now defaults to false. Runs of the same map now bake the same
  volume.
- Testing note: console commands passed with `+` on the command line run before
  the window exists, so the automatic preset overrides them. Use
  `+"wait 2; r_preset 3; …"` when scripting captures.
