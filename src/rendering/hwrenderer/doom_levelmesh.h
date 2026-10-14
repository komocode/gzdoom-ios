#pragma once

#include "hw_levelmesh.h"
#include "tarray.h"
#include "vectors.h"
#include "r_defs.h"

struct FLevelLocals;
struct F3DFloor;
class AActor;

struct Surface
{
	SurfaceType type;
	int typeIndex;	// side for walls, subsector for flats, -1 for models
	int numVerts;
	unsigned int startVertIndex;
	secplane_t plane;
	sector_t *controlSector = nullptr;
	sector_t *sector = nullptr;	// the sector whose light the surface receives
	FTextureID texture;	// base texture, for the albedo of the baked lighting
	bool bSky = false;	// not every surface type sets it
	bool bTwoSided = false;	// masked mid textures and models are seen from both sides
	bool bShadowCaster = true;	// part of LevelMesh::ShadowElements
	bool bLightOwner = false;	// a model whose actor carries lights: it must not shadow them
};

// The level geometry for ray traced shadows and the baked GI:
//
// - walls, flats and opaque 3D floors (water, glass, fog and non-solid 3D floors
//   are left out, so light passes them),
// - masked two-sided mid textures (bars, fences, 3DMIDTEX bridges) as the
//   rectangles of their opaque texels, so they cast striped shadows,
// - static model decorations (MODELDEF models and voxels) in their current frame.
//
// It is built from the current level state on first use, so savegames and hub
// re-entry see the restored sectors and polyobjects, and rebuilt (throttled)
// while ray traced shadows are active and sectors, polyobjects or static models
// move.
class DoomLevelMesh : public hwrenderer::LevelMesh
{
public:
	DoomLevelMesh(FLevelLocals &doomMap);

	TArray<Surface> Surfaces;

	// Brings level->levelMesh up to date. trackChanges rebuilds it when the
	// geometry moved since it was built, at most every 100 ms unless immediate.
	// Returns true when it was replaced.
	static bool Update(FLevelLocals *level, bool trackChanges, bool immediate = false);
	// Drops the mesh; the next Update builds it again from the current state.
	static void Invalidate(FLevelLocals *level);

	// Actors whose model geometry is part of the mesh.
	static bool IsStaticModelActor(AActor *actor);

private:
	void CreateSubsectorSurfaces(FLevelLocals &doomMap);
	void CreatePlaneSurface(subsector_t *sub, const secplane_t &plane, bool ceiling, FTextureID texture, sector_t *controlSector, bool sky);
	void CreateSideSurfaces(FLevelLocals &doomMap, side_t *side);
	void Create3DFloorSideSurfaces(side_t *side, sector_t *front, sector_t *back, const FVector2 &v1, const FVector2 &v2);
	void CreateMaskedMidSurface(side_t *side, sector_t *front, sector_t *back, const FVector2 &v1, const FVector2 &v2);
	void CreateModelSurfaces(FLevelLocals &doomMap);
	void AddWallQuad(Surface &surf, const FVector2 &v1, const FVector2 &v2, float z1Bottom, float z2Bottom, float z1Top, float z2Top);
	void FinishSurface(Surface &surf);

	static uint64_t GeometryKey(FLevelLocals &doomMap);
	static bool Is3DFloorOpaque(const F3DFloor *rover);
	static bool IsTopSideSky(sector_t* frontsector, sector_t* backsector, side_t* side);
	static bool IsBottomSideSky(sector_t* frontsector, sector_t* backsector, side_t* side);
	static bool IsTopSideVisible(side_t* side);
	static bool IsBottomSideVisible(side_t* side);
	static bool IsSkyPlane(sector_t* sector, int plane);

	uint64_t Key = 0;	// GeometryKey at build time
	uint64_t LastCheck = 0;

	static FVector2 ToFVector2(const DVector2& v) { return FVector2((float)v.X, (float)v.Y); }

	static bool IsDegenerate(const FVector3 &v0, const FVector3 &v1, const FVector3 &v2);
};
