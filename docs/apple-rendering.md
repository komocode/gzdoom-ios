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
| Baked GI (indirect bounce, colour bleeding) | `r_gi`, `r_gi_strength`, `r_gi_saturation`, `r_gi_quality`, `r_gi_rebake` | Irradiance probe grid. Each probe holds an ambient cube (6 colours), baked once per level on background threads by ray tracing the level mesh with a BVH. Hits return albedo × (sector light + static dynamic lights, with shadow rays, GLDEFS intensity, additive lights and spot cones) plus the emissive part of fullbright and brightmapped textures. Sky hits return the sky texture's average colour (night skies stay dark), blended towards the fog colour by `skyfog`. The probe grid only spans the playable map. The scene shader adds part of the bounce above the sector light level and tints by its chroma. Doom light levels stay the floor. | E1M1: 97×60×10 probes in about 0.12 s on M1 Max. One 3D texture fetch ×3 per pixel. |
| Soft dynamic-light shadows | `gl_light_shadowmap`, `gl_shadowmap_filter`, `gl_shadowmap_quality` | Contact-hardening filter on GZDoom's 1D shadow maps. A blocker search estimates the occluder distance, then a jittered, tent-weighted PCF widens the penumbra with it. Used in modern mode when the filter is above 0. | 6 + 4–10 taps per shadowed light |
| Texture relief ("auto normal") | `r_autonormal`, `r_autonormal_strength` | Normals from the luminance gradient of the diffuse texture, used only for dynamic lights. Classic light intensities are kept: relief modulates them. | ALU only |
| Specular | `r_specular`, `r_specular_strength` | Normalized Blinn-Phong from dynamic lights. Gloss (inverse roughness) is estimated from texel brightness and saturation; liquids and floors raise it. Metalness is estimated too: mid-to-bright, low-saturation texels (steel, brass, copper) tint their highlights with their own colour, as PBR metals do. Highlights are unclamped so they feed bloom. | ALU only |
| PBR material maps | existing GZDoom `normal`, `specular`, `metallic`, `roughness`, `ao` in GLDEFS | Authored maps still take priority. The automatic texture-map generation is done in the shader rather than as files: normal (relief), roughness (gloss), metallic and height (parallax) are all derived from the diffuse texture. | — |
| Parallax occlusion mapping | `r_parallax` | 10-step steep parallax from the luminance height. Only solid walls and opaque flats opt in (`uMaterialParams.z`), so sprites and masked mid textures keep their exact shapes. Reflective floors keep it; liquids and puddles are flat. Fades out between 256 and 640 units. | ≤ 10 texture taps, near surfaces only |
| Screen-space reflections | `r_ssr` (1 liquids, wet and GLDEFS-reflective surfaces; 2 + glossy floors), `r_ssr_strength`, `r_ssr_stretch` | Runs after the translucent pass and GPU particles of the main view, so lamps, torches, monsters, glows, particles and portal contents are reflected. Opaque-style sprites write depth during that pass so rays find them. Half-resolution ray march with binary refinement against a linear depth taken at that point. Roughness picks a level of a 5-level blur chain of the scene by the width of the reflection cone, sampled as a 5-tap streak along the surface normal's direction on screen (vertical on floors), longer at grazing angles. With TAA the rays are jittered over the roughness lobe every frame. Rays that miss fall back to the sky or the blurred screen where they left the view, and hits fade into that fallback at the screen edges, so there are no holes. Composited at full resolution with the G-buffer reflectivity. Doom's animated liquids are recognised by flat name, since Doom's TERRAIN lump doesn't mark them. | Half res, 24–40 steps + 5 small blur passes |
| Wet surfaces | MAPINFO `wetness`, `puddles`, `rainripples`; UDMF sector `wetness`, `puddles`; GLDEFS `reflectivity`, `roughness` | See [Wet surfaces and reflections](#wet-surfaces-and-reflections). Wetness darkens and saturates albedo, sharpens and strengthens dynamic-light highlights (stretched toward the viewer) and makes floors reflective. Puddles are near-mirrors from a noise mask in map space. | ALU + 4 taps on reflective pixels |
| Screen-space GI | `r_ssgi`, `r_ssgi_strength` | 4 cosine-distributed rays × 8 steps per half-resolution pixel gather nearby lit surfaces, tinted by the receiver's hue and added to the scene. Complements the baked probes with bounce from moving lights, muzzle flashes and explosions. Shares inputs with SSR and needs no temporal history. | Half res, 32 taps |
| Ambient occlusion | `gl_ssao` | GZDoom's existing HBAO-style SSAO. The presets drive its quality, and it shares linear depth with SSR and volumetrics. | existing |
| Volumetric light and height fog | `r_volumetric` (1 half, 2 full resolution), `r_volumetric_strength`, `r_volumetric_shadows`, `r_volumetric_samples`, `r_volumetric_distance`, `r_volumetric_fogscatter`, `r_heightfog` | Single scattering along each view ray up to the visible surface, for up to 32 dynamic lights. Spot lights show their cones, and shadowed lights cast light shafts through bars and around pillars. Exponential height fog can roll with noise and glows around lights. See [Volumetric light and fog](#volumetric-light-and-fog). No froxel grid. | Half res: closed form per unshadowed light, 12–16 samples per shadowed or spot light |
| HDR emission | `r_emissive`, `r_emissive_strength`, GLDEFS `emissive` | Brightmapped texels, the bright texels of fullbright sprites (lamps, projectiles, muzzle flashes) and additive dynamic lights may exceed white, so they, rather than every bright texel, drive bloom. A texel emits `max(lit, albedo × brightmap × √strength)` in the gamma-2 scene, so strength 1 is exactly the classic brightmap. `emissive <float>` in a GLDEFS `brightmap` or `material` block scales one texture (0 disables its HDR emission). | ALU only |
| HDR resolve and tone mapping | `gl_tonemap 6` (ACES filmic), `r_tonemap_white` | One pass linearises the gamma-2 scene, applies exposure, adds the bloom in linear light, grades and tone maps. ACES is normalised so `r_tonemap_white` (linear, default 4 = the default emissive peak) reaches display white; `r_tonemap_white 1` maps 1.0 to white. With no curve selected the pass still runs when bloom, exposure or grading need it. MAPINFO `lightblendmode = Unclamped, ACES` also works. | existing pass |
| Exposure | `r_exposure` (EV), `r_autoexposure`, `r_autoexposure_min/max/speed/key`, MAPINFO `exposurebias`, `PPShader.SetExposureBias` | A manual EV bias, plus optional eye adaptation independent of bloom: the log-average luminance is reduced to one texel and the exposure moves toward `key / average`, clamped to the EV range, at a frame-rate-independent speed. Bloom thresholds the exposed image. High and Ultra enable adaptation (−1.5 to +0.75 EV). | half-res extract + 1×1 chain |
| Colour grading | `r_grading_lut`, `r_grade_contrast`, `r_grade_saturation`, `r_grade_shadowtint`, `r_grade_hightint`, MAPINFO `gradinglut`, `PPShader.SetGradingLUT` | Contrast around middle grey and saturation in linear light, split toning and a 32³ LUT (a 1024×32 strip: red across a slice, green down, blue picks the slice) in display space, all in the tone mapping pass. LUT changes crossfade. Ships `LUTNEUTR` (identity, a template for artists) and `LUTNIGHT` (cool shadows, warm highlights, saturated reds), generated by `scripts/grading_luts.py`. | 2–4 taps |
| Contact shadows | `r_actorspriteshadow 2`, `r_actorspriteshadowfadeheight` | GZDoom's projected sprite shadows, always on from medium up and faded with height, plus AO. | existing |
| Temporal stability | `r_taa_reactive`, `r_sprite_depth` | The scene pass writes a reactive mask into the alpha of the normal G-buffer (otherwise unused): 1 for blended draws (translucent sprites, walls and flats, particles), moving sprites and models, and HUD models; ⅔ for animated or warped liquids. The TAA resolve clips that history tighter and keeps less of it; MetalFX gets it as `reactiveMaskTexture` (macOS 14.4 / iOS 17.4). Because reflections are traced after the translucent pass, blended draws must keep the normals behind them: alpha-blended draws mark only the pixels they cover (alpha ≥ 0.5), additive ones raise the mask without touching the normals (rain by its density), and subtractive and fuzz styles don't mark. Opaque sprites (normal style, binary alpha) also write depth, so TAA and MetalFX reproject them like geometry; translucent sprites are drawn as before. | small |
| Temporal anti-aliasing | `r_taa` | The 3D projection is jittered by a subpixel Halton(2,3) offset each frame. The resolve reprojects last frame's HDR result through the depth buffer and camera motion, clips it to the current 3x3 neighbourhood in YCoCg (rejecting stale history from moving sprites and disocclusions, which have no motion vectors) and blends with luminance weights. Runs before bloom and tone mapping. History resets on teleports, menus and resolution changes. Replaces FXAA from High up. | 1 full-res pass + copy |
| Temporal upscaling | `r_upscale` (0.5–1, 1 = off; needs `r_taa`), `r_upscale_metalfx` | The 3D scene renders into a smaller part of the scene viewport and is reconstructed to the full viewport. On the Metal renderer this is MetalFX's temporal upscaler, fed with the HDR colour, depth and camera motion vectors computed from depth and the previous camera. Elsewhere (or with `r_upscale_metalfx 0`) the TAA resolve does it: each output pixel is a Gaussian-weighted sum of the nearby jittered input samples, and the history counts for more where no sample landed near the pixel. More jitter phases are used while upscaling (16, or 32 below 0.7). Exposure, bloom, tone mapping, the weapon sprite and the HUD all run at output resolution, unlike `vid_scalefactor`, which scales the HUD too. | Scene cost × scale²; resolve replaces TAA |
| Ray traced shadows | `vk_raytrace` (Metal renderer; restart) | On GPUs with Metal ray tracing, the scene shaders trace shadow rays for dynamic lights against an acceleration structure over the level mesh, instead of GZDoom's 2D shadow maps. Unlike the shadow maps, they respect height, 3D floors, overhangs, bars and static models (see [Level mesh](#level-mesh)), and every light that allows shadows gets them, not only lights that reach past a wall. The 1D shadow maps are not rendered while tracing. `gl_shadowmap_filter` sets the soft-shadow sample count. Auto mode enables this only on Apple9+ GPUs with render-stage ray tracing (A17 Pro / M3 and later). Manual mode remains available. | 1–27 rays per shadowed light and pixel |
| Modern bloom | `gl_bloom`, `gl_bloom_amount`, `r_bloom_modern`, `r_bloom_threshold`, `r_bloom_knee` | 13-tap downsample chain (6 levels) with a soft-knee threshold on linear, exposed HDR values, then a tent-filtered additive upsample. The tone mapper adds the result in linear light (it used to be added to the gamma-encoded scene, which crushed faint halos). Fewer passes than the classic Gaussian bloom and no flicker on small highlights. | ~12 small passes |
| Weapon bloom | `r_weapon_prebloom` | The 2D weapon sprites are drawn after TAA (so they stay sharp and unjittered) but before exposure and bloom, so muzzle flashes and emissive weapon parts bloom and are graded like the scene. | none |
| Sharpen, vignette, grain | `r_sharpen`, `r_vignette`, `r_grain` | One pass: contrast-adaptive sharpening (CAS), a vignette, and triangular grain that also removes banding. | one full-screen pass |
| GPU particles | `r_gpuparticles` (1 normal, 2 dense) | Visual-only and stateless. Hitscan shots draw a tracer, eject a brass casing that lies on the floor for a few seconds and leave a wisp of gun smoke. Bullet impacts throw sparks, chips and a lingering dust cloud in the colour of the surface that was hit, sprayed along its normal and faded softly where they meet that surface and the floor. Blood throws droplets, explosions throw a fireball, embers, debris and rising smoke, and glowing projectiles burst in their light's colour. Each particle is written once into a ring of persistent vertex memory; the vertex shader evaluates the motion in closed form (gravity, drag, resting on the floor, sparks stretched along their velocity), so the CPU simulates nothing per frame. Falling particles rest on the floor or 3D floor below the point where their trajectory lands (sampled in 6 steps at spawn). Non-emissive particles are lit per vertex by the spawn sector's light, the baked GI, the 8 strongest dynamic lights near the camera (spot cones included) and lightning, and all particles take the view sector's fog. ZScript can spawn them too (below). Game particles, RNG, savegames and netplay are untouched. | Draws only the live part of a 16384-particle ring; 2 draws |
| Weather | `r_weather_quality` (1 normal, 2 + distant rain); MAPINFO and ZScript (below) | Rain or snow from a fixed set of drops in three nested layers around the camera plus splash slots, all computed in the vertex shader from the vertex index and the time: one draw and no CPU work per drop. Each layer is a world-space lattice that wraps around the camera. Drops fall only where a top-down sky exposure map (built once per level from sectors and 3D floors, updated for moving sectors) shows open sky, and end on the highest floor or 3D floor below it, where ripple rings and small splashes appear. Rain is a thin camera-facing streak along its velocity (wind included), lit additively by the sector light, the GI, nearby dynamic lights and lightning. Far drops keep a minimum on-screen width and fade instead, so distant layers read as sheets. | 98k vertices (123k at 2), thin streaks; 1 draw |
| Soft particles | `r_softparticles` | GPU particles, weather and translucent sprites flagged `+SOFTPARTICLE` (smoke, fog and steam cards) fade out where they approach opaque geometry, using the half-resolution linear depth that the AO pass computes. Needs the G-buffer pass (SSAO, SSR, SSGI, volumetrics or the TAA reactive mask); with soft particles on, the AO pass computes the depth even when SSAO itself is off. | 1 texel fetch per fragment of those draws |
| Lightning | existing lightning, `LevelLocals.WeatherFlash` | Each flash of the lightning thinker (or a scripted, visual-only flash) adds a brief sky light to every surface, particle and drop under open sky, after the light clamps so that it reaches the bloom. | 1 texel fetch per pixel while flashing |
| Sun light and shadows | MAPINFO `sun`, `sunbounce`, `sunspecular`; `r_sun`, `r_sun_shadowsize`, `r_sun_weapon`, `r_sun_rebake`; `LevelLocals.SetSun`, `SetSunColor` | A directional light with shadows from a depth map that worker threads bake from the level mesh after the map loads. The scene shaders add diffuse and GGX sunlight after the light clamps, shadowed through a percentage-closer filter whose radius is the map's softness, and the GI bake bounces it. Only maps with a `sun` key have one. See [Sun](#sun). | None without a sun. With one: a 2–17 ms bake per direction, 5–9 shadow texel fetches per sunlit pixel (up to 64 on shadow edges) and a GGX term; 4–6 ms per frame at 3344×1882 on M1 Max |
| Planar reflections | `gl_plane_reflection`, `gl_mirrors` | Already in GZDoom for mirrors and reflective planes; left as is. SSR adds reflections where maps don't author them. | existing |

### Volumetric light and fog

The pass integrates single scattering along each view ray up to the visible
surface (`wadsrc/static/shaders/pp/volumetric.fp`). Lights are chosen on the CPU
each frame (`src/rendering/hwrenderer/hw_volumetric.cpp`):

- **Visibility.** A light only counts if it reaches an area that the BSP walk
  rendered this frame (its touch list stops at one-sided walls), so lights in
  closed neighbouring rooms no longer glow through walls. Lights fade in and
  out over a few frames rather than popping, and fade with distance over the
  last quarter of `r_volumetric_distance` (default 4096) instead of being
  culled at a fixed 2048. Lights attached to the camera (weapon flashes)
  are skipped: the camera sits inside them, so they would fill the whole view.
- **Importance.** Lights are ranked by brightness × radius / distance. The
  first 32 are used. The most important up to `r_volumetric_shadows` (2 on
  Medium, 4 on High, 6 on Ultra) are shadowed. A light keeps its shadows
  until another one is clearly more important.
- **Shadowed and spot lights** are ray marched with `r_volumetric_samples`
  equiangular samples (spread evenly over the angle the ray segment subtends
  at the light, so they gather near it). Each frame uses a new per-pixel
  offset, which TAA integrates, followed by a 3×3 depth-aware blur. Spot
  lights march only the part of the ray inside the cone (an analytic ray-cone
  clip), with the soft edge between the inner and outer angles. Visibility
  comes from shadow rays against the level's acceleration structure when
  Metal ray tracing is active, otherwise from GZDoom's 1D shadow maps
  (2D occlusion by one-sided lines, which includes void-pillar bars). Other
  point lights use the closed-form inverse-square integral, plus a soft glow
  that fills each light's sphere so shafts stay visible away from the light.
- **Media.** Thin air everywhere; the camera sector's coloured fog (the same
  density the scene shaders use for it); and an exponential height fog that
  is constant below its base height and thins above it. Fog makes lights
  scatter more (`r_volumetric_fogscatter`), so a red lamp in ground fog lights
  up a red mist, and it also absorbs: distant halos fade with the fog. The
  height fog's own colour is blended over the scene by its optical depth
  (closed form, or 8 jittered samples when the noise is on). Sector fog is
  already applied by the scene shaders, so the pass only adds its glow and
  absorption.

The height fog is a level property. MAPINFO keys:

```
heightfog = 3.0, 0, 40          // density per 1000 map units, base height, falloff height
heightfogcolor = "28 2c 36"
heightfognoise = 0.6            // 0-1, rolling mist
heightfogwind = 24, 8           // noise scroll in map units per second
```

ZScript can read these as `Level.heightfog*` and change them with
`Level.SetHeightFog(density, baseheight, falloff, color, noise)`; they are
saved with the game. The `r_heightfog_density` / `_base` / `_falloff` /
`_color` / `_noise` CVARs override the map's fog while the density is above 0,
for trying values out. Height fog needs `r_volumetric`.

`r_volumetric 2` marches every pixel of the 3D viewport from the scene depth
buffer; `1` marches a quarter of them. Both buffers follow the 3D viewport, so
they shrink with `r_upscale`. `stat volumetric` shows the chosen lights and fog.
The selected light list is available to other effects through
`R_GetVolumetricLights()`.

On the Metal renderer, the light list is passed with `setFragmentBytes` and the
acceleration structure is bound to the postprocess pass. On Vulkan it is a
per-frame storage buffer, so the push constant block stays under 256 bytes.
The OpenGL backends skip the pass.

Test maps with a barred gate, a pillar in the light shafts, a red light in a
closed neighbouring room, and a sky courtyard with ground fog, a red lamp and a
spot light on a tower showed shadowed shafts, a visible cone and red mist, with
both ray traced and shadow-map visibility, at both resolutions, and on the
Vulkan renderer. The light in the closed room was not selected.

Limitations: the 1D shadow maps have no height, so without ray tracing a light
above a wall still shines over it; ray traced volumetric shadows are Metal
only; translucent surfaces and particles are drawn after the pass and are not
covered by the height fog; there are no per-sector height fog settings yet, and
no screen-space sky shafts.

### Not implemented, and why

- **Textured GPU particles.** All particles share one draw per blend mode,
  so they use built-in procedural shapes (glow, smoke, chunk, droplet, ring,
  mote) instead of textures.
- **Exact ordering with translucent surfaces.** Weather is drawn before the
  translucent pass, so water and glass behind it composite correctly, but
  drops in front of a translucent surface are dimmed by it. GPU particles are
  drawn after it: correct in front of translucent surfaces, drawn over those
  behind them. Rain never falls below water: it stops at 3D floor water tops.
- **Per-object motion vectors.** GZDoom doesn't produce them, so TAA and both
  upscalers reproject with depth and camera motion only. Moving sprites,
  particles and liquids are marked in the reactive mask instead, which removes
  the ghosting at the cost of less temporal anti-aliasing on them. True
  velocities would need the previous position of every actor, model bone,
  sector mover and texture scroll in a separate target.
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
  every postprocess pass run unchanged.
- **Frames in flight.** `r_framesinflight` (default 2, 1–3) frames are
  committed at Present without waiting, so the CPU records the next frame
  while the GPU renders the last one: frame time is about max(CPU, GPU)
  instead of CPU + GPU. Before a frame starts, the device waits only for the
  frame that last used the same buffers. CPU-written data is never overwritten
  while the GPU may read it:
  - Stream uniforms, matrices, viewpoints, lights and bones have one buffer per
    frame in flight, switched at the frame start.
  - The flat vertex buffer is N-buffered (`FFlatVertexBuffer`'s pipeline
    buffers, as GLES does). Its copy count is fixed at startup (at least 2),
    so raising the setting to 3 takes a restart. Rewriting its static part
    (level load, resize) waits for the GPU first.
  - `SetSubData` (the shadow map's dynamic AABB nodes) and `Lock` write to a
    copy while the GPU is busy. The software renderer's output buffers wait
    for the frame that last showed them.
  - GPU particles never reuse a ring slot that one of the last 8 draws
    included; when the ring is full, the new particle is dropped instead.
  - Old MetalFX scalers and ray tracing acceleration structures are released
    once the frames that used them have finished. Other resources are kept
    alive by the command buffers that use them.

  Screenshots, savegame pictures, resolution and MSAA changes, mid-frame
  uniform-buffer overflow, shutdown and changes to `r_framesinflight` drain
  the GPU first. `stat gpu` and the adaptive quality controller use the GPU
  time of the newest finished frame, read in the completion handlers.
  `r_framesinflight 1` restores the old behaviour of waiting at the end of
  every frame. The Vulkan backend keeps its own synchronization.
- **Unified memory.** Buffers and uploaded textures use shared storage: no
  staging copies. Mipmaps are generated on the GPU in a separate upload command
  buffer that runs before the frame.
- **No first-use stalls.** Creating a pipeline compiles its shaders for the
  vertex layout, attachments and blend state. Until the system shader cache
  holds that combination, this took 0.1–0.45 s on the M4. The common scene
  pipelines (opaque, masked, translucent and additive walls, flats, sprites,
  liquids, sky, stencil and particles, with and without the G-buffer) are
  compiled during startup for the active shaders. The other shader set (traced
  or shadow-map) and any new vertex layout or MSAA count follow on Metal's
  compiler threads, two at a time, so they never delay a pipeline a frame
  needs. Apple builds also default `gl_precache` to on, so a level's textures,
  sprites and models are uploaded at load time instead of on first sight.
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
  exposure adaptation timing. With frames in flight, API validation stays
  clean at 1, 2 and 3 frames and while switching between them, including the
  software renderer and ray tracing; shader validation is clean at 2. Classic frames match
  the synchronous renderer except for the weapon bob phase.

HDR output: with `vk_hdr 1` the Metal layer becomes an RGBA16Float extended
linear sRGB EDR surface (`wantsExtendedDynamicRangeContent`). The tone mapper
keeps the SDR curve up to white and rolls the energy above it into the
display's current headroom (`NSScreen.maximumExtendedDynamicRangeColorComponentValue`,
`UIScreen.currentEDRHeadroom`, up to 16×); the HUD and menus stay at SDR
white, and the present pass decodes to linear without dithering. Off by
default; the SDR path is unchanged and screenshots stay SDR. Not available on
tvOS. Only checked on an SDR display (no headroom), where it renders the SDR image.

Not done yet: memoryless G-buffer attachments,
which need the postprocess passes that read the G-buffer to move into tile
shaders. GPU frame time is shown with `stat gpu`.

## Level mesh

The baked GI, the ray traced shadows and the sun's shadow map share one mesh of
the level (`src/rendering/hwrenderer/doom_levelmesh.cpp`).

- **Built from the current state.** It is built on first use, after a savegame
  or hub snapshot has been restored, so restored doors, lifts and polyobjects
  are where the game left them. `r_gi_rebake` rebuilds it too.
- **Moving geometry.** While ray traced shadows are active, a hash of the sector
  planes, 3D floor states, polyobject positions and static model placements is
  checked every 100 ms; when it changes the mesh and the acceleration structure
  are rebuilt (E1M1: under 1 ms on the CPU), so open doors stop casting shadows.
  The baked GI stays as it was baked.
- **3D floors.** Only existing, solid, opaque ones occlude, using their own top
  and bottom planes (so Vavoom and inverted types are right). Water, glass
  (alpha below 255), additive and fog 3D floors let light through. Their sides
  face the sector they are seen from and use the master line's texture.
- **Sky.** Sky ceilings, sky floors (abyss or sea below a cliff), skybox planes
  and walls between them return sky light in the bake and never block shadow
  rays.
- **Masked mid textures** (bars, fences, grates, 3DMIDTEX bridges) are added as
  the rectangles of their opaque texels (greedy-merged, at most 256 per texture),
  so both the bake and the opaque ray tracing geometry are alpha-tested exactly
  with no texture lookups in the shaders. Translucent mid textures are skipped.
- **Static models.** Actors with `+STATICMESH`, and with `r_staticmodels 2`
  (default) also non-moving decorations (not monsters, players, pickups,
  projectiles, bobbing or rotating models), add their model (MD2/MD3/OBJ/IQM/UE1
  and voxels) in its current frame with the actor's full model transform.
  `-NOSTATICMESH` opts out, `r_staticmodels 1` takes only flagged actors and 0
  none. The triangles are merged into the single level BLAS rather than
  instanced: decorations are few, the mesh is only rebuilt when something
  changes, and the shaders and the Vulkan backend need no changes. Each model
  keeps a CPU copy of its vertices for this. Models that carry lights don't
  shadow them. Interpolation is ignored and skeletal IQM models contribute
  their bind pose. Budgets: 100k triangles per actor, 1M per level.
- **Grid bounds.** The probe grid covers sectors connected (through two-sided
  lines and linked portals) to a player start, monster, item or teleport
  destination, and not to a skybox viewpoint, so skyboxes, vistas and control
  sectors don't coarsen it.
- Acceleration structures are cached on a mesh generation counter rather than
  the mesh address, which the allocator can reuse for the next level.

## Wet surfaces and reflections

Reflections are authored per texture, per sector and per map:

- **GLDEFS** (`material texture|flat|sprite <name> { ... }`): `reflectivity
  <0-1>` makes any surface reflective, walls and model skins included;
  `roughness <0-1>` sets how blurred its reflections are. `roughness <map>`
  still names a roughness texture; PBR materials with one use it per pixel.
- **UDMF sector**: `wetness = <0-1>;` makes the floor wet (walls get half of it
  for darkening and gloss), `puddles = <0-1>;` covers that much of the floor
  with puddles.
- **MAPINFO** (map block): `wetness = <0-1>` and `puddles = <0-1>` are the
  defaults for every sector of the map; `rainripples = <0-1>` animates rain
  rings in wet floors under a sky ceiling, perturbing their reflections.
  Weather code can drive it at runtime through `level.info->rainripples`
  (`FLevelLocals::info`), which `HWFlat::SetReflectivity` reads every frame.

Without authoring, liquids reflect at 0.55 (mirror-like) and, with `r_ssr 2`,
other floors at 0.14 (roughness 0.5). Wet floors reflect at 0.35 × wetness
with roughness down to 0.35; puddles at 0.85 with roughness 0.04 and no
parallax. The reflection normal follows the normal map or the automatic
relief at 35 % strength (none inside puddles). Liquids on 3D floors are
detected from the plane's own texture, not the floor of the sector they sit in.
MAPINFO `wettexels` breaks wet floors and puddles into a mosaic of mirror and
matte texels (see [Map atmosphere](#map-atmosphere)).

## Weather, lightning and scripted particles

MAPINFO level keys (all optional):

```
weather = "rain"            // "rain", "snow" or "none"
weatherdensity = 1.2        // 0-2
weatherwind = 120, 30       // map units per second, direction in degrees
weatherlength = 1           // streak length (rain) or flake size (snow) multiplier
weatherspeed = 1            // fall speed multiplier
weathercolor = "b0 c0 d8"   // tint
```

ZScript, on `LevelLocals` (savegames keep the weather):
`SetWeather(type, density, windSpeed, windAngle, tint, dropLength, fallSpeed)`
with `WEATHER_None`, `WEATHER_Rain` or `WEATHER_Snow`, `GetWeather()`, and
`WeatherFlash(strength, duration)` for a visual-only lightning flash (sector
lights unchanged). Flashes of the existing lightning thinker light the
weather and open-sky surfaces automatically. Testing overrides (not saved):
`r_weather` (0 map, 1 rain, 2 snow, -1 none), `r_weather_density`,
`r_weather_wind` and `r_weather_winddir`.

GPU particles from game code: fill an `FXParticleParams` (`count`, `color1` /
`color2`, `shape`, `flags`, `pos` and `spread`, the velocity cone `dir` /
`coneAngle` / `speedMin` / `speedMax`, `gravity`, `drag`, `lifeMin` /
`lifeMax`, `sizeStart` / `sizeEnd`, `alpha`, `brightness`, `stretch`, `delay`)
and call `Level.SpawnFXParticles(p)`. Shapes are `FXS_Glow`, `FXS_Smoke`,
`FXS_Chunk`, `FXS_Droplet`, `FXS_Ring` and `FXS_Mote`. Flags are
`FXF_Additive` (additive and emissive), `FXF_Unlit` (alpha blended but
emissive), `FXF_RestOnFloor` and `FXF_NoDistanceScale`. New emissions are
thinned with camera distance (full count up to 768 units, down to 15 %) and
culled beyond 4096; `FXF_NoDistanceScale` keeps the full count and culls
only beyond 8192, for effects that must read from afar, such as surf at the
foot of distant cliffs. The per-tic, smoke and ring limits still apply.
`FXEmitter` is an invisible actor that emits
them continuously (`Rate` per second) and/or in bursts, optionally following
its `master`; subclasses override `SetupParticles`. Examples: `FXTorchEmbers`,
`FXDustMotes`, `FXSteamVent`, `FXSparkShower` and `FXSeaSpray`. Emitters use
only the particles' own random generator, so they are netplay- and demo-safe;
the particles themselves are not saved. `+SOFTPARTICLE` on a translucent
sprite actor makes it fade into the scene like the GPU particles.

## Map atmosphere

Opt-in MAPINFO map keys for a map's look. Maps without them render and bake
exactly as before.

```
gisky = "18 22 3a", 0.6              // GI sky radiance: colour, intensity
gimoon = 35, 28, "9cb4ff", 2.5, 24   // GI moon: yaw, pitch, colour, intensity, exponent
rainscatter = 1.5                    // volumetric scattering added by rain
volumetricstrength = 0.4             // multiplies r_volumetric_strength on this map (0-8)
ssrstretch = 4                       // replaces r_ssr_stretch on this map (> 0)
wettexels = 1                        // wet floors as a texel mosaic (0-1)
forceheightfog                       // height fog even with r_heightfog off
forcebloom                           // bloom even with gl_bloom off
```

- **`gisky`** replaces the sky radiance of the GI bake (normally the sky
  texture's average colour) with colour / 255 × intensity (default 1). Sky
  fog (`skyfog`) still blends it toward the fog colour.
- **`gimoon`** adds a directional lobe to GI rays that hit the sky:
  colour / 255 × intensity × max(0, ray · moon)^exponent, with the moon at
  (cos pitch cos yaw, cos pitch sin yaw, sin pitch) in map space: yaw 0 is
  east (+x), 90 north (+y), pitch is the elevation. The exponent (default 32)
  is optional. Diffuse probes and reflection probes see the same sky. Lobes
  sharper than a bake can sample (fewer than about four rays or reflection
  texels inside the lobe) are widened with the same energy (linear energy for
  reflections), so probes don't flicker between catching and missing the moon.
  The bake summary (`developer`) reports both and the widened exponents.
- **`rainscatter`** adds rainscatter × weather density to the scattering of
  the volumetric medium while the map's weather is rain, so flood and spot
  lights read as beams in the rain outdoors. It is scaled by how much of the
  area within about 256 units of the camera is in the rain (open sky in the
  sky exposure map, not under a 3D floor roof above the camera; under cover,
  only rain seen through doors and windows counts) and eased in and out, and
  starts at the right value on a new map or a loaded save. Thin air counts as
  1, so `rainscatter = 1` at density 1 doubles the scattering; 0.5–2 suits
  most maps (4 washes out a small courtyard). It does not absorb, unlike fog.
  `stat volumetric` shows the current value.
- **`volumetricstrength`** (0–8, default 1) multiplies the player's
  `r_volumetric_strength` on this map, for maps whose many lights would
  saturate the scattering (a night storm with hundreds of lamps) or that need
  more of it. It scales only the light scattered by the volumetric pass,
  including the glow of lights in fog and rain; the height fog's colour and
  absorption are unchanged. 0 drops every light from the pass. It is a balance
  value, not an override of a preference, so `r_map_atmosphere 0` keeps it.
  `stat volumetric` shows it as "map strength". Single lights can be scaled
  with their `arg4` (see [Light definitions](#light-definitions)).
- **`wettexels`** (0-1) draws wet floors as a texel mosaic, like wet pixel
  art: each texel of the floor texture either holds a water film that
  mirrors the scene (reflectivity at least 0.8, roughness 0.1) or stays matte
  grit (a fifth of the wet reflectivity, roughness at least 0.6). Glossy
  texels (low authored roughness, else bright ones) take the film first and a
  per-texel hash spreads the rest: 60 % x wetness of the texels hold film, and
  60 % inside puddles, whose matte texels break them up. Film texels tilt a
  little toward or away from the viewer, so lamps and the sun reflect as
  broken vertical streaks, and the film sets the roughness that dynamic lights
  and the sun use. The value fades the mosaic in over the plain wet look;
  up-facing floors wetter than about 0.1 take part. It costs two hashes per
  wet pixel. A material look, not an override of a preference, so
  `r_map_atmosphere 0` keeps it. It rides in `uWeatherParams.w`.
- **`forceheightfog`**, **`forcebloom`** and **`ssrstretch`** override the
  player's `r_heightfog`, `gl_bloom` and `r_ssr_stretch` on this map without
  changing the CVARs (nothing is written to the ini). Height fog still needs
  `r_volumetric`, and bloom follows `r_bloom_modern`; with `r_preset_auto`,
  `forcebloom` does not bring back bloom that the Apple quality policy turned
  off (lowest GPU tiers, low power mode, heat). The flags take an optional
  `= true` / `= false` (or on/off, yes/no, 1/0). `r_map_atmosphere 0`
  (archived, default 1, "Map atmosphere overrides" in the Modern Rendering
  menu) ignores these three.

### Sun

```
sun = 200, 26, "ffd9a0", 3.5, 1.5    // yaw, elevation, colour, intensity[, softness]
sunbounce = 1                        // GI bounce of the sun (0-16, default 1)
sunspecular = 1                      // sun highlights (0-16, default 1)
```

- **`sun`** lights the map with a directional sun that casts shadows. Yaw and
  elevation point toward the sun in map space, as for `gimoon`: yaw 0 is east
  (+x), 90 north (+y), and the elevation (1–90) is in degrees above the
  horizon. The colour is sRGB. The intensity (0–64) scales it in linear light:
  at 1, a white sun adds as much light to a surface facing it as light level
  255 gives it. The softness (0–8, default 1.5) is the radius of the shadow
  filter in shadow map texels; 0 gives hard edges. The example is a low,
  golden afternoon sun from the west-south-west.
- **`sunbounce`** scales the sunlight that the GI bake bounces; 0 keeps the
  sun out of the probes.
- **`sunspecular`** scales the sun's highlights, including its glints on wet
  surfaces.

A map without a `sun` key has no sun and pays nothing for it: nothing is
baked, and it draws with the same shaders as before (below).

**Shading.** The scene shaders add albedo × colour × N·L × shadow plus a GGX
highlight in linear light after the light clamps, like lightning, so the
sector light level doesn't scale the sun and sunlit surfaces can reach the
bloom. Fog covers it like any other light. N comes from the authored normal
map or the relief normal (`r_autonormal`). The highlight follows the material
roughness, or the authored specular and PBR maps, and sharpens on wet
surfaces and puddles (`wetness`, `puddles`), where it is stretched toward the
viewer like dynamic-light highlights. Authored sprite materials use their
normal maps. Other sprites take the sun and its shadow at their position,
with a mild wrap toward the camera because they are flat. The HUD weapon
takes `r_sun_weapon` of the sun (default 0.35) while the player stands in
sunlight, easing in and out of shadows. The sky and fullbright, additive,
fuzzy and shadow-style sprites take no sun. The GI bake adds the sun at the
ray hits that a shadow ray finds open to the sun, so sunlit ground bounces its
colour into the shade.

**Shadows.** One tic after the map loads, once static models are in place,
worker threads render the [level mesh](#level-mesh) into an orthographic
float depth map along the sunlight. Walls, ceilings, opaque 3D floors, masked
mid textures (through their opaque texels) and static models cast shadows.
Sector floors, sky surfaces, horizon and portal lines and sectors outside the
playable map (3D floor control sectors, skybox areas) don't. Texels are square
and at least one map unit; `r_sun_shadowsize` (512–8192, default 4096) is the
longer side. The log reports `SUN: baked <width>x<height> in <n> ms`: on an M1
Max, 2–3 ms for the 2568×1891 map of a small test level and 14–17 ms for the
4096×1899 map of MAP01B (198k triangles). There is no sun until the first bake
is done; a level with nothing to bake gets an unshadowed sun. Ceilings cast as
if they were 2–48 units thick (more for softer shadows and lower suns), so that
a low sun can't slip between a roof and the wall below it. As a result,
openings in a roof without thickness shrink by about that thickness divided by
tan(elevation). The map is static. Doors, lifts, polyobjects and moving models
keep the shadows they had at the bake until `r_sun_rebake`, and actors don't
cast (their projected sprite shadows remain). The shadow map takes width ×
height × 4 bytes on the GPU, plus a CPU copy that answers whether the eye is
in sunlight, at most 64 MiB each at 4096×4096.

**Shaders.** The sun shading is compiled only into sun variants of the scene
shaders (`SUN_LIGHT` in `main.fp`), which the backends use while a viewpoint
with a sun is bound. All other drawing, and every map without a sun, keeps the
plain shaders. Compiling the sun into them, even behind a branch that is never
taken, slowed every map down on Apple GPUs. When a sunlit level loads, the
variants of the common shaders (textured, material, warped and untextured,
with and without alpha test, in the pass the main view uses) and of the user
shaders on the level's walls and flats are compiled. On an M1 Max this takes
0.35–0.4 s with a warm system shader cache and 0.9 s on the first run after an
engine update (Metal builds them in parallel), and 0.15 s on Vulkan. Compiled
variants are kept for the session. Others compile when first drawn.

**Scripts.** `LevelLocals.SetSunColor(Color c, double intensity)` changes the
colour and intensity at once. `LevelLocals.SetSun(double yaw, double elevation)`
changes the direction and re-bakes the shadow map in the background, keeping
the old shadows and direction until the new map is ready. Either one turns the
sun on for a map without a `sun` key (softness 1.5; yaw 0 and elevation 45
until `SetSun` is called). `GetSunColor()` and `GetSunIntensity()` (0 without
a sun) read the settings back, and savegames keep them. The GI bounce keeps the
sun of its own bake until `r_gi_rebake`.

```
Level.SetSun(180, 12);                          // low in the west
Level.SetSunColor(Color(255, 150, 90), 4.0);    // evening orange
```

**CVARs.** `r_sun` (default on) draws the sun of maps that have one.
`r_sun_shadowsize` sets the shadow map size and re-bakes when changed.
`r_sun_weapon` (0–1, default 0.35) sets how much sun the HUD weapon takes.
`r_sun_rebake` re-bakes with the level mesh rebuilt, so doors and lifts cast
where they are now.

**Limitations.** The OpenGL backends have no sun. Moving geometry needs
`r_sun_rebake`, and actors don't cast. The volumetric pass doesn't scatter the
sun, so there are no sun shafts. On a small test level at 3344×1882 on an M1
Max, the sun adds 4–6 ms of GPU time per frame to about 16 ms without it
(measured on a busy machine). About half of that comes from the larger shader
variants rather than from the sun's own work.

## Light definitions

Apple builds default `autoloadlights` and `autoloadbrightmaps` to on (upstream
default: off, enabled from the Windows/Linux startup dialog, which the Apple
port doesn't have). Without them, the stock games have no dynamic lights for
the specular, volumetric and baked-GI passes to use. In E1M1 the GI bake goes
from 0 to 49 static lights.

Map-placed point and spot light things that don't animate (DoomEdNums 9800,
9810, 9820, 9830, 9840, 9850, 9860, 9870: `PointLight`, `SpotLight` and their
additive, subtractive and attenuated variants) don't use their fifth argument,
so `arg4` sets the light's volumetric scattering in percent:

| `arg4` | Volumetric scattering |
| --- | --- |
| 0 (default) | 100%, as before |
| N > 0 | N% (capped at 1000), e.g. 25 for a quarter, 300 for three times |
| N < 0 | none: the light is not a candidate for the pass at all |

It scales both the light's rank among the 32 volumetric lights and its
scattered colour; surface lighting, shadows and the GI bake are unchanged
(subtractive lights never scatter). Vavoom lights (1502, 1503, 9825) follow the
same rule. Hexen-format maps store thing arguments as bytes (0–255), so turning
a light's scattering off with a negative value, or going above 255%, needs a
UDMF map (or ACS at run time). Changes made at run time (ACS `SetThingSpecial`)
apply on the next tic, or when the light is activated. Pulse, flicker and random
flicker lights (9801, 9802, 9804 and the matching numbers in each family) keep
`arg4` as their secondary intensity; they, sector lights (9803 and its matches,
which don't read `arg4`) and lights attached through GLDEFS or `A_AttachLight`
always scatter at 100%.

## Quality presets

`r_preset` sets every feature at once, and each CVAR can be changed
afterwards. Choosing a preset in the menu or console turns `r_preset_auto`
off, which keeps the platform from changing it again.

| Preset | GI | Relief / specular | Parallax | SSAO | SSR | SSGI | Volumetric | Tonemap | Shadow filter / size | Sprite shadows | Particles | Weather | Soft particles | AA |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 Classic | – | – | – | off | off | – | off | off | 1 / 128 | default | – | – | – | – |
| 1 Low | ✓ | ✓ | – | off | off | – | off | ACES | 1 / 128 | default | normal | normal | – | – |
| 2 Medium | ✓ | ✓ | – | 1 | liquids, wet | – | half, 2 shadowed | ACES | 1 / 256 | always | normal | normal | ✓ | – |
| 3 High | ✓ | ✓ | ✓ | 2 | + floors | – | half, 4 shadowed | ACES | 2 / 512 | always | dense | + distant | ✓ | TAA |
| 4 Ultra | ✓ | ✓ | ✓ | 3 | + floors | ✓ | full, 6 shadowed | ACES | 3 / 1024 | always | dense | + distant | ✓ | TAA |

All presets above Classic enable the modern bloom, dynamic-light shadow maps,
HDR emission, weapon bloom, sprite depth and (with `r_pixelart`, on by default)
pixel-art filtering: `gl_texture_filter 6`, nearest magnification with smooth
mipmaps. With TAA the scene samples sharper mips (LOD bias log2 of the 3D scale
minus 0.25, plus `r_mipbias`), since the resolve accumulates subpixel detail.
High and Ultra add eye adaptation and the TAA reactive mask. Classic sets
`gl_texture_filter 4` and turns all of these off. Grading is art direction,
so the presets leave it alone. The menu is under **Options → Display → Modern Rendering**.

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
Medium and Low scale the whole output when needed; with `r_pixelart` that
stretch uses nearest filtering, snapped down to an exact 1/n when within 6%,
instead of a bilinear stretch that smears pixel art. MetalFX is queried before
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

- `HWViewpointUniforms` gained `mRenderFeatures`, GI parameters, the probe
  volume origin and size, `mEmissiveStrength` (√ of `r_emissive_strength`) and
  `mMipBias`. The per-draw `StreamData` uses three spare `vec4`s:
  `uMaterialParams` (reflectivity, gloss, parallax, particle time),
  `uMaterialParams2` (wetness, puddles, rain ripples, G-buffer mode) and
  `uSurfaceParams` (x = TAA reactive mask, y = fullbright emission, z = 1 keeps
  the sun off the draw: `FRenderState::SetNoSun`);
  `uDetailParms.w` carries the material's GLDEFS `emissive` factor. The
  per-draw values are reset by `SetMaterial`.
- The normal G-buffer's alpha is the TAA reactive mask (RGB10A2: 0, ⅓, ⅔, 1).
  With TAA and the mask on, the G-buffer pass runs even without SSAO or SSR.
- The irradiance volume is a `sampler2DArray` at fixed descriptor set
  binding 3, with layer = face × sizeZ + z, interpolated in z in the shader.
- The G-buffer's SceneFog alpha encodes reflections. A byte value b ≥ 128
  marks a scene surface; v = 255 − b holds 4 bits of reflectivity (v / 8) and
  3 bits of roughness (v mod 8), dithered. Cleared sky pixels never reflect.
  SceneNormal carries the reflection normal on reflective pixels.
- Because reflections run after the translucent pass, translucent draws use a
  G-buffer mode (`EGBufferMode`): pixels they cover with alpha ≥ 0.5 become
  non-reflective, everything else (and additive, fuzz and subtractive styles)
  leaves the G-buffer untouched, by writing alpha 1 or 0 under their own
  blend. Portal contents are drawn the same way, so only the main view
  reflects. GPU particles mark only their dense, non-additive parts. The TAA
  reactive mask shares the normal target, so blended draws write it only where
  they cover, and additive draws write a zero normal with the mask in alpha,
  which their blend adds to the alpha alone.
- SSAO, SSGI, volumetrics and reflections size their half-resolution buffers
  from the 3D viewport, so their cost drops with `r_upscale`.
- GPU particles share the scene shaders: `uMaterialParams.w > 0` switches
  `main.vp` and `main.fp` to the particle path (w is the current time; z is 0
  for the alpha-blended particle pass, 1 additive, 2 rain, 3 snow). The
  per-draw `StreamData` vectors `uParticleParams` and `uParticleParams2`
  (formerly padding) carry the weather parameters and the soft-fade distance;
  on ordinary draws `uParticleParams.x > 0` is the soft-fade distance of a
  `+SOFTPARTICLE` sprite. Particles and weather are drawn in the main view
  only, so mirrors and portals don't show them.
- The CPU writes each particle slot once and never touches it again while it
  can be drawn (the ring is append-only), so frames still in flight on the GPU
  read consistent data. A new level just forgets the old range.
- Sky exposure map: an RGBA16F 2D array texture (one layer) at fixed set 0,
  binding 4 (`WeatherMap`, Metal texture 15), read with `texelFetch` from the
  vertex and fragment stages. Per cell of 16 or more map units: r = height
  where rain stops, g = sky ceiling height, b = sector light, a = 1 under open
  sky. `uWeatherMap` (origin x, y, 1 / cell size, cells in x; w = 0 when
  unbound) and `uWeatherParams` (cells in y, lightning flash, time) are in
  `HWViewpointUniforms`. `WeatherCell(mapPos)` and `WeatherExposure(worldPos)`
  in `main.vp` and `main.fp` answer "is this point under open sky", for example
  for wet surfaces.
- Soft particles: the half-resolution linear depth from the AO pass is bound
  at set 0, binding 5 (`SceneLinearDepth`, Metal texture 16) after
  `AmbientOccludeScene`, for the rest of the main view; `uSceneDepthMap` maps
  `gl_FragCoord` to its texels. Until then, and without a G-buffer effect, a
  null texture is bound and the fade is off. Vulkan writes a new fixed
  descriptor set at that point, which the next render pass binds. Both new
  textures are read without a sampler (`GL_EXT_samplerless_texture_functions`)
  because Metal has no sampler slot left. The irradiance volume and the light
  buffer are now visible to the vertex stage for lit particles. The OpenGL
  backend declares the new textures but binds nothing, so weather and the soft
  fade stay off there.
- Checked on the Mac (Metal, and Vulkan through MoltenVK) with a UDMF test map:
  a sky courtyard crossed by a 3D floor bridge and a roofed room. Rain falls
  only under open sky and splashes on the ground; none falls under the bridge
  or indoors. A lightning flash lights the courtyard but not the floor under
  the bridge or the room. Soft smoke cards lose their hard
  line where they cut the floor; debris thrown off the bridge lands on the
  ground below it. DOOM2 MAP01 and MAP02 show rain only in their outdoor areas.
  Metal API validation is clean.
- Post-process uniforms are Vulkan push constants. The volumetric block is
  kept at 256 bytes (`static_assert`) so it works outside MoltenVK too; the
  light list goes in a storage buffer (`PPRenderState::SetStorageData`).
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
- HDR emission, bloom, exposure, grading, weapon bloom and the reactive mask
  were checked on E1M1 and DOOM2 MAP21 on an M4 Mac (Metal with API
  validation, and Vulkan). Classic matches the previous build apart from
  animation timing. At `r_upscale 0.6`, imps walking toward the camera leave a
  doubled (TAA) or smeared (MetalFX) trail with `r_taa_reactive 0` and none
  with it on. The neutral LUT matches no LUT apart from 8-bit rounding.
- Sun (`src/rendering/hwrenderer/hw_sun.cpp`): `HWViewpointUniforms` gained
  `mSunDir` (toward the sun; w = 1 while it shines), `mSunColor` (linear colour
  × intensity; w = `sunspecular`), the light-space rows `mSunShadowU`,
  `mSunShadowV` and `mSunShadowD` (in the shaders' x, height, y order; U and V
  in texels), `mSunParams`
  (map width and height, softness, map units per texel) and `mSunParams2` (HUD
  mode, where the direction is in view space and x replaces the shadow lookup).
  The shadow map is an R32F 2D texture at set 0, binding 6 (`SunShadowMap`,
  Metal texture 17), read with `texelFetch`; without a sun a 1×1 placeholder
  is bound. `HWViewpointBuffer` records which viewpoints have a sun, and
  binding one sets `FRenderState::SetSunShading`, which selects the sun
  variants: Metal swaps the program (`MtlShaderManager::GetSunVariant`), and
  Vulkan swaps the fragment shader (`VkShaderManager::GetSunFragment`, keyed by
  `VkPipelineKey::SunShading`). `DFrameBuffer::PrecacheSunShaders` compiles
  them during the level load. On Metal, `MtlShaderManager::PrecacheSunVariants`
  translates them on the main thread and has Metal build the libraries in
  parallel (`newLibraryWithSource:options:completionHandler:`), then their
  common pipelines join the background prewarm. The GI bake traces a shadow
  ray toward the sun from each ray hit, through sky surfaces (`TRI_SKY`).
- The sun was checked on the Mac (Metal, and Vulkan through MoltenVK, which
  match) on a UDMF test level with a courtyard, a roofed porch and room, a
  pillar, a fence, a truck model, sprites and wet floors and puddles, and on
  MAP01B. Shadows show no leaks under roofs and no acne on sunlit walls, and
  highlights sharpen on wet ground. Maps without a sun render as before.
- Testing note: console commands passed with `+` on the command line run before
  the window exists, so the automatic preset overrides them. Use
  `+"wait 2; r_preset 3; …"` when scripting captures.

## Authored sprite materials

`r_sprite_pbr 1` enables per-pixel lighting for actor sprites that have authored
normal, specular, metallic, roughness or occlusion maps. Sprites without maps
retain their previous rendering. There is no runtime material inference or
automatic normal generation for sprites. Fullbright, additive, fuzz, stencil,
custom-shader and particle effects keep their previous treatment. Modern presets
enable sprite lighting; Medium and above enable reflections. Existing saved
configurations can enable the new controls under Modern Rendering / Sprite
materials or with the console commands below.

An optional material PK3 uses these paths for short sprite names:

```
materials/normalmaps/auto/SPRTA0.png
materials/metallic/auto/SPRTA0.png
materials/roughness/auto/SPRTA0.png
materials/ao/auto/SPRTA0.png
```

Names must match each active frame and rotation. Full-path textures use the
source path without its extension after the channel directory. Existing GLDEFS
`material sprite` declarations also work. Author tangent normals with green +Y
pointing upward (neutral RGB 128,128,255), scalar roughness/metallic/AO in red,
and the original framing. The renderer aligns map borders and dimensions; check
pixel alignment of generated maps before packaging. Authored specular
maps retain their workflow. Partial maps use constant neutral defaults for
missing channels, never material guesses from colour.

- `r_sprite_pbr 0/1`: authored sprite lighting.
- `r_sprite_reflections 0/1`: local environment reflections.
- `r_sprite_light_strength 0..6`: direct-light intensity; 1 preserves the original response.
  This scales diffuse and specular light together, retaining falloff, shadows,
  material color and normal direction. Ambient and probe reflections keep their original intensity.
- `r_sprite_normal_strength 0..3`: normal strength; 1 uses the authored map.
- `r_sprite_debug 0..4`: lit, world normals, roughness, metallic, occlusion.
- `r_gi_rebake`: refresh static bounce and reflection captures.

Diffuse bounce follows the material normal. Direct lights use GGX highlights.
Reflections use up to 128 spatially blended static room captures with four
roughness levels, sampling room textures, sector lighting and static lights.
These are low-resolution environment probes. Moving actors and changing doors
are not captured live; rebaking refreshes the static snapshot. Translucent map
geometry and unusual wall UV transforms remain approximate. Sprite materials do
not create body geometry, change silhouettes, displace depth or cast volumetric
self shadows.

Asset workflow (requires Pillow):

```sh
python3 scripts/sprite_material_assets.py extract /path/to/game.pk3 --palette-wad /path/to/base.wad --out /path/to/extracted
# Author normalmaps/, metallic/, roughness/ and ao/ atlas images at the exact
# atlas size. Inspect every frame against the original and preserve placement.
python3 scripts/sprite_material_assets.py pack --manifest /path/to/extracted/manifest.json --authored /path/to/authored --output /path/to/game-materials.pk3
```

The tool extracts and packs pixels; it does not invent materials. It records
active frames and offsets, rejects incomplete channel sets by default, and
writes a coverage receipt into the PK3. Source artwork and the authored pack
belong with the supplied game, not the engine's generic asset archive.

```sh
python3 scripts/test_sprite_material_assets.py
python3 scripts/tests/test_sprite_reflections.py
python3 scripts/tests/validate_sprite_shaders.py
python3 scripts/tests/sprite_lighting_scene.py /tmp/sprite-lighting-test
```

The shader validator checks that an unmapped sprite is byte-identical with
sprite features enabled or disabled. Add `--preview-dir /tmp/sprite-preview`
to export synthetic spatial-material previews without opening a game window.
Those previews compare flat-normal PBR with normal-mapped PBR, not the classic
renderer.

The last command creates a synthetic analytic sphere and an unmapped control
for renderer checks. This is test data, not production material generation.
