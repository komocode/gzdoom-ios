#pragma once

#include "vectors.h"

struct FLevelLocals;
struct HWViewpointUniforms;

// Baked irradiance volume ("probe GI").
//
// A regular grid of probes covers the level. Each probe stores an ambient cube:
// six colors holding the cosine-weighted light arriving from +X, -X, +Y, -Y,
// +Z and -Z. Probes are baked once per level on a background thread by tracing
// rays against the level mesh. A ray hit returns the surface albedo lit by its
// sector light level and the static dynamic lights in view of it, so the
// volume captures one bounce of indirect light and colored light bleeding.
//
// The scene shader uses the volume to raise the effective light level near
// brighter areas and to tint surfaces by their surroundings. Doom's own light
// levels remain the floor, so classic maps never get darker.

void GI_LevelLoaded(FLevelLocals *level);
void GI_LevelUnloaded();
void GI_Update(FLevelLocals *level);
void GI_SetUniforms(HWViewpointUniforms &uniforms);
