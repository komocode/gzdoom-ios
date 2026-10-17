#pragma once

#include "vectors.h"
#include "matrix.h"

struct FLevelLocals;
struct HWViewpointUniforms;

// Directional sun light with a baked shadow map (MAPINFO sun, sunbounce and
// sunspecular; LevelLocals.SetSun and SetSunColor).
//
// After a map has loaded, worker threads render the level into an orthographic
// depth map along the sun direction (like the GI bake): walls, ceilings, opaque
// 3D floors, the opaque texel rectangles of masked mid textures and the static
// models of the level mesh. Ceilings cast as if a few units thick, so that sun
// grazing a wall top can't slip between the ceiling and the wall below it.
// Sector floors, sky surfaces, horizon and portal lines and sectors outside the
// playable map (3D floor control sectors, skybox areas) cast no shadows. The map
// covers the playable geometry with square texels; r_sun_shadowsize is its
// larger dimension. There is no sun until the bake is done; if the level has
// nothing to bake the sun shines unshadowed.
//
// The scene shaders add the sun in linear space after the light clamps (like
// lightning): albedo * colour * N.L * shadow plus a GGX highlight that follows
// the material roughness, the sector wetness and puddles. Sector light levels
// don't scale it; fog does. Sprites take it at their position with a mild wrap
// toward the camera, the HUD weapon takes it at the eye. The GI bake bounces it
// (sunbounce). Sky, additive, fullbright and fuzzy draws don't take it.
//
// The sun shading is compiled only into sun variants of the scene shaders
// (SUN_LIGHT in main.fp). Viewpoints with a sun draw with those (FRenderState::
// SetSunShading); all other drawing, and every map without a sun, keeps the
// plain shaders. The variants of the common shaders, and of the user shaders on
// the level's walls and flats, compile when a sunlit level loads (Metal builds
// them in parallel); others compile when first drawn. The GL backends have no sun.
//
// SetSunColor only changes uniforms. A new direction (SetSun) or shadow map size
// re-bakes in the background; the old map and direction stay until it is done.
// The GI bounce keeps the sun of its own bake. Doors, lifts and moving models
// don't update the map until r_sun_rebake.

void Sun_LevelLoaded(FLevelLocals *level);
void Sun_LevelUnloaded();
void Sun_Update(FLevelLocals *level);
void Sun_SetUniforms(FLevelLocals *level, HWViewpointUniforms &uniforms);

// The HUD is drawn in view space: rotates the sun into it and replaces the
// shadow map lookup by the sun's visibility at the eye.
void Sun_SetHUDUniforms(HWViewpointUniforms &uniforms, const VSMatrix &viewMatrix, const DVector3 &eye);
