# Modern rendering on Apple platforms

This port adds an optional modern lighting pipeline to GZDoom's hardware
renderer (Vulkan through MoltenVK) and a better-looking software path for
Apple Watch. Everything sits behind independent toggles. `r_preset 0` keeps
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
| Modern bloom | `gl_bloom`, `r_bloom_modern` | 13-tap downsample chain (6 levels) with a soft-knee threshold, then a tent-filtered additive upsample. Fewer passes than the classic Gaussian bloom and no flicker on small highlights. | ~12 small passes |
| Sharpen, vignette, grain | `r_sharpen`, `r_vignette`, `r_grain` | One pass: contrast-adaptive sharpening (CAS), a vignette, and triangular grain that also removes banding. | one full-screen pass |
| Planar reflections | `gl_plane_reflection`, `gl_mirrors` | Already in GZDoom for mirrors and reflective planes; left as is. SSR adds reflections where maps don't author them. | existing |

### Not implemented, and why

- **TAAU (temporal upscaling).** TAA is implemented (above), but it resolves at
  render resolution. Upscaling uses `vid_scalefactor` with linear filtering plus
  CAS sharpening (`r_sharpen`), which is close to FSR 1. Proper temporal
  upscaling needs per-object motion vectors, which GZDoom doesn't produce; the
  right path is MetalFX in a native Metal backend (see below).
- **GPU particles.** GZDoom's particles are simulated by game logic and
  synchronised over the network. Moving them to compute would change behaviour
  and needs a compute path the MoltenVK backend doesn't have. Deferred.
- **Hardware ray tracing.** GZDoom's RT path (`vk_raytrace`) needs Vulkan ray
  queries, which MoltenVK does not expose. A native Metal backend could use
  Metal ray tracing on M3/A17 Pro and later for RT shadows on top of the
  baked GI.
- **Nanite-style geometry.** Not useful for Doom-scale geometry.

### Native Metal backend: evaluation

MoltenVK translates Vulkan to Metal at good quality. The remaining costs on
Apple GPUs:

- Render passes cannot use memoryless attachments or tile shaders, so the
  G-buffer goes through memory.
- Push-constant and descriptor-indexing limits apply (hence the 7-light
  volumetric cap).
- No ray queries.
- No MetalFX.

A native backend would be a new `src/common/rendering/metal` beside `vulkan`,
implementing `DFrameBuffer`, `FRenderState` and the `PPRenderState` hooks. The
shaders compile from the same GLSL through SPIRV-Cross, as MoltenVK already
does.

Payoff:

- About 20–30% less bandwidth from memoryless G-buffer targets and tile-shaded
  SSAO.
- MetalFX temporal upscaling.
- Metal ray tracing.

It's a multi-month project. Everything in this document works without it, so it
is a later step rather than a dependency.

### Light definitions

Apple builds default `autoloadlights` and `autoloadbrightmaps` to on (upstream
default: off, enabled from the Windows/Linux startup dialog, which the Apple
port doesn't have). Without them, the stock games have no dynamic lights for
the specular, volumetric and baked-GI passes to use. In E1M1 the GI bake goes
from 0 to 49 static lights.

## Quality presets

`r_preset` sets every feature at once, and each CVAR can be changed
afterwards. Choosing a preset in the menu or console turns `r_preset_auto`
off, which keeps the platform from changing it again.

| Preset | GI | Relief / specular | Parallax | SSAO | SSR | SSGI | Volumetric | Tonemap | Shadow filter / size | Sprite shadows | AA |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 Classic | – | – | – | off | off | – | off | off | 1 / 128 | default | – |
| 1 Low | ✓ | ✓ | – | off | off | – | off | ACES | 1 / 128 | default | – |
| 2 Medium | ✓ | ✓ | – | 1 | liquids | – | half | ACES | 1 / 256 | always | – |
| 3 High | ✓ | ✓ | ✓ | 2 | + floors | – | half | ACES | 2 / 512 | always | TAA |
| 4 Ultra | ✓ | ✓ | ✓ | 3 | + floors | ✓ | full | ACES | 3 / 1024 | always | TAA |

All presets above Classic enable the modern bloom and dynamic-light shadow
maps. The menu is under **Options → Display → Modern Rendering**.

### Automatic per-device choice

`r_preset_auto` (default on) lets the platform choose:

| Device | Preset | Notes |
|---|---|---|
| Mac, Apple Silicon, < 16 GB | High | chosen when the window is created |
| Mac, Apple Silicon, ≥ 16 GB | Ultra | Pro/Max class |
| iPhone/iPad A12 (Apple GPU family 5) | Low | 720p, 60 fps |
| A13 with ≥ 3 GB | Medium | 1080p |
| A14 / M1 iPad with ≥ 5 GB | High | 1440p, 120 Hz where available |
| A16+ / M-series iPad with ≥ 7 GB | Ultra | 1600p |
| Apple TV 4K (2nd gen, A12) | High | 1440p, on mains power |
| Apple TV 4K (3rd gen, A15) | Ultra | 2160p |

Thermal state and Low Power Mode lower the preset at runtime:

- "Fair" or Low Power Mode caps the preset at Medium (and SSAO and bloom off,
  as before).
- "Serious" or worse drops to Low and 35 fps. Baked GI and relief stay on there
  because they cost almost nothing per pixel.

This is implemented in `src/common/platform/ios/ios_hardware_policy.h`.

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
  0.77) also rendered correctly.
- TAA was checked on the Mac with static and turning captures: edges and
  texture shimmer resolve, and there is no ghosting or doubling while turning.
- Testing note: console commands passed with `+` on the command line run before
  the window exists, so the automatic preset overrides them. Use
  `+"wait 2; r_preset 3; …"` when scripting captures.
