#pragma once

#include "c_cvars.h"

// Feature bits passed to the scene shaders through HWViewpointUniforms::mRenderFeatures.
// Zero selects the original lighting path, so classic mode is bit-for-bit unchanged.
enum ERenderFeature
{
	RF_MODERN_LIGHT = 1,      // modern light model (wrap lighting, HDR highlights)
	RF_AUTO_NORMAL = 2,       // derive surface relief from the diffuse texture
	RF_SPECULAR = 4,          // specular highlights from dynamic lights
	RF_IRRADIANCE = 8,        // sample the baked irradiance volume
	RF_REFLECTIONS = 16,      // write reflectivity to the G-buffer for SSR
	RF_PARALLAX = 32,         // parallax occlusion from the diffuse height estimate
	RF_HDR_EMISSIVE = 64,     // brightmaps, fullbright texels and additive lights may exceed 1.0
};

// Quality presets. Higher presets enable more features; each feature also has its
// own CVAR, so the preset is only a starting point.
enum ERenderPreset
{
	RP_CLASSIC = 0,
	RP_LOW = 1,
	RP_MEDIUM = 2,
	RP_HIGH = 3,
	RP_ULTRA = 4,
};

EXTERN_CVAR(Int, r_preset)
EXTERN_CVAR(Bool, r_preset_auto)
EXTERN_CVAR(Bool, r_adaptive_quality)
EXTERN_CVAR(Bool, r_raytrace_auto)
EXTERN_CVAR(Int, r_adaptive_targetfps)
EXTERN_CVAR(Bool, r_adaptive_debug)
EXTERN_CVAR(Bool, r_gi)
EXTERN_CVAR(Float, r_gi_strength)
EXTERN_CVAR(Float, r_gi_saturation)
EXTERN_CVAR(Int, r_gi_quality)
EXTERN_CVAR(Bool, r_autonormal)
EXTERN_CVAR(Float, r_autonormal_strength)
EXTERN_CVAR(Bool, r_specular)
EXTERN_CVAR(Float, r_specular_strength)
EXTERN_CVAR(Bool, r_parallax)
EXTERN_CVAR(Int, r_ssr)
EXTERN_CVAR(Float, r_ssr_strength)
EXTERN_CVAR(Float, r_ssr_stretch)
EXTERN_CVAR(Int, r_ssgi)
EXTERN_CVAR(Float, r_ssgi_strength)
EXTERN_CVAR(Int, r_volumetric)
EXTERN_CVAR(Float, r_volumetric_strength)
EXTERN_CVAR(Int, r_volumetric_shadows)
EXTERN_CVAR(Int, r_volumetric_samples)
EXTERN_CVAR(Float, r_volumetric_distance)
EXTERN_CVAR(Float, r_volumetric_fogscatter)
EXTERN_CVAR(Bool, r_taa)
EXTERN_CVAR(Float, r_upscale)
EXTERN_CVAR(Bool, r_upscale_metalfx)
EXTERN_CVAR(Bool, r_bloom_modern)
EXTERN_CVAR(Float, r_sharpen)
EXTERN_CVAR(Float, r_vignette)
EXTERN_CVAR(Float, r_grain)
EXTERN_CVAR(Int, r_gpuparticles)
EXTERN_CVAR(Bool, r_emissive)
EXTERN_CVAR(Float, r_emissive_strength)
EXTERN_CVAR(Float, r_bloom_threshold)
EXTERN_CVAR(Float, r_bloom_knee)
EXTERN_CVAR(Float, r_exposure)
EXTERN_CVAR(Bool, r_autoexposure)
EXTERN_CVAR(Float, r_autoexposure_min)
EXTERN_CVAR(Float, r_autoexposure_max)
EXTERN_CVAR(Float, r_autoexposure_speed)
EXTERN_CVAR(Float, r_autoexposure_key)
EXTERN_CVAR(Float, r_tonemap_white)
EXTERN_CVAR(String, r_grading_lut)
EXTERN_CVAR(Float, r_grade_contrast)
EXTERN_CVAR(Float, r_grade_saturation)
EXTERN_CVAR(Color, r_grade_shadowtint)
EXTERN_CVAR(Color, r_grade_hightint)
EXTERN_CVAR(Bool, r_weapon_prebloom)
EXTERN_CVAR(Bool, r_taa_reactive)
EXTERN_CVAR(Bool, r_sprite_depth)
EXTERN_CVAR(Bool, r_pixelart)
EXTERN_CVAR(Float, r_mipbias)
EXTERN_CVAR(Int, r_weather_quality)
EXTERN_CVAR(Bool, r_softparticles)

int R_ModernFeatureMask(bool gbuffer);
// Texture LOD bias for the scene shaders: negative while TAA or upscaling resolves subpixel detail.
float R_SceneMipBias(bool temporal, float upscale);
void R_ApplyRenderPreset(int preset);

// Lets platform code pick a preset for the device. It is applied while
// r_preset_auto is set; choosing r_preset by hand turns r_preset_auto off.
void R_SetAutoPreset(int preset);
