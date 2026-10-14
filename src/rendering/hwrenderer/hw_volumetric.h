#pragma once

#include "vectors.h"
#include "tarray.h"

struct HWDrawInfo;
struct FDynamicLight;

// Lights and medium of the volumetric pass (r_volumetric).
//
// Every frame the main view picks the dynamic lights whose scattering is
// visible: lights touching an area the BSP walk rendered (so lights behind
// walls don't glow through them), faded in and out over a few frames, and
// faded with distance. The brightest ones up to r_volumetric_shadows get
// shadowed light shafts. The height fog comes from MAPINFO / ZScript, or the
// r_heightfog_* CVARs for testing.

struct VolumetricLight
{
	FVector3 Pos;			// GL world space (x, height, y)
	float Radius;
	FVector3 Color;			// faded colour, 0-1
	float Core;				// size of the inverse-square falloff core
	FVector3 SpotDir;		// beam direction, zero for point lights
	float SpotCosOuter;
	float SpotCosInner;
	int ShadowMapIndex;		// row in the 1D shadow maps, -1 for none
	bool Shadowed;			// marched with shadow rays / shadow map taps
	FDynamicLight *Light;
};

// Called by the main view between the BSP walk and the opaque pass.
void R_CollectVolumetrics(HWDrawInfo *di);

// The lights picked for the current frame, most important first (for other
// effects that want to light their own media the same way).
const TArray<VolumetricLight> &R_GetVolumetricLights();
