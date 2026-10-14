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

EXTERN_CVAR(Int, gl_ssao)
EXTERN_CVAR(Bool, gl_bloom)
EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, gl_shadowmap_filter)
EXTERN_CVAR(Int, gl_shadowmap_quality)
EXTERN_CVAR(Bool, gl_light_shadowmap)
EXTERN_CVAR(Int, gl_fxaa)
EXTERN_CVAR(Int, r_actorspriteshadow)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)

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
FEATURE_CVAR(Bool, r_parallax, false, "parallax occlusion mapping from texture height estimates")
FEATURE_CVAR(Int, r_ssr, 0, "screen-space reflections: 0 off, 1 liquids, 2 liquids and glossy floors")
FEATURE_CVAR(Float, r_ssr_strength, 1.f, "strength of screen-space reflections")
FEATURE_CVAR(Int, r_ssgi, 0, "screen-space indirect light from dynamic sources: 0 off, 1 on")
FEATURE_CVAR(Float, r_ssgi_strength, 1.f, "strength of screen-space indirect light")
FEATURE_CVAR(Int, r_volumetric, 0, "volumetric light scattering: 0 off, 1 half resolution, 2 full resolution")
FEATURE_CVAR(Float, r_volumetric_strength, 1.f, "strength of volumetric light scattering")
FEATURE_CVAR(Bool, r_taa, false, "temporal anti-aliasing with subpixel jitter and reprojected history")
FEATURE_CVAR(Float, r_upscale, 1.f, "temporal upscaling with TAA: 3D render scale from 0.5 to 1 (1 = off); the HUD stays at full resolution")
FEATURE_CVAR(Bool, r_upscale_metalfx, true, "use MetalFX for temporal upscaling when the Metal renderer provides it")
FEATURE_CVAR(Bool, r_bloom_modern, true, "use the energy-conserving progressive bloom")
FEATURE_CVAR(Float, r_sharpen, 0.f, "contrast-adaptive sharpening amount (0-1)")
FEATURE_CVAR(Float, r_vignette, 0.f, "vignette strength (0-1)")
FEATURE_CVAR(Float, r_grain, 0.f, "film grain / dither strength (0-1)")
FEATURE_CVAR(Int, r_gpuparticles, 0, "visual-only GPU particles for impacts, blood and explosions: 0 off, 1 normal, 2 dense")

int R_ModernFeatureMask(bool gbuffer)
{
	int mask = 0;
	if (r_autonormal) mask |= RF_AUTO_NORMAL;
	if (r_specular) mask |= RF_SPECULAR;
	if (r_gi) mask |= RF_IRRADIANCE;
	if (r_parallax) mask |= RF_PARALLAX;
	if (gbuffer && r_ssr > 0) mask |= RF_REFLECTIONS;
	if (mask) mask |= RF_MODERN_LIGHT;
	return mask;
}

struct PresetValues
{
	bool gi, autonormal, specular, parallax, bloom, taa;
	int ssao, ssr, ssgi, volumetric, tonemap, shadowfilter, shadowquality, fxaa, spriteshadow, particles;
	float sharpen, vignette, grain;
};

static const PresetValues presets[] =
{
	// Sprite shadows (ss) ground actors as contact shadows: 1 default, 2 always.
	// TAA replaces FXAA from high up; a little more sharpening restores texel detail.
	// Particles (pt): 1 normal, 2 dense.
	//  gi     anorm  spec   pom    bloom  taa    ssao ssr gi vol tm sf  sq    fxaa ss pt sharpen vig    grain
	{ false, false, false, false, false, false, 0,   0,  0, 0,  0, 1, 128,  0,   1, 0, 0.f,    0.f,   0.f  }, // classic
	{ true,  true,  true,  false, true,  false, 0,   0,  0, 0,  6, 1, 128,  0,   1, 1, 0.25f,  0.f,   0.2f }, // low
	{ true,  true,  true,  false, true,  false, 1,   1,  0, 1,  6, 1, 256,  0,   2, 1, 0.25f,  0.15f, 0.2f }, // medium
	{ true,  true,  true,  true,  true,  true,  2,   2,  0, 1,  6, 2, 512,  0,   2, 2, 0.4f,   0.2f,  0.25f }, // high
	{ true,  true,  true,  true,  true,  true,  3,   2,  1, 2,  6, 3, 1024, 0,   2, 2, 0.4f,   0.2f,  0.25f }, // ultra
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
	r_ssr = p.ssr;
	r_ssgi = p.ssgi;
	r_taa = p.taa;
	r_volumetric = p.volumetric;
	r_sharpen = p.sharpen;
	r_vignette = p.vignette;
	r_grain = p.grain;
	r_gpuparticles = p.particles;
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
	Printf("r_preset 1: low - baked GI, texture relief, specular, filmic tonemap, bloom, GPU particles\n");
	Printf("r_preset 2: medium - adds ambient occlusion, liquid reflections, half-res volumetrics\n");
	Printf("r_preset 3: high - adds parallax, glossy floor reflections, soft shadows, TAA, dense particles\n");
	Printf("r_preset 4: ultra - adds screen-space GI, full-res volumetrics, highest AO and shadow quality\n");
	Printf("Last applied: %d%s\n", *r_preset, r_preset_auto ? " (chosen for this device)" : "");
}
