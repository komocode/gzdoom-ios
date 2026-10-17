#pragma once

struct FLevelLocals;
struct HWDrawInfo;
class FRenderState;
struct HWViewpointUniforms;

// Visual-only weather: rain and snow drawn by the GPU, plus a sky exposure map.
//
// Sky exposure map. Once per level the map is sampled from above on a grid of
// 16+ unit cells. Each cell stores the height where rain stops (the highest
// floor or 3D floor below a sky ceiling), the sky ceiling height, the sector
// light level and whether the cell is under open sky at all. Cells whose
// sectors move (doors, lifts, 3D floors) are re-evaluated a few times a second.
// The map is an RGBA16F texture bound to every scene shader:
//
//   WeatherMap (set 0, binding 4; Metal texture 15), read with texelFetch.
//   uWeatherMap = map origin x, y, 1 / cell size, cell count x (0: no map)
//   uWeatherParams = cell count y, lightning flash 0-1, time, MAPINFO wettexels
//   main.vp / main.fp: WeatherCell(mapPos), WeatherExposure(worldPos) returns 1
//   where a point is under open sky (rain falls on it, lightning lights it).
//
// Weather. A fixed set of drops in three nested layers around the camera
// (1024, 2560 and 6144 units wide) plus splash slots is drawn in one call.
// The vertex shader computes each drop from its index and the time: every
// layer is a world-space lattice that wraps around the camera, so the CPU does
// nothing per drop and drops stay put while the camera moves. Drops are killed
// where the exposure map says the sky is covered and end at the stop height,
// where closed-form splashes (ripple rings and droplet bursts) appear. Rain is
// a thin camera-facing ribbon stretched along its velocity, lit additively by
// the sector light, the baked GI, the strongest nearby dynamic lights and
// lightning; distant drops keep a minimum on-screen width and get fainter, so
// far layers read as sheets. Snow uses the same lattice with slow, wobbling
// flakes. Drawn before the translucent pass of the main view, so translucent
// water and glass composite over weather behind them (weather in front of a
// translucent surface is slightly dimmed by it); GPU particles are drawn after it.
//
// Lightning (DLightningThinker or LevelLocals.WeatherFlash) sets a brief flash
// that brightens weather, lit GPU particles and every surface under open sky.

void Weather_LevelLoaded(FLevelLocals *level);
void Weather_LevelUnloaded();

// Builds or updates the sky exposure map (main view, before drawing).
void Weather_Update(FLevelLocals *level);
void Weather_SetUniforms(HWDrawInfo *di, HWViewpointUniforms &uniforms);
void Weather_Draw(HWDrawInfo *di, FRenderState &state);

// Visual-only flash (no sector light change), e.g. for scripted lightning.
void Weather_Flash(FLevelLocals *level, double strength, double duration);

// Rain density (0-2, testing overrides included; 0 without rain) times the
// fraction of points within about 256 map units of the camera that are in the
// rain: under open sky in the exposure map, not under a 3D floor roof, and,
// while the camera itself is under cover, reached by this frame's BSP walk.
// For effects of the rain itself, such as light scattering in the volumetric pass.
float Weather_RainAround(HWDrawInfo *di);
