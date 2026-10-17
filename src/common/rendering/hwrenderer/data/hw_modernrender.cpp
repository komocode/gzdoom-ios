/*
** hw_modernrender.cpp
** Feature toggles and quality presets for the modern lighting pipeline.
**
** Every feature is independently switchable. r_preset only writes the
** individual CVARs, so users can start from a preset and then adjust it.
** Preset 0 (classic) restores the original GZDoom presentation.
*/

#include "hw_modernrender.h"
#include "hw_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include <algorithm>
#include <cmath>

EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, gl_shadowmap_filter)
EXTERN_CVAR(Int, gl_shadowmap_quality)
EXTERN_CVAR(Bool, gl_light_shadowmap)
EXTERN_CVAR(Int, gl_fxaa)
EXTERN_CVAR(Int, r_actorspriteshadow)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)
EXTERN_CVAR(Int, gl_texture_filter)

static bool applyingPreset;

CVARD(Bool, r_preset_auto, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "let the platform choose r_preset from the device and thermal state")
CVARD(Bool, r_adaptive_quality, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjust Apple auto quality from measured GPU frame time")
CVARD(Bool, r_raytrace_auto, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "automatically use hardware ray traced shadows in Apple auto quality mode")
CVARD(Int, r_adaptive_targetfps, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Apple auto-quality FPS preference: 0 device estimate, or 35-120 capped by display and thermal limits")
CVARD(Bool, r_adaptive_debug, false, 0, "print Apple quality and timing diagnostics every five seconds")

CUSTOM_CVARD(Int, r_preset, -1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "rendering quality preset: 0 classic, 1 low, 2 medium, 3 high, 4 ultra, -1 not set")
{
	if (self < -1 || self > RP_ULTRA) self = -1;
	else if (self >= 0 && !applyingPreset)
	{
		r_preset_auto = false;
		R_ApplyRenderPreset(self);
	}
}

// Individual features. r_preset writes these; they can be changed afterwards.
#define FEATURE_CVAR(type, name, def, desc) CVARD(type, name, def, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, desc)

FEATURE_CVAR(Bool, r_gi, false, "baked irradiance probes: indirect light and colored light bleeding")
FEATURE_CVAR(Float, r_gi_strength, 1.f, "strength of baked indirect light")
FEATURE_CVAR(Float, r_gi_saturation, 0.6f, "how strongly indirect light tints surfaces")
FEATURE_CVAR(Int, r_gi_quality, 1, "irradiance bake quality: 0 fast, 1 normal, 2 high")
FEATURE_CVAR(Bool, r_autonormal, false, "derive surface relief from textures for dynamic lights")
FEATURE_CVAR(Float, r_autonormal_strength, 1.f, "strength of derived surface relief")
FEATURE_CVAR(Bool, r_specular, false, "specular highlights from dynamic lights on all surfaces")
FEATURE_CVAR(Float, r_specular_strength, 1.f, "strength of derived specular highlights")
FEATURE_CVAR(Bool, r_sprite_pbr, false, "per-pixel lighting for sprites with authored material maps; unmapped sprites keep classic lighting")
FEATURE_CVAR(Bool, r_sprite_reflections, false, "local baked environment reflections on authored sprite materials")
FEATURE_CVAR(Float, r_sprite_light_strength, 1.f, "direct-light intensity on authored sprite materials; 1 preserves the original response")
FEATURE_CVAR(Float, r_sprite_normal_strength, 1.f, "strength of authored sprite surface normals")
FEATURE_CVAR(Int, r_sprite_debug, 0, "sprite material view: 0 lit, 1 normals, 2 roughness, 3 metallic, 4 occlusion")
FEATURE_CVAR(Bool, r_parallax, false, "parallax occlusion mapping from texture height estimates")
FEATURE_CVAR(Int, r_ssr, 0, "screen-space reflections: 0 off, 1 liquids, wet and GLDEFS-reflective surfaces, 2 also glossy floors")
FEATURE_CVAR(Float, r_ssr_strength, 1.f, "strength of screen-space reflections")
FEATURE_CVAR(Float, r_ssr_stretch, 2.5f, "vertical stretch of rough and wet-floor reflections (0 = round)")
FEATURE_CVAR(Int, r_ssgi, 0, "screen-space indirect light from dynamic sources: 0 off, 1 on")
FEATURE_CVAR(Float, r_ssgi_strength, 1.f, "strength of screen-space indirect light")
FEATURE_CVAR(Int, r_volumetric, 0, "volumetric light and height fog: 0 off, 1 half resolution, 2 full resolution")
FEATURE_CVAR(Float, r_volumetric_strength, 1.f, "strength of volumetric light scattering")
FEATURE_CVAR(Int, r_volumetric_shadows, 4, "most volumetric lights per frame with shadowed light shafts (0 = none)")
FEATURE_CVAR(Int, r_volumetric_samples, 12, "ray march samples per shadowed or spot volumetric light")
FEATURE_CVAR(Float, r_volumetric_distance, 4096.f, "distance at which volumetric lights have faded out")
FEATURE_CVAR(Float, r_volumetric_fogscatter, 1.f, "how much brighter fog makes volumetric light")
FEATURE_CVAR(Bool, r_taa, false, "temporal anti-aliasing with subpixel jitter and reprojected history")
FEATURE_CVAR(Float, r_upscale, 1.f, "temporal upscaling with TAA: 3D render scale from 0.5 to 1 (1 = off); the HUD stays at full resolution")
FEATURE_CVAR(Bool, r_upscale_metalfx, true, "use MetalFX for temporal upscaling when the Metal renderer provides it")
FEATURE_CVAR(Bool, r_bloom_modern, true, "use the energy-conserving progressive bloom")
FEATURE_CVAR(Float, r_sharpen, 0.f, "contrast-adaptive sharpening amount (0-1)")
FEATURE_CVAR(Float, r_vignette, 0.f, "vignette strength (0-1)")
FEATURE_CVAR(Float, r_grain, 0.f, "film grain / dither strength (0-1)")
FEATURE_CVAR(Int, r_gpuparticles, 0, "visual-only GPU particles for impacts, blood and explosions: 0 off, 1 normal, 2 dense, 3 cinematic/adaptive")
FEATURE_CVAR(Bool, r_emissive, false, "HDR emission: brightmapped and fullbright texels and additive lights may exceed white and drive bloom")
FEATURE_CVAR(Float, r_emissive_strength, 4.f, "linear intensity of HDR emissive texels (1 = classic brightness)")
FEATURE_CVAR(Float, r_bloom_threshold, 1.f, "modern bloom: linear HDR brightness where bloom starts")
FEATURE_CVAR(Float, r_bloom_knee, 0.35f, "modern bloom: width of the soft threshold knee")
FEATURE_CVAR(Float, r_exposure, 0.f, "exposure bias in EV applied before tone mapping")
FEATURE_CVAR(Bool, r_autoexposure, false, "eye adaptation: adjust exposure to the average scene brightness")
FEATURE_CVAR(Float, r_autoexposure_min, -1.5f, "eye adaptation: lowest exposure change in EV")
FEATURE_CVAR(Float, r_autoexposure_max, 0.75f, "eye adaptation: highest exposure change in EV")
FEATURE_CVAR(Float, r_autoexposure_speed, 1.5f, "eye adaptation speed (per second)")
FEATURE_CVAR(Float, r_autoexposure_key, 0.04f, "eye adaptation: linear average brightness the exposure aims for")
FEATURE_CVAR(Float, r_tonemap_white, 4.f, "linear HDR value that the filmic tone mapper maps to display white")
FEATURE_CVAR(String, r_grading_lut, "", "colour grading LUT texture (a 32x32x32 strip, 1024x32), empty for none")
FEATURE_CVAR(Float, r_grade_contrast, 1.f, "colour grading: contrast around middle grey")
FEATURE_CVAR(Float, r_grade_saturation, 1.f, "colour grading: saturation")
FEATURE_CVAR(Color, r_grade_shadowtint, 0xffffff, "colour grading: tint of the shadows (white = none)")
FEATURE_CVAR(Color, r_grade_hightint, 0xffffff, "colour grading: tint of the highlights (white = none)")
FEATURE_CVAR(Bool, r_weapon_prebloom, false, "draw the weapon sprite before bloom and exposure (after TAA), so its emissive parts bloom")
FEATURE_CVAR(Bool, r_taa_reactive, false, "mark moving sprites, particles, liquids and translucency so TAA and MetalFX keep less history there")
FEATURE_CVAR(Bool, r_sprite_depth, false, "opaque sprites write depth so TAA reprojects them")
FEATURE_CVAR(Float, r_mipbias, 0.f, "extra texture LOD bias in the modern presets")

CUSTOM_CVARD(Bool, r_pixelart, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "modern presets: nearest-neighbour magnification with smooth mipmaps for crisp pixel art")
{
	if (r_preset > RP_CLASSIC) gl_texture_filter = self ? 6 : 4;
}
FEATURE_CVAR(Int, r_weather_quality, 0, "map weather (rain, snow): 0 off, 1 normal, 2 high (distant rain sheets)")
FEATURE_CVAR(Bool, r_softparticles, false, "fade particles, weather and soft sprites where they meet the scene (needs the G-buffer pass)")
// Not part of the presets: lets the player ignore the maps' render overrides.
CVARD(Bool, r_map_atmosphere, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "let maps force height fog and bloom and set reflection streaks (MAPINFO forceheightfog, forcebloom, ssrstretch)")

int R_ModernFeatureMask(bool gbuffer)
{
	int mask = 0;
	if (r_autonormal) mask |= RF_AUTO_NORMAL;
	if (r_specular) mask |= RF_SPECULAR;
	if (r_gi) mask |= RF_IRRADIANCE;
	if (r_parallax) mask |= RF_PARALLAX;
	if (r_sprite_pbr) mask |= RF_SPRITE_PBR;
	if (r_sprite_pbr && r_sprite_reflections) mask |= RF_SPRITE_REFLECTIONS;
	if (gbuffer && r_ssr > 0) mask |= RF_REFLECTIONS;
	if (r_emissive) mask |= RF_HDR_EMISSIVE;
	// Sprite switches must not change lighting on unmapped sprites or walls.
	if (mask & ~(RF_SPRITE_PBR | RF_SPRITE_REFLECTIONS)) mask |= RF_MODERN_LIGHT;
	return mask;
}

float R_SceneMipBias(bool temporal, float upscale)
{
	if (r_preset <= RP_CLASSIC) return 0.f;
	float bias = r_mipbias;
	// The resolve accumulates subpixel detail, so sample the output resolution's mip.
	if (temporal) bias += std::log2(std::clamp(upscale, 0.5f, 1.f)) - 0.25f;
	return std::clamp(bias, -2.f, 2.f);
}

struct PresetValues
{
	bool gi, autonormal, specular, parallax, bloom, taa, autoexposure;
	int ssao, ssr, ssgi, volumetric, tonemap, shadowfilter, shadowquality, fxaa, spriteshadow, particles;
	float sharpen, vignette, grain;
	int weather;
	bool softparticles;
};

static const PresetValues presets[] =
{
	// Sprite shadows (ss) ground actors as contact shadows: 1 default, 2 always.
	// TAA replaces FXAA from high up; a little more sharpening restores texel detail.
	// Particles (pt): 1 normal, 2 dense. Eye adaptation (ae) from high up.
	// Weather (wx): 1 normal, 2 high. Soft particles (soft) need SSAO's linear depth.
	//  gi     anorm  spec   pom    bloom  taa    ae     ssao ssr gi vol tm sf  sq    fxaa ss pt sharpen vig    grain  wx soft
	{ false, false, false, false, false, false, false, 0,   0,  0, 0,  0, 1, 128,  0,   1, 0, 0.f,    0.f,   0.f,   0, false }, // classic
	{ true,  true,  true,  false, true,  false, false, 0,   0,  0, 0,  6, 1, 128,  0,   1, 1, 0.25f,  0.f,   0.2f,  1, false }, // low
	{ true,  true,  true,  false, true,  false, false, 1,   1,  0, 1,  6, 1, 256,  0,   2, 1, 0.25f,  0.15f, 0.2f,  1, true  }, // medium
	{ true,  true,  true,  true,  true,  true,  true,  2,   2,  0, 1,  6, 2, 512,  0,   2, 2, 0.4f,   0.2f,  0.25f, 2, true  }, // high
	{ true,  true,  true,  true,  true,  true,  true,  3,   2,  1, 2,  6, 3, 1024, 0,   2, 2, 0.4f,   0.2f,  0.25f, 2, true  }, // ultra
};

void R_ApplyRenderPreset(int preset)
{
	if (preset < 0 || preset > RP_ULTRA) return;
	const auto &p = presets[preset];
	applyingPreset = true;
	r_preset = preset;
	r_gi = p.gi;
	r_autonormal = p.autonormal;
	r_specular = p.specular;
	r_parallax = p.parallax;
	r_sprite_pbr = preset > RP_CLASSIC;
	r_sprite_reflections = preset >= RP_MEDIUM;
	r_sprite_debug = 0;
	r_ssr = p.ssr;
	r_ssgi = p.ssgi;
	r_taa = p.taa;
	r_volumetric = p.volumetric;
	r_volumetric_shadows = preset >= RP_ULTRA ? 6 : preset >= RP_HIGH ? 4 : 2;
	r_volumetric_samples = preset >= RP_ULTRA ? 16 : 12;
	r_sharpen = p.sharpen;
	r_vignette = p.vignette;
	r_grain = p.grain;
	r_gpuparticles = p.particles;
	r_weather_quality = p.weather;
	r_softparticles = p.softparticles;
	gl_bloom = p.bloom;
	gl_ssao = p.ssao;
	gl_tonemap = p.tonemap;
	gl_fxaa = p.fxaa;
	gl_shadowmap_filter = p.shadowfilter;
	gl_shadowmap_quality = p.shadowquality;
	r_actorspriteshadow = p.spriteshadow;
	// Fading with height keeps flying monsters from casting floor-glued shadows.
	r_actorspriteshadowfadeheight = p.spriteshadow > 1 ? 128.f : 0.f;
	if (preset > RP_CLASSIC) gl_light_shadowmap = true;
	// HDR emission, weapon bloom and sprite depth belong to every modern preset;
	// the temporal mask only matters with TAA.
	bool modern = preset > RP_CLASSIC;
	r_emissive = modern;
	r_weapon_prebloom = modern;
	r_sprite_depth = modern;
	r_taa_reactive = p.taa;
	r_autoexposure = p.autoexposure;
	gl_texture_filter = modern && r_pixelart ? 6 : 4;
	applyingPreset = false;
}

void R_SetAutoPreset(int preset)
{
	if (!r_preset_auto || preset == r_preset) return;
	R_ApplyRenderPreset(preset);
}

CCMD(r_presets)
{
	Printf("r_preset 0: classic GZDoom presentation\n");
	Printf("r_preset 1: low - baked GI, texture relief, specular, filmic tonemap, bloom, GPU particles, weather\n");
	Printf("r_preset 2: medium - adds ambient occlusion, liquid reflections, half-res volumetrics and height fog, soft particles\n");
	Printf("r_preset 3: high - adds parallax, glossy floor reflections, soft shadows, TAA, eye adaptation, dense particles, distant rain\n");
	Printf("r_preset 4: ultra - adds screen-space GI, full-res volumetrics, highest AO and shadow quality\n");
	Printf("Last applied: %d%s\n", *r_preset, r_preset_auto ? " (chosen for this device)" : "");
}

// Per-map overrides, set every frame for the level being drawn. Kept apart
// from the CVARs so they never end up in the user's ini.
static bool mapForceBloom;
static float mapSSRStretch;
static bool deviceBloom = true;

void R_SetMapAtmosphere(bool forceBloom, float ssrStretch)
{
	mapForceBloom = forceBloom;
	mapSSRStretch = ssrStretch;
}

void R_SetDeviceBloom(bool allowed)
{
	deviceBloom = allowed;
}

bool R_BloomEnabled()
{
	// forcebloom overrides the player's gl_bloom, not the device policy that
	// turned bloom off (weak GPU, low power, heat) while r_preset_auto is on.
	return gl_bloom || (r_map_atmosphere && mapForceBloom && (deviceBloom || !r_preset_auto));
}

float R_SSRStretch()
{
	return r_map_atmosphere && mapSSRStretch > 0.f ? mapSSRStretch : (float)r_ssr_stretch;
}
