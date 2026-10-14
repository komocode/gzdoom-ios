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
| Ray traced shadows | `vk_raytrace` (Metal renderer; restart) | On GPUs with Metal ray tracing, the scene shaders trace shadow rays for dynamic lights against an acceleration structure over the level mesh, instead of GZDoom's 2D shadow maps. Unlike the shadow maps, they respect height, 3D floors and overhangs. `gl_shadowmap_filter` sets the soft-shadow sample count. Off by default; hardware ray tracing (M3, A17 Pro and later) is recommended. | 1–28 rays per shadowed light and pixel |
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

### Automatic per-device choice

`r_preset_auto` (default on) lets the platform choose:

| Device | Preset | Notes |
|---|---|---|
| Mac, Apple Silicon, < 16 GB | High | chosen when the window is created |
| Mac, Apple Silicon, ≥ 16 GB | Ultra | Pro/Max class |
| iPhone/iPad A12 (Apple GPU family 5) | Low | 720p, 60 fps |
| A13 with ≥ 3 GB | Medium | 1080p |
| A14 / M1 iPad with ≥ 5 GB | High | 1440p 3D, 120 Hz where available |
| A16+ / M-series iPad with ≥ 7 GB | Ultra | 1600p 3D |
| Apple TV 4K (2nd gen, A12) | High | 1440p 3D, on mains power |
| Apple TV 4K (3rd gen, A15) | Ultra | 2160p |

The heights are the 3D render budget. High and Ultra use TAA, which replaces
MSAA, and upscale temporally to an output up to 1.25 times taller (never above
the display), so the HUD and post-processing are sharper at the same 3D cost.
Low and Medium render at the budget height with MSAA, and the output is scaled
with `vid_scalefactor`.

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
