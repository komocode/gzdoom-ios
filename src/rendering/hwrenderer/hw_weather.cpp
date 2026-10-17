/*
** hw_weather.cpp
** Visual-only rain and snow, and the sky exposure map. See hw_weather.h.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#include "hw_weather.h"
#include "hw_gpuparticles.h"
#include "hw_modernrender.h"
#include "hw_renderstate.h"
#include "hw_viewpointuniforms.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "scene/hw_drawinfo.h"
#include "buffers.h"
#include "flatvertices.h"
#include "g_levellocals.h"
#include "p_3dfloors.h"
#include "r_sky.h"
#include "texturemanager.h"
#include "v_video.h"
#include "c_cvars.h"
#include "printf.h"
#include "doomdef.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Testing overrides of the map's weather (not saved).
CVARD(Int, r_weather, 0, 0, "override the map's weather: 0 map default, 1 rain, 2 snow, -1 none")
CVARD(Float, r_weather_density, -1.f, 0, "override the weather density (0-2); -1 uses the map's")
CVARD(Float, r_weather_wind, -1.f, 0, "override the wind speed in map units per second; -1 uses the map's")
CVARD(Float, r_weather_winddir, 0.f, 0, "wind direction in degrees, used with r_weather_wind")

namespace
{

// Drops per layer and splash slots; the weather vertex shader knows the layout:
// layer 0 (near), layer 1, ripples, splash bursts, then layer 2 (far, quality 2 only).
constexpr int LayerDrops = 8192;
constexpr int SplashSlots = 3072;
constexpr int NearQuads = LayerDrops * 2 + SplashSlots * 2;
constexpr int AllQuads = NearQuads + LayerDrops;

struct WeatherVertex
{
	float index, corner, kind, unused;
};

// Sky exposure map
struct ExposureMap
{
	FLevelLocals *level = nullptr;
	bool requested = false;
	bool bound = false;
	int width = 0, height = 0;
	double originX = 0, originY = 0, cellSize = 16;
	TArray<uint16_t> texels;	// RGBA16F
	TArray<int> cellSector;
	TArray<double> signatures;
	int lastCheck = 0;
};

ExposureMap exposure;

uint16_t ToHalf(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, 4);
	uint32_t sign = (bits >> 16) & 0x8000;
	f = std::min(std::fabs(f), 65000.f);
	memcpy(&bits, &f, 4);
	int exponent = int((bits >> 23) & 0xff) - 127 + 15;
	if (exponent <= 0) return uint16_t(sign);
	uint32_t mantissa = (bits & 0x7fffff) + 0x1000; // round to nearest
	if (mantissa & 0x800000)
	{
		mantissa = 0;
		exponent++;
	}
	if (exponent >= 31) return uint16_t(sign | 0x7bff);
	return uint16_t(sign | (exponent << 10) | (mantissa >> 13));
}

// Inverse of ToHalf, which writes no denormals, infinities or NaNs.
float FromHalf(uint16_t h)
{
	uint32_t bits = uint32_t(h & 0x8000) << 16;
	int exponent = (h >> 10) & 0x1f;
	if (exponent) bits |= uint32_t(exponent - 15 + 127) << 23 | uint32_t(h & 0x3ff) << 13;
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

bool HasSkyCeiling(sector_t *sec)
{
	FSectorPortal *portal = sec->ValidatePortal(sector_t::ceiling);
	if (portal != nullptr) return portal->mType == PORTS_SKYVIEWPOINT;
	return sec->GetTexture(sector_t::ceiling) == skyflatnum;
}

// Changes when anything that decides where rain stops in this sector moves.
double SectorSignature(sector_t *sec)
{
	double s = sec->floorplane.fD() + sec->ceilingplane.fD() * 3.0 + (HasSkyCeiling(sec) ? 7.0 : 0.0);
	for (F3DFloor *ff : sec->e->XFloor.ffloors)
		s += ff->top.plane->fD() * 5.0 + ((ff->flags & FF_EXISTS) ? 11.0 : 0.0);
	return s;
}

void EvaluateCell(int index)
{
	FLevelLocals *level = exposure.level;
	double x = exposure.originX + (index % exposure.width + 0.5) * exposure.cellSize;
	double y = exposure.originY + (index / exposure.width + 0.5) * exposure.cellSize;
	sector_t *sec = level->PointInSector(DVector2(x, y));
	exposure.cellSector[index] = sec->Index();

	double ceiling = sec->ceilingplane.ZatPoint(x, y);
	double stop = sec->floorplane.ZatPoint(x, y);
	bool open = HasSkyCeiling(sec);
	if (open)
	{
		// Rain stops on the highest visible or solid 3D floor below the sky,
		// so bridges, roofs and water surfaces made of 3D floors shelter what's under them.
		for (F3DFloor *ff : sec->e->XFloor.ffloors)
		{
			if (!(ff->flags & FF_EXISTS) || !(ff->flags & (FF_SOLID | FF_RENDERPLANES))) continue;
			double top = ff->top.plane->ZatPoint(x, y);
			if (top > stop && top <= ceiling + 1) stop = top;
		}
	}

	uint16_t *texel = &exposure.texels[index * 4];
	texel[0] = ToHalf(float(stop));
	texel[1] = ToHalf(float(ceiling));
	texel[2] = ToHalf(sec->lightlevel / 255.f);
	texel[3] = ToHalf(open ? 1.f : 0.f);
}

void Upload()
{
	exposure.bound = screen->InitWeatherMap(exposure.width, exposure.height, exposure.texels);
}

void BuildMap(FLevelLocals *level)
{
	exposure.level = level;
	exposure.bound = false;
	if (level->vertexes.Size() == 0 || level->sectors.Size() == 0) return;

	double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
	for (auto &v : level->vertexes)
	{
		minX = std::min(minX, v.fX());
		minY = std::min(minY, v.fY());
		maxX = std::max(maxX, v.fX());
		maxY = std::max(maxY, v.fY());
	}
	// 16 unit cells, coarser on very large maps (at most 512 cells a side).
	double extent = std::max(maxX - minX, maxY - minY);
	exposure.cellSize = std::max(16.0, std::ceil(extent / 512.0 / 8.0) * 8.0);
	exposure.originX = minX - exposure.cellSize;
	exposure.originY = minY - exposure.cellSize;
	exposure.width = int((maxX - exposure.originX) / exposure.cellSize) + 2;
	exposure.height = int((maxY - exposure.originY) / exposure.cellSize) + 2;

	int count = exposure.width * exposure.height;
	exposure.texels.Resize(count * 4);
	exposure.cellSector.Resize(count);
	for (int i = 0; i < count; i++) EvaluateCell(i);

	exposure.signatures.Resize(level->sectors.Size());
	for (unsigned i = 0; i < level->sectors.Size(); i++) exposure.signatures[i] = SectorSignature(&level->sectors[i]);
	exposure.lastCheck = level->maptime;
	Upload();
	DPrintf(DMSG_NOTIFY, "Weather: %dx%d sky exposure cells of %.0f units\n", exposure.width, exposure.height, exposure.cellSize);
}

// Doors, lifts and moving 3D floors: re-evaluate the cells of the sectors that changed.
void UpdateMovedSectors(FLevelLocals *level)
{
	int interval = exposure.width * exposure.height > 65536 ? 16 : 4;
	if (level->maptime - exposure.lastCheck < interval && level->maptime >= exposure.lastCheck) return;
	exposure.lastCheck = level->maptime;

	TArray<bool> changed(level->sectors.Size(), true);
	bool any = false;
	for (unsigned i = 0; i < level->sectors.Size(); i++)
	{
		double s = SectorSignature(&level->sectors[i]);
		changed[i] = s != exposure.signatures[i];
		if (changed[i])
		{
			exposure.signatures[i] = s;
			any = true;
		}
	}
	if (!any) return;

	for (unsigned i = 0; i < exposure.cellSector.Size(); i++)
	{
		if (changed[exposure.cellSector[i]]) EvaluateCell(i);
	}
	Upload();
}

FWeatherSettings EffectiveWeather(FLevelLocals *level)
{
	FWeatherSettings w = level->weather;
	if (r_weather < 0) w.Type = WEATHER_None;
	else if (r_weather == 1) w.Type = WEATHER_Rain;
	else if (r_weather == 2) w.Type = WEATHER_Snow;
	if (r_weather_density >= 0) w.Density = r_weather_density;
	if (r_weather_wind >= 0)
	{
		w.WindSpeed = r_weather_wind;
		w.WindAngle = r_weather_winddir;
	}
	return w;
}

float WeatherTime(FLevelLocals *level, double ticFrac)
{
	return float((level->maptime + ticFrac) / TICRATE) + 1.f;
}

float FlashIntensity(FLevelLocals *level, double ticFrac)
{
	if (r_weather_quality <= 0 || level->weatherFlashDuration <= 0) return 0;
	float t = float((level->maptime + ticFrac) / TICRATE) - level->weatherFlashStart;
	if (t < 0 || t >= level->weatherFlashDuration) return 0;
	float u = 1.f - t / level->weatherFlashDuration;
	// A bright strike that flickers once as it fades.
	float flicker = 0.8f + 0.2f * std::cos(t * 40.f);
	return std::clamp(level->weatherFlashStrength, 0.f, 2.f) * u * u * flicker;
}

bool CreateBuffers()
{
	if (screen->mWeatherVertices) return true;

	auto vb = screen->CreateVertexBuffer();
	auto ib = screen->CreateIndexBuffer();
	if (!vb || !ib)
	{
		delete vb;
		delete ib;
		return false;
	}

	TArray<WeatherVertex> vertices(AllQuads * 4, true);
	int quad = 0;
	auto addQuads = [&](int count, int kind)
	{
		for (int i = 0; i < count; i++, quad++)
			for (int c = 0; c < 4; c++)
				vertices[quad * 4 + c] = { float(i), float(c), float(kind), 0.f };
	};
	addQuads(LayerDrops, 0);
	addQuads(LayerDrops, 1);
	addQuads(SplashSlots, 3);
	addQuads(SplashSlots, 4);
	addQuads(LayerDrops, 2);

	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_VERTEX, VFmt_Float4, (int)myoffsetof(WeatherVertex, index) },
	};
	vb->SetData(vertices.Size() * sizeof(WeatherVertex), vertices.Data(), BufferUsageType::Static);
	vb->SetFormat(1, 1, sizeof(WeatherVertex), format);

	TArray<uint32_t> indices(AllQuads * 6, true);
	for (uint32_t i = 0; i < (uint32_t)AllQuads; i++)
	{
		static const uint32_t order[6] = { 0, 1, 2, 2, 1, 3 };
		for (int j = 0; j < 6; j++) indices[i * 6 + j] = i * 4 + order[j];
	}
	ib->SetData(indices.Size() * sizeof(uint32_t), indices.Data(), BufferUsageType::Static);

	screen->mWeatherVertices = vb;
	screen->mWeatherIndices = ib;
	return true;
}

} // namespace

void Weather_LevelLoaded(FLevelLocals *level)
{
	exposure.level = level;
	exposure.requested = true;
	exposure.bound = false;
}

void Weather_LevelUnloaded()
{
	exposure.level = nullptr;
	exposure.requested = false;
	exposure.bound = false;
	exposure.texels.Reset();
	exposure.cellSector.Reset();
	exposure.signatures.Reset();
	if (screen) screen->InitWeatherMap(0, 0, exposure.texels);
}

void Weather_Update(FLevelLocals *level)
{
	if (level == nullptr || level != primaryLevel) return;
	if (exposure.requested || exposure.level != level)
	{
		exposure.requested = false;
		BuildMap(level);
	}
	else if (exposure.bound)
	{
		UpdateMovedSectors(level);
	}
}

void Weather_SetUniforms(HWDrawInfo *di, HWViewpointUniforms &uniforms)
{
	FLevelLocals *level = di->Level;
	// The w of the weather parameters carries the map's wettexels (main.fp).
	float wetTexels = level ? level->atmosphere.WetTexels : 0.f;
	if (exposure.bound && exposure.level == level)
	{
		uniforms.mWeatherMap = { float(exposure.originX), float(exposure.originY), float(1.0 / exposure.cellSize), float(exposure.width) };
		uniforms.mWeatherParams = { float(exposure.height), FlashIntensity(level, di->Viewpoint.TicFrac), WeatherTime(level, di->Viewpoint.TicFrac), wetTexels };
	}
	else
	{
		uniforms.mWeatherMap = { 0, 0, 0, 0 };
		uniforms.mWeatherParams = { 0, 0, 0, wetTexels };
	}

	// Maps a scene fragment position to the half-resolution linear depth of
	// the scene viewport, which the AO pass writes (soft particles).
	const IntRect &vp = screen->mSceneViewport;
	int aw = hw_postprocess.ssao.GetDepthWidth(), ah = hw_postprocess.ssao.GetDepthHeight();
	if (aw > 0 && ah > 0 && vp.width > 0 && vp.height > 0)
	{
		float sx = aw / float(vp.width), sy = ah / float(vp.height);
		uniforms.mSceneDepthMap = { sx, sy, -vp.left * sx, -vp.top * sy };
	}
	else
	{
		uniforms.mSceneDepthMap = { 0, 0, 0, 0 };
	}
}

float Weather_RainAround(HWDrawInfo *di)
{
	FLevelLocals *level = di->Level;
	if (level == nullptr) return 0.f;
	FWeatherSettings w = EffectiveWeather(level);
	if (w.Type != WEATHER_Rain || w.Density <= 0) return 0.f;
	if (exposure.level != level || exposure.width <= 0 || exposure.texels.Size() == 0 || exposure.cellSector.Size() == 0) return w.Density;

	// A point is in the rain if its cell is under open sky and no 3D floor
	// roof (a stop height above the cell's floor) covers the camera's height.
	const DVector3 &camera = di->Viewpoint.Pos;
	auto inRain = [&](double x, double y)
	{
		int cx = std::clamp(int((x - exposure.originX) / exposure.cellSize), 0, exposure.width - 1);
		int cy = std::clamp(int((y - exposure.originY) / exposure.cellSize), 0, exposure.height - 1);
		int cell = cy * exposure.width + cx;
		const uint16_t *texel = &exposure.texels[cell * 4];
		if (texel[3] == 0) return false;
		double cellX = exposure.originX + (cx + 0.5) * exposure.cellSize, cellY = exposure.originY + (cy + 0.5) * exposure.cellSize;
		double floorZ = level->sectors[exposure.cellSector[cell]].floorplane.ZatPoint(cellX, cellY);
		double stop = FromHalf(texel[0]);
		// The tolerance covers the half float rounding of the stop height.
		return stop <= floorZ + 1.0 + std::fabs(floorZ) / 1024.0 || stop <= camera.Z;
	};

	// 5x5 points 128 units apart around the camera. Under cover, only rain the
	// camera can see counts (through a door or window), not a courtyard behind
	// a wall: the BSP walk of this frame must have reached the point.
	auto &sections = level->sections;
	bool cameraOpen = inRain(camera.X, camera.Y);
	int open = cameraOpen;
	for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++)
	{
		if (i == 0 && j == 0) continue;
		double x = camera.X + i * 128.0, y = camera.Y + j * 128.0;
		if (!inRain(x, y)) continue;
		if (!cameraOpen)
		{
			auto sub = level->PointInRenderSubsector(DVector2(x, y));
			if (!sub || !sub->section || !(di->section_renderflags[sections.SectionIndex(sub->section)] & SSRF_PROCESSED)) continue;
		}
		open++;
	}
	return w.Density * open / 25.f;
}

void Weather_Flash(FLevelLocals *level, double strength, double duration)
{
	if (level == nullptr || duration <= 0) return;
	level->weatherFlashStart = float(level->maptime) / TICRATE;
	level->weatherFlashDuration = float(duration);
	level->weatherFlashStrength = float(strength);
}

void Weather_Draw(HWDrawInfo *di, FRenderState &state)
{
	FLevelLocals *level = di->Level;
	if (r_weather_quality <= 0 || level != primaryLevel || !exposure.bound || exposure.level != level) return;

	FWeatherSettings w = EffectiveWeather(level);
	if (w.Type == WEATHER_None || w.Density <= 0 || !CreateBuffers()) return;
	bool snow = w.Type == WEATHER_Snow;

	double angle = w.WindAngle * (M_PI / 180.0);
	float windX = float(std::cos(angle) * w.WindSpeed), windY = float(std::sin(angle) * w.WindSpeed);
	float fall = (snow ? 70.f : 1100.f) * std::max(w.FallSpeed, 0.05f);
	// Rain streaks are as long as a drop moves in about a frame of exposure.
	float speed = std::sqrt(fall * fall + windX * windX + windY * windY);
	float length = snow ? 2.4f * w.Length : std::clamp(speed * 0.028f * w.Length, 4.f, 256.f);
	float active = std::clamp(0.65f * w.Density, 0.f, 1.f);
	FVector3 tint = snow ? FVector3(0.95f, 0.97f, 1.f) : FVector3(0.7f, 0.76f, 0.85f);
	if (w.Color != 0)
	{
		PalEntry c = w.Color;
		tint = { c.r / 255.f, c.g / 255.f, c.b / 255.f };
	}
	float brightness = snow ? 0.9f : 0.4f;
	float soft = di->SceneDepthReady && r_softparticles ? 12.f : 0.f;

	sector_t *viewsector = di->Viewpoint.sector;
	state.SetVertexBuffer(screen->mWeatherVertices, 0, 0);
	state.SetIndexBuffer(screen->mWeatherIndices);
	state.SetMaterial(TexMan.GetGameTexture(TexMan.glPart), UF_None, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
	state.SetTextureMode(TM_NORMAL);
	if (viewsector) SetFog(state, level, di->lightmode, viewsector->lightlevel, 0, di->isFullbrightScene(), &viewsector->Colormap, false);
	state.SetLightIndex(FX_ParticleLightList(di));
	state.SetNoSoftLightLevel();
	state.ResetColor();
	state.EnableBrightmap(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetDepthMask(false);
	state.SetDepthFunc(DF_LEqual);
	state.SetRenderStyle(snow ? STYLE_Translucent : STYLE_Add);
	state.SetDynLight(tint.X, tint.Y, tint.Z);
	state.SetWeatherParams(WeatherTime(level, di->Viewpoint.TicFrac), snow);
	state.SetParticleExtra({ windX, windY, fall, length }, { active, brightness, 0.45f, soft });
	state.DrawIndexed(DT_Triangles, 0, (r_weather_quality >= 2 ? AllQuads : NearQuads) * 6);

	state.SetParticleExtra({ 0, 0, 0, 0 }, { 0, 0, 0, 0 });
	state.SetMaterialParams(0, 0);
	state.SetDynLight(0, 0, 0);
	state.SetLightIndex(-1);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetDepthMask(true);
	state.EnableFog(true);
	state.SetVertexBuffer(screen->mVertexData);
}
