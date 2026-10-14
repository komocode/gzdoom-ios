/*
** hw_volumetric.cpp
** Light and fog selection for the volumetric pass. See hw_volumetric.h.
*/

#include "hw_volumetric.h"
#include "hw_modernrender.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "scene/hw_drawinfo.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "actor.h"
#include "i_time.h"
#include "v_video.h"
#include "c_cvars.h"
#include "stats.h"

#include <algorithm>
#include <cmath>

CVARD(Bool, r_heightfog, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "draw the maps' height fog (needs r_volumetric)")
// Overrides of the level's height fog for testing: used while the density is above 0.
CVARD(Float, r_heightfog_density, 0.f, 0, "test height fog: density per 1000 map units (0 = use the map's)")
CVARD(Float, r_heightfog_base, 0.f, 0, "test height fog: height below which the density is constant")
CVARD(Float, r_heightfog_falloff, 64.f, 0, "test height fog: height over which the density falls to 1/e")
CVARD(Float, r_heightfog_noise, 0.f, 0, "test height fog: amount of rolling noise (0-1)")
CVARD(Color, r_heightfog_color, 0x80888c, 0, "test height fog: colour")

static TArray<VolumetricLight> VolumetricLights;

const TArray<VolumetricLight> &R_GetVolumetricLights()
{
	return VolumetricLights;
}

static int VolumetricCandidates;
static float VolumetricFogDensity, VolumetricSectorFog;

ADD_STAT(volumetric)
{
	int shadowed = 0, spots = 0;
	for (auto &light : VolumetricLights)
	{
		if (light.Shadowed) shadowed++;
		if (light.SpotDir.LengthSquared() > 0) spots++;
	}
	FString out;
	out.Format("Volumetric lights: %d of %d candidates, %d shadowed%s, %d spot\nHeight fog %.2f, sector fog %.2f per 1000 units",
		VolumetricLights.Size(), VolumetricCandidates, shadowed, hw_postprocess.sceneEffects.VolumetricRayQuery() ? " (ray traced)" : "", spots,
		VolumetricFogDensity * 1000.0f, VolumetricSectorFog * 1000.0f);
	return out;
}

//-----------------------------------------------------------------------------
//
// The camera sector's coloured fog, in extinction per map unit. It matches
// the distance fog the scene shaders apply (SetFog), so halos fade with it.
// Black fog is Doom's light diminishing, not a medium.
//
//-----------------------------------------------------------------------------

static float CameraSectorFog(HWDrawInfo *di)
{
	auto Level = di->Level;
	sector_t *sector = di->Viewpoint.sector;
	if (!sector || gl_fogmode == 0 || di->isFullbrightScene())
		return 0.0f;

	float density;
	if (Level->flags & LEVEL_HASFADETABLE)
	{
		density = 70.0f;
	}
	else
	{
		PalEntry fogcolor = sector->Colormap.FadeColor;
		if ((fogcolor.d & 0xffffff) == 0)
			return 0.0f;
		density = GetFogDensity(Level, di->lightmode, sector->lightlevel, fogcolor, sector->Colormap.FogDensity, sector->Colormap.BlendFactor);
	}
	return density / 64000.0f;
}

static VolumetricFog CollectFog(HWDrawInfo *di)
{
	VolumetricFog fog;
	auto Level = di->Level;
	fog.SectorDensity = CameraSectorFog(di);
	if (!r_heightfog)
		return fog;

	double density = Level->heightfogdensity;
	double base = Level->heightfogbase;
	double falloff = Level->heightfogfalloff;
	double noise = Level->heightfognoise;
	PalEntry color = Level->heightfogcolor;
	if (r_heightfog_density > 0.0f)
	{
		density = r_heightfog_density;
		base = r_heightfog_base;
		falloff = r_heightfog_falloff;
		noise = r_heightfog_noise;
		color = r_heightfog_color;
	}
	if (density <= 0.0)
		return fog;

	double time = (Level->maptime + di->Viewpoint.TicFrac) / TICRATE;
	fog.Density = float(density / 1000.0);
	fog.Base = float(base);
	fog.Falloff = float(std::max(falloff, 1.0));
	fog.Noise = float(clamp(noise, 0.0, 1.0));
	fog.Color = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f };
	// The noise scrolls with the wind (map x, y = GL x, z) and churns slowly.
	fog.NoiseScale = 1.0f / 160.0f;
	fog.NoiseOffset = { float(-Level->heightfogwind.X * time * fog.NoiseScale), float(time * 0.08), float(-Level->heightfogwind.Y * time * fog.NoiseScale) };
	return fog;
}

//-----------------------------------------------------------------------------
//
// A light's scattering can only be visible if it reaches an area the BSP walk
// rendered this frame. The touch lists stop at one-sided walls.
//
//-----------------------------------------------------------------------------

static bool LightReachesRenderedArea(HWDrawInfo *di, FDynamicLight *light)
{
	auto &sections = di->Level->sections;
	for (FSection *section : light->touchlists.flat_tlist)
	{
		if (di->section_renderflags[sections.SectionIndex(section)] & SSRF_PROCESSED)
			return true;
	}
	if (light->touchlists.flat_tlist.Size() == 0 && light->Sector)
	{
		auto sub = di->Level->PointInRenderSubsector(light->Pos.XY());
		return sub && sub->section && (di->section_renderflags[sections.SectionIndex(sub->section)] & SSRF_PROCESSED);
	}
	return false;
}

void R_CollectVolumetrics(HWDrawInfo *di)
{
	static uint64_t lastTime = 0;
	uint64_t now = I_msTime();
	float dt = lastTime ? clamp((now - lastTime) / 1000.0f, 0.0f, 0.25f) : 0.0f;
	lastTime = now;

	VolumetricLights.Clear();
	auto &effects = hw_postprocess.sceneEffects;
	auto Level = di->Level;
	if (r_volumetric <= 0)
	{
		effects.SetVolumetricLights(nullptr, 0);
		effects.SetVolumetricFog({});
		return;
	}

	VolumetricFog fog = CollectFog(di);
	VolumetricFogDensity = fog.Density;
	VolumetricSectorFog = fog.SectorDensity;
	effects.SetVolumetricFog(fog);
	effects.SetVolumetricView(di->VPUniforms.mViewMatrix.get());

	struct Candidate { float score; FDynamicLight *light; float fade; };
	TArray<Candidate> candidates;

	auto &vp = di->Viewpoint;
	DVector3 camera = vp.Pos;
	DVector3 forward(vp.Angles.Yaw.Cos() * vp.Angles.Pitch.Cos(), vp.Angles.Yaw.Sin() * vp.Angles.Pitch.Cos(), -vp.Angles.Pitch.Sin());
	// Generous half angle of the view frustum's corners.
	double halfFov = atan(tan(vp.FieldOfView.Radians() * 0.5) * 1.3) + 0.1;
	float maxDistance = std::max((float)r_volumetric_distance, 256.0f);

	for (FDynamicLight *light = Level->lights; light; light = light->next)
	{
		float radius = light->GetRadius();
		float r = light->GetRed() / 255.f, g = light->GetGreen() / 255.f, b = light->GetBlue() / 255.f;
		float brightness = std::max({ r, g, b }) * (float)light->GetLightDefIntensity();
		bool candidate = light->IsActive() && !light->IsSubtractive() && light->visibletoplayer && radius > 0 && brightness > 0.02f;
		// The camera's own lights (weapon flashes) would fill the whole view with scattering.
		if (light->target && light->target == vp.camera) candidate = false;

		// Fade with the distance to the light's sphere instead of popping.
		DVector3 delta = light->Pos - camera;
		double dist = delta.Length();
		float distanceFade = clamp(float((maxDistance - std::max(dist - radius, 0.0)) / (maxDistance * 0.25)), 0.0f, 1.0f);
		if (distanceFade <= 0.0f) candidate = false;

		// Lights whose sphere is outside the view frustum scatter nothing visible.
		if (candidate && dist > radius)
		{
			double angle = acos(clamp((delta | forward) / dist, -1.0, 1.0)) - asin(clamp(radius / dist, 0.0, 1.0));
			if (angle > halfFov) candidate = false;
		}

		float target = candidate && LightReachesRenderedArea(di, light) ? 1.0f : 0.0f;
		float rate = target > light->volumetricFade ? 8.0f : 3.0f;
		light->volumetricFade = dt > 0.0f ? clamp(light->volumetricFade + clamp(target - light->volumetricFade, -rate * dt, rate * dt), 0.0f, 1.0f) : target;
		if (!candidate || light->volumetricFade <= 0.001f)
		{
			light->volumetricShadowed = false;
			continue;
		}

		float fade = light->volumetricFade * distanceFade;
		candidates.Push({ brightness * radius / float(dist + 64.0) * fade, light, fade });
	}

	VolumetricCandidates = candidates.Size();
	std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
	int count = std::min((int)candidates.Size(), (int)MaxVolumetricLights);

	// Shadowed shafts for the most important lights. A light keeps its
	// shadows unless another one is clearly more important, so the budget
	// doesn't flip between lights of similar importance.
	bool rayQuery = effects.VolumetricRayQuery();
	bool shadowMaps = screen->mShadowMap.Enabled();
	TArray<int> shadowOrder;
	for (int i = 0; i < count; i++)
	{
		FDynamicLight *light = candidates[i].light;
		bool canShadow = light->shadowmapped && (rayQuery || (shadowMaps && light->mShadowmapIndex < 1024));
		if (canShadow) shadowOrder.Push(i);
	}
	std::stable_sort(shadowOrder.begin(), shadowOrder.end(), [&](int a, int b)
	{
		float sa = candidates[a].score * (candidates[a].light->volumetricShadowed ? 1.5f : 1.0f);
		float sb = candidates[b].score * (candidates[b].light->volumetricShadowed ? 1.5f : 1.0f);
		return sa > sb;
	});
	bool shadowed[MaxVolumetricLights] = {};
	for (unsigned i = 0; i < shadowOrder.Size() && (int)i < r_volumetric_shadows; i++)
		shadowed[shadowOrder[i]] = true;

	FVector4 data[MaxVolumetricLights * VolumetricLightVec4s];
	for (int i = 0; i < count; i++)
	{
		FDynamicLight *light = candidates[i].light;
		float fade = candidates[i].fade * (float)light->GetLightDefIntensity();
		float radius = light->GetRadius();

		VolumetricLight vl;
		vl.Light = light;
		vl.Pos = { (float)light->X(), (float)light->Z(), (float)light->Y() };
		vl.Radius = radius;
		vl.Color = { light->GetRed() / 255.f * fade, light->GetGreen() / 255.f * fade, light->GetBlue() / 255.f * fade };
		vl.Core = radius * 0.3f;
		vl.SpotDir = { 0.0f, 0.0f, 0.0f };
		vl.SpotCosOuter = vl.SpotCosInner = 0.0f;
		if (light->IsSpot() && light->target)
		{
			// Same direction as the scene shaders' spot lights, pointing along the beam.
			DAngle negPitch = -*light->pPitch;
			DAngle angle = light->target->Angles.Yaw;
			double xzLen = negPitch.Cos();
			vl.SpotDir = { float(angle.Cos() * xzLen), float(negPitch.Sin()), float(angle.Sin() * xzLen) };
			vl.SpotCosInner = (float)light->pSpotInnerAngle->Cos();
			vl.SpotCosOuter = (float)light->pSpotOuterAngle->Cos();
			// Beams are thin, so they get a tighter core than point light halos.
			vl.Core = radius * 0.2f;
		}
		vl.ShadowMapIndex = shadowMaps && light->mShadowmapIndex < 1024 ? light->mShadowmapIndex : -1;
		vl.Shadowed = shadowed[i];
		light->volumetricShadowed = vl.Shadowed;
		VolumetricLights.Push(vl);

		FVector4 *d = &data[i * VolumetricLightVec4s];
		d[0] = { vl.Pos.X, vl.Pos.Y, vl.Pos.Z, vl.Radius };
		d[1] = { vl.Color.X, vl.Color.Y, vl.Color.Z, vl.Core };
		d[2] = { vl.SpotDir.X, vl.SpotDir.Y, vl.SpotDir.Z, 0.0f };
		d[3] = { vl.SpotCosOuter, vl.SpotCosInner, (float)vl.ShadowMapIndex, vl.Shadowed ? 1.0f : 0.0f };
	}
	for (int i = count; i < (int)candidates.Size(); i++)
		candidates[i].light->volumetricShadowed = false;

	effects.SetVolumetricLights(data, count);
}
