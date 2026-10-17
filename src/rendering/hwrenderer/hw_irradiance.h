#pragma once

#include "vectors.h"
#include "textureid.h"

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
//
// Sprite reflections share the snapshot/BVH, but trace a separate bounded grid
// of at most 128 radiance probes. Each probe contains four roughness-filtered
// 16x16 octahedral maps. Reflection hits sample captured texture pixels using
// the static surface transform, including brightmap/fullbright emission.
// Models without captured UVs and textures beyond the 16 MiB snapshot budget
// use the material-average radiance; the diffuse GI shading remains unchanged.
//
// GPU layout: diffuse layers remain face * sizeZ + z. Reflection layers follow
// them, packed in 36x36 tiles (four 18x18 maps with a one-texel seam border).
// Array dimensions can be padded, so diffuse sampling uses textureSize for UV
// normalization. HWViewpointUniforms::mReflection* describes probe coordinates
// and atlas packing. The whole array is bounded to 256 layers.
//
// This is a static-room reflection source: moving actors, mirrors, translucent
// surfaces, animated texture changes and moving doors are not captured live.
// Skewed/sloped wall texture transforms are approximate. r_gi_rebake refreshes
// the shared snapshot. Baking is enabled by either diffuse GI or sprite
// reflections, and never touches texture resources from worker threads.

void GI_LevelLoaded(FLevelLocals *level);
void GI_LevelUnloaded();
void GI_Update(FLevelLocals *level);
void GI_SetUniforms(HWViewpointUniforms &uniforms);

// Average colour of a texture (cached, main thread only).
FVector3 GI_TextureAlbedo(FTextureID id);
