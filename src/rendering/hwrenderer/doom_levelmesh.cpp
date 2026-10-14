
#include "templates.h"
#include "doom_levelmesh.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "p_3dfloors.h"
#include "p_3dmidtex.h"
#include "po_man.h"
#include "actor.h"
#include "hw_irradiance.h"
#include "models.h"
#include "modelrenderer.h"
#include "bitmap.h"
#include "i_time.h"
#include "c_cvars.h"
#include "printf.h"

#include <unordered_map>
#include <vector>

// 0: no models, 1: actors flagged +STATICMESH, 2: also non-moving model decorations.
CUSTOM_CVAR(Int, r_staticmodels, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 2) self = 2;
	else if (primaryLevel && primaryLevel->levelMesh)
	{
		DoomLevelMesh::Invalidate(primaryLevel);
		GI_LevelLoaded(primaryLevel);
	}
}

namespace
{

// Opaque texel rectangles of a masked texture, in texture fractions (0..1).
struct MaskRect
{
	float x0, y0, x1, y1;
};

std::unordered_map<int, std::vector<MaskRect>> maskCache;

// Greedy rectangles over the alpha-tested texels: runs on each row, merged with
// an identical run on the row above. Bars, fences and grates come out as a
// handful of rectangles. Noisy masks are reduced in resolution until they fit.
const std::vector<MaskRect> &GetMaskRects(FTextureID id)
{
	auto found = maskCache.find(id.GetIndex());
	if (found != maskCache.end()) return found->second;
	std::vector<MaskRect> &rects = maskCache[id.GetIndex()];

	auto game = TexMan.GetGameTexture(id, false);
	if (!game || !game->isValid() || !game->GetTexture()) return rects;
	FBitmap bitmap = game->GetTexture()->GetBgraBitmap(nullptr);
	const uint8_t *pixels = bitmap.GetPixels();
	int w = bitmap.GetWidth(), h = bitmap.GetHeight();
	if (!pixels || w <= 0 || h <= 0) return rects;

	const size_t maxRects = 256;
	for (int shift = 0; shift < 5; shift++)
	{
		int cell = 1 << shift;
		int cw = (w + cell - 1) / cell, ch = (h + cell - 1) / cell;
		std::vector<uint8_t> opaque(size_t(cw) * ch);
		for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++)
		{
			int solid = 0, total = 0;
			for (int sy = y * cell; sy < std::min(h, (y + 1) * cell); sy++)
				for (int sx = x * cell; sx < std::min(w, (x + 1) * cell); sx++, total++)
					solid += pixels[sy * bitmap.GetPitch() + sx * 4 + 3] >= 128;
			opaque[y * cw + x] = solid * 2 >= total;
		}

		rects.clear();
		struct Run { int x0, x1, rect; };
		std::vector<Run> above, current;
		for (int y = 0; y < ch && rects.size() <= maxRects; y++)
		{
			current.clear();
			for (int x = 0; x < cw;)
			{
				if (!opaque[y * cw + x]) { x++; continue; }
				int x0 = x;
				while (x < cw && opaque[y * cw + x]) x++;
				int rect = -1;
				for (auto &r : above)
				{
					if (r.x0 == x0 && r.x1 == x) { rect = r.rect; break; }
				}
				if (rect >= 0) rects[rect].y1 = float(y + 1);
				else
				{
					rect = (int)rects.size();
					rects.push_back({ float(x0), float(y), float(x), float(y + 1) });
				}
				current.push_back({ x0, x, rect });
			}
			std::swap(above, current);
		}
		if (rects.size() <= maxRects)
		{
			for (auto &r : rects)
			{
				r.x0 = std::min(r.x0 * cell, float(w)) / w;
				r.x1 = std::min(r.x1 * cell, float(w)) / w;
				r.y0 = std::min(r.y0 * cell, float(h)) / h;
				r.y1 = std::min(r.y1 * cell, float(h)) / h;
			}
			return rects;
		}
	}
	rects.clear();
	return rects;
}

secplane_t ToPlane(const FVector3& pt1, const FVector3& pt2, const FVector3& pt3)
{
	FVector3 n = ((pt2 - pt1) ^ (pt3 - pt2)).Unit();
	float d = pt1 | n;
	secplane_t p;
	p.set(n.X, n.Y, n.Z, d);
	return p;
}

// CPU copy of a model's vertex buffer, owned by the model like the GPU ones.
class FLevelMeshModelBuffer : public IModelVertexBuffer
{
public:
	TArray<FModelVertex> Vertices;
	TArray<unsigned int> Indices;

	FModelVertex *LockVertexBuffer(unsigned int size) override { Vertices.Resize(size); return Vertices.Data(); }
	void UnlockVertexBuffer() override { }
	unsigned int *LockIndexBuffer(unsigned int size) override { Indices.Resize(size); return Indices.Data(); }
	void UnlockIndexBuffer() override { }
};

// "Draws" a model into world space triangles. Interpolation is ignored: the
// current frame is used. Skeletal models contribute their bind pose.
class FLevelMeshModelRenderer : public FModelRenderer
{
public:
	struct Batch
	{
		FTextureID skin;
		TArray<FVector3> triangles;
	};
	TArray<Batch> Batches;
	unsigned TriangleBudget = 0;

	ModelRendererType GetType() const override { return LevelMeshModelRendererType; }
	void BeginDrawModel(FRenderStyle style, int smf_flags, const VSMatrix &objectToWorldMatrix, bool mirrored) override { Matrix = objectToWorldMatrix; }
	void EndDrawModel(FRenderStyle style, int smf_flags) override { }
	IModelVertexBuffer *CreateVertexBuffer(bool needindex, bool singleframe) override { return new FLevelMeshModelBuffer; }
	VSMatrix GetViewToWorldMatrix() override { VSMatrix m; m.loadIdentity(); return m; }
	void BeginDrawHUDModel(FRenderStyle style, const VSMatrix &objectToWorldMatrix, bool mirrored, int smf_flags) override { }
	void EndDrawHUDModel(FRenderStyle style, int smf_flags) override { }
	void SetInterpolation(double interpolation) override { }
	void SetMaterial(FGameTexture *skin, bool clampNoFilter, FTranslationID translation) override { Skin = skin ? skin->GetID() : FTextureID(); }
	void SetupFrame(FModel *model, unsigned int frame1, unsigned int frame2, unsigned int size, int boneStartIndex) override
	{
		Buffer = static_cast<FLevelMeshModelBuffer*>(model->GetVertexBuffer(GetType()));
		Base = frame1;
	}
	void DrawArrays(int start, int count) override
	{
		Batch *batch = BeginBatch(count);
		if (!batch) return;
		for (int i = 0; i < count; i++) Emit(batch, Base + start + i);
	}
	void DrawElements(int numIndices, size_t offset) override
	{
		Batch *batch = BeginBatch(numIndices);
		if (!batch) return;
		size_t first = offset / sizeof(unsigned int);
		for (int i = 0; i < numIndices; i++)
		{
			size_t index = first + i;
			Emit(batch, index < Buffer->Indices.Size() ? Base + Buffer->Indices[index] : ~0u);
		}
	}

private:
	VSMatrix Matrix;
	FTextureID Skin;
	FLevelMeshModelBuffer *Buffer = nullptr;
	unsigned Base = 0;

	Batch *BeginBatch(int count)
	{
		if (!Buffer || count < 3 || unsigned(count / 3) > TriangleBudget) return nullptr;
		TriangleBudget -= count / 3;
		Batches.Reserve(1);
		Batch *batch = &Batches.Last();
		batch->skin = Skin;
		batch->triangles.Clear();
		return batch;
	}

	void Emit(Batch *batch, unsigned index)
	{
		FVector3 p(0, 0, 0);
		if (index < Buffer->Vertices.Size())
		{
			const FModelVertex &v = Buffer->Vertices[index];
			FLOATTYPE in[4] = { v.x, v.y, v.z, 1 }, out[4];
			Matrix.multMatrixPoint(in, out);
			// Model matrices work in GL's axis order (x, height, y).
			p = FVector3((float)out[0], (float)out[2], (float)out[1]);
		}
		batch->triangles.Push(p);
	}
};

const unsigned MaxModelTriangles = 1000000;
const unsigned MaxActorTriangles = 100000;

} // namespace

DoomLevelMesh::DoomLevelMesh(FLevelLocals &doomMap)
{
	for (unsigned int i = 0; i < doomMap.sides.Size(); i++)
	{
		CreateSideSurfaces(doomMap, &doomMap.sides[i]);
	}

	CreateSubsectorSurfaces(doomMap);
	CreateModelSurfaces(doomMap);

	for (unsigned i = 0; i + 2 < MeshElements.Size(); i += 3)
	{
		if (Surfaces[MeshSurfaces[i / 3]].bShadowCaster)
		{
			ShadowElements.Push(MeshElements[i]);
			ShadowElements.Push(MeshElements[i + 1]);
			ShadowElements.Push(MeshElements[i + 2]);
		}
	}
	Key = GeometryKey(doomMap);
	LastCheck = I_msTime();
}

void DoomLevelMesh::FinishSurface(Surface &surf)
{
	int index = (int)Surfaces.Size();
	unsigned pos = surf.startVertIndex;
	FVector3 *verts = &MeshVertices[pos];
	for (int j = 0; j < surf.numVerts; j++)
		MeshUVIndex.Push(j);

	auto addTriangle = [&](unsigned a, unsigned b, unsigned c)
	{
		if (IsDegenerate(MeshVertices[a], MeshVertices[b], MeshVertices[c])) return;
		MeshElements.Push(a);
		MeshElements.Push(b);
		MeshElements.Push(c);
		MeshSurfaces.Push(index);
	};

	if (surf.type == ST_FLOOR || surf.type == ST_CEILING)
	{
		for (int j = 2; j < surf.numVerts; j++)
			addTriangle(pos, pos + j - 1, pos + j);
	}
	else if (surf.type == ST_MODEL)
	{
		for (int j = 0; j + 2 < surf.numVerts; j += 3)
			addTriangle(pos + j, pos + j + 1, pos + j + 2);
	}
	else
	{
		// Walls are lists of quads: bottom left, bottom right, top left, top right.
		for (int j = 0; j + 3 < surf.numVerts; j += 4)
		{
			addTriangle(pos + j + 0, pos + j + 1, pos + j + 2);
			addTriangle(pos + j + 3, pos + j + 2, pos + j + 1);
		}
		if (surf.numVerts >= 3)
			surf.plane = ToPlane(verts[0], verts[1], verts[2]);
	}
	Surfaces.Push(surf);
}

void DoomLevelMesh::AddWallQuad(Surface &surf, const FVector2 &v1, const FVector2 &v2, float z1Bottom, float z2Bottom, float z1Top, float z2Top)
{
	MeshVertices.Push(FVector3(v1.X, v1.Y, z1Bottom));
	MeshVertices.Push(FVector3(v2.X, v2.Y, z2Bottom));
	MeshVertices.Push(FVector3(v1.X, v1.Y, z1Top));
	MeshVertices.Push(FVector3(v2.X, v2.Y, z2Top));
	surf.numVerts += 4;
}

void DoomLevelMesh::CreateSideSurfaces(FLevelLocals &doomMap, side_t *side)
{
	sector_t *front;
	sector_t *back;

	front = side->sector;
	back = (side->linedef->frontsector == front) ? side->linedef->backsector : side->linedef->frontsector;

	FVector2 v1 = ToFVector2(side->V1()->fPos());
	FVector2 v2 = ToFVector2(side->V2()->fPos());

	float v1Top = (float)front->ceilingplane.ZatPoint(v1);
	float v1Bottom = (float)front->floorplane.ZatPoint(v1);
	float v2Top = (float)front->ceilingplane.ZatPoint(v2);
	float v2Bottom = (float)front->floorplane.ZatPoint(v2);

	int typeIndex = side->Index();

	auto newSurface = [&](SurfaceType type)
	{
		Surface surf;
		surf.type = type;
		surf.typeIndex = typeIndex;
		surf.sector = front;
		surf.startVertIndex = MeshVertices.Size();
		surf.numVerts = 0;
		return surf;
	};

	if (back)
	{
		Create3DFloorSideSurfaces(side, front, back, v1, v2);
		CreateMaskedMidSurface(side, front, back, v1, v2);

		float v1TopBack = (float)back->ceilingplane.ZatPoint(v1);
		float v1BottomBack = (float)back->floorplane.ZatPoint(v1);
		float v2TopBack = (float)back->ceilingplane.ZatPoint(v2);
		float v2BottomBack = (float)back->floorplane.ZatPoint(v2);

		if (v1Top == v1TopBack && v1Bottom == v1BottomBack && v2Top == v2TopBack && v2Bottom == v2BottomBack)
		{
			return;
		}

		// bottom seg
		if (v1Bottom < v1BottomBack || v2Bottom < v2BottomBack)
		{
			bool bSky = IsBottomSideSky(front, back, side);
			if (bSky || IsBottomSideVisible(side))
			{
				Surface surf = newSurface(ST_LOWERWALL);
				surf.texture = side->GetTexture(side_t::bottom);
				surf.bSky = bSky;
				surf.bShadowCaster = !bSky;
				AddWallQuad(surf, v1, v2, v1Bottom, v2Bottom, v1BottomBack, v2BottomBack);
				FinishSurface(surf);
			}

			v1Bottom = v1BottomBack;
			v2Bottom = v2BottomBack;
		}

		// top seg
		if (v1Top > v1TopBack || v2Top > v2TopBack)
		{
			bool bSky = IsTopSideSky(front, back, side);
			if (bSky || IsTopSideVisible(side))
			{
				Surface surf = newSurface(ST_UPPERWALL);
				surf.texture = side->GetTexture(side_t::top);
				surf.bSky = bSky;
				surf.bShadowCaster = !bSky;
				AddWallQuad(surf, v1, v2, v1TopBack, v2TopBack, v1Top, v2Top);
				FinishSurface(surf);
			}

			v1Top = v1TopBack;
			v2Top = v2TopBack;
		}
	}

	// middle seg
	if (back == nullptr)
	{
		Surface surf = newSurface(ST_MIDDLEWALL);
		surf.texture = side->GetTexture(side_t::mid);
		AddWallQuad(surf, v1, v2, v1Bottom, v2Bottom, v1Top, v2Top);
		FinishSurface(surf);
	}
}

// The sides of 3D floors that sit in the back sector, as seen from the front
// sector (as the renderer draws them), so their normals face the viewer side.
void DoomLevelMesh::Create3DFloorSideSurfaces(side_t *side, sector_t *front, sector_t *back, const FVector2 &v1, const FVector2 &v2)
{
	auto &frontFloors = front->e->XFloor.ffloors;
	for (F3DFloor *rover : back->e->XFloor.ffloors)
	{
		if (rover->flags & FF_THISINSIDE) continue;
		if ((rover->flags & (FF_EXISTS | FF_RENDERSIDES)) != (FF_EXISTS | FF_RENDERSIDES) || (rover->flags & FF_INVERTSIDES)) continue;
		if (!Is3DFloorOpaque(rover)) continue;

		// Don't create a wall when both sectors have the same 3D floor.
		bool bothSides = false;
		for (F3DFloor *other : frontFloors)
		{
			if (other->model == rover->model) { bothSides = true; break; }
		}
		if (bothSides) continue;

		// Only the part inside the opening between both sectors is visible.
		float openBottom1 = (float)std::max(front->floorplane.ZatPoint(v1), back->floorplane.ZatPoint(v1));
		float openBottom2 = (float)std::max(front->floorplane.ZatPoint(v2), back->floorplane.ZatPoint(v2));
		float openTop1 = (float)std::min(front->ceilingplane.ZatPoint(v1), back->ceilingplane.ZatPoint(v1));
		float openTop2 = (float)std::min(front->ceilingplane.ZatPoint(v2), back->ceilingplane.ZatPoint(v2));
		float top1 = std::min((float)rover->top.plane->ZatPoint(v1), openTop1);
		float top2 = std::min((float)rover->top.plane->ZatPoint(v2), openTop2);
		float bottom1 = std::max((float)rover->bottom.plane->ZatPoint(v1), openBottom1);
		float bottom2 = std::max((float)rover->bottom.plane->ZatPoint(v2), openBottom2);
		if (top1 <= bottom1 && top2 <= bottom2) continue;

		Surface surf;
		surf.type = ST_MIDDLEWALL;
		surf.typeIndex = side->Index();
		surf.sector = front;
		surf.controlSector = rover->model;
		if (rover->flags & FF_UPPERTEXTURE) surf.texture = side->GetTexture(side_t::top);
		else if (rover->flags & FF_LOWERTEXTURE) surf.texture = side->GetTexture(side_t::bottom);
		else if (rover->master && rover->master->sidedef[0]) surf.texture = rover->master->sidedef[0]->GetTexture(side_t::mid);
		surf.startVertIndex = MeshVertices.Size();
		surf.numVerts = 0;
		AddWallQuad(surf, v1, v2, bottom1, bottom2, std::max(top1, bottom1), std::max(top2, bottom2));
		FinishSurface(surf);
	}
}

// Two-sided mid textures with transparent texels (bars, fences, grates, railings,
// 3DMIDTEX bridges). Only the opaque texel rectangles are added, which gives exact
// alpha-tested occlusion to both the GI bake and the opaque ray tracing geometry
// without any per-hit texture lookups in the shaders.
void DoomLevelMesh::CreateMaskedMidSurface(side_t *side, sector_t *front, sector_t *back, const FVector2 &v1, const FVector2 &v2)
{
	line_t *line = side->linedef;
	FTextureID id = side->GetTexture(side_t::mid);
	if (!id.isValid() || line->alpha < 1. || (line->flags & ML_ADDTRANS)) return;
	auto tex = TexMan.GetGameTexture(id, true);
	if (!tex || !tex->isValid()) return;
	int sideno = line->sidedef[0] == side ? 0 : 1;
	double texTop, texBottom;
	if (!P_GetMidTexturePosition(line, sideno, &texTop, &texBottom) || texTop <= texBottom) return;
	const std::vector<MaskRect> &rects = GetMaskRects(id);
	if (rects.empty()) return;

	float length = (v2 - v1).Length();
	if (length <= 0.f) return;
	FTexCoordInfo tci;
	tci.GetFromTexture(tex, (float)side->GetTextureXScale(side_t::mid), (float)side->GetTextureYScale(side_t::mid), !!(line->GetLevel()->flags3 & LEVEL3_FORCEWORLDPANNING));
	float u0 = tci.FloatToTexU(tci.TextureOffset((float)side->GetTextureXOffset(side_t::mid)));
	float uLength = tci.FloatToTexU(side->TexelLength);
	if (uLength == 0.f) return;

	float openBottom = (float)std::max(std::max(front->floorplane.ZatPoint(v1), back->floorplane.ZatPoint(v1)), std::max(front->floorplane.ZatPoint(v2), back->floorplane.ZatPoint(v2)));
	float openTop = (float)std::min(std::min(front->ceilingplane.ZatPoint(v1), back->ceilingplane.ZatPoint(v1)), std::min(front->ceilingplane.ZatPoint(v2), back->ceilingplane.ZatPoint(v2)));
	float height = float(texTop - texBottom);
	bool wrap = (line->flags & ML_WRAP_MIDTEX) || (side->Flags & WALLF_WRAP_MIDTEX);
	int firstRow = 0, lastRow = 0;
	if (wrap)
	{
		firstRow = (int)std::floor((texTop - openTop) / height);
		lastRow = (int)std::ceil((texTop - openBottom) / height) - 1;
	}
	float uMin = std::min(u0, u0 + uLength), uMax = std::max(u0, u0 + uLength);
	int firstColumn = (int)std::floor(uMin), lastColumn = (int)std::ceil(uMax) - 1;

	Surface surf;
	surf.type = ST_MIDDLEWALL;
	surf.typeIndex = side->Index();
	surf.sector = front;
	surf.texture = id;
	surf.bTwoSided = true;
	surf.startVertIndex = MeshVertices.Size();
	surf.numVerts = 0;
	FVector2 dir = (v2 - v1) / length;
	const int maxQuads = 4096;
	for (int row = firstRow; row <= lastRow; row++)
	{
		for (int column = firstColumn; column <= lastColumn; column++)
		{
			for (const MaskRect &r : rects)
			{
				if (surf.numVerts >= maxQuads * 4) break;
				float s0 = ((column + r.x0) - u0) / uLength * length;
				float s1 = ((column + r.x1) - u0) / uLength * length;
				if (s0 > s1) std::swap(s0, s1);
				s0 = std::max(s0, 0.f);
				s1 = std::min(s1, length);
				float zTop = std::min(float(texTop - (row + r.y0) * height), openTop);
				float zBottom = std::max(float(texTop - (row + r.y1) * height), openBottom);
				if (s1 - s0 < 0.01f || zTop - zBottom < 0.01f) continue;
				AddWallQuad(surf, v1 + dir * s0, v1 + dir * s1, zBottom, zBottom, zTop, zTop);
			}
		}
	}
	if (surf.numVerts > 0)
		FinishSurface(surf);
}

void DoomLevelMesh::CreatePlaneSurface(subsector_t *sub, const secplane_t &plane, bool ceiling, FTextureID texture, sector_t *controlSector, bool sky)
{
	Surface surf;
	surf.type = ceiling ? ST_CEILING : ST_FLOOR;
	surf.typeIndex = sub->Index();
	surf.sector = sub->sector;
	surf.controlSector = controlSector;
	surf.texture = texture;
	surf.bSky = sky;
	surf.bShadowCaster = !sky;
	surf.plane = plane;
	if (controlSector) surf.plane.FlipVert();

	surf.numVerts = sub->numlines;
	surf.startVertIndex = MeshVertices.Size();
	MeshVertices.Resize(surf.startVertIndex + surf.numVerts);
	FVector3* verts = &MeshVertices[surf.startVertIndex];

	for (int j = 0; j < surf.numVerts; j++)
	{
		// Floors wind the other way round so both face into the sector.
		seg_t *seg = &sub->firstline[ceiling ? j : (surf.numVerts - 1) - j];
		FVector2 v1 = ToFVector2(seg->v1->fPos());

		verts[j].X = v1.X;
		verts[j].Y = v1.Y;
		verts[j].Z = (float)plane.ZatPoint(v1);
	}

	FinishSurface(surf);
}

void DoomLevelMesh::CreateSubsectorSurfaces(FLevelLocals &doomMap)
{
	for (unsigned int i = 0; i < doomMap.subsectors.Size(); i++)
	{
		subsector_t *sub = &doomMap.subsectors[i];

		if (sub->numlines < 3)
		{
			continue;
		}

		sector_t *sector = sub->sector;
		if (!sector)
			continue;

		CreatePlaneSurface(sub, sector->floorplane, false, sector->GetTexture(sector_t::floor), nullptr, IsSkyPlane(sector, sector_t::floor));
		CreatePlaneSurface(sub, sector->ceilingplane, true, sector->GetTexture(sector_t::ceiling), nullptr, IsSkyPlane(sector, sector_t::ceiling));

		// The top of a 3D floor faces up like a floor, its bottom down like a
		// ceiling. top/bottom already account for Vavoom-style and inverted types.
		for (F3DFloor *rover : sector->e->XFloor.ffloors)
		{
			if ((rover->flags & (FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) != (FF_EXISTS | FF_RENDERPLANES)) continue;
			if (!Is3DFloorOpaque(rover)) continue;
			if (!rover->top.copied)
				CreatePlaneSurface(sub, *rover->top.plane, false, *rover->top.texture, rover->model, false);
			if (!rover->bottom.copied)
				CreatePlaneSurface(sub, *rover->bottom.plane, true, *rover->bottom.texture, rover->model, false);
		}
	}
}

bool DoomLevelMesh::IsStaticModelActor(AActor *actor)
{
	if (r_staticmodels <= 0 || (actor->renderflags2 & RF2_NOSTATICMESH)) return false;
	if (actor->renderflags & RF_INVISIBLE || (actor->flags & MF_NOSECTOR) || !(actor->RenderStyle == DefaultRenderStyle()) || actor->Alpha < 1.) return false;
	FSpriteModelFrame *smf = FindModelFrame(actor, actor->sprite, actor->frame, !!(actor->flags & MF_DROPPED));
	if (!smf) return false;
	if (actor->renderflags2 & RF2_STATICMESH) return true;
	if (r_staticmodels < 2) return false;

	// Automatic: decorations that don't move, aren't monsters, players, pickups or
	// projectiles, and whose model isn't animated by its own rotation.
	if (actor->player || (actor->flags & (MF_SHOOTABLE | MF_COUNTKILL | MF_MISSILE | MF_SPECIAL))) return false;
	if ((actor->flags2 & MF2_FLOATBOB) || (actor->flags9 & MF9_DECOUPLEDANIMATIONS) || !actor->Vel.isZero()) return false;
	if (smf->getFlags(actor->modelData) & (MDL_ROTATING | MDL_PITCHFROMMOMENTUM)) return false;
	return true;
}

void DoomLevelMesh::CreateModelSurfaces(FLevelLocals &doomMap)
{
	if (r_staticmodels <= 0) return;

	FLevelMeshModelRenderer renderer;
	renderer.TriangleBudget = MaxModelTriangles;
	auto it = doomMap.GetThinkerIterator<AActor>();
	while (AActor *actor = it.Next())
	{
		if (!IsStaticModelActor(actor)) continue;
		FSpriteModelFrame *smf = FindModelFrame(actor, actor->sprite, actor->frame, !!(actor->flags & MF_DROPPED));
		// Same placement as the sprite renderer, without interpolation or bobbing.
		DVector3 pos = actor->Pos() + actor->WorldOffset;
		if ((actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) pos.Z -= actor->Floorclip;

		unsigned budget = renderer.TriangleBudget;
		renderer.TriangleBudget = std::min(budget, MaxActorTriangles);
		renderer.Batches.Clear();
		RenderModel(&renderer, (float)pos.X, (float)pos.Y, (float)pos.Z, smf, actor, 0.);
		renderer.TriangleBudget = budget - (std::min(budget, MaxActorTriangles) - renderer.TriangleBudget);

		// Lights carried by a model (lamps, torches) sit inside it, so its
		// geometry must not shadow them.
		bool lightOwner = actor->AttachedLights.Size() > 0;
		for (auto &batch : renderer.Batches)
		{
			Surface surf;
			surf.type = ST_MODEL;
			surf.typeIndex = -1;
			surf.sector = actor->Sector;
			surf.texture = batch.skin;
			surf.bTwoSided = true;
			surf.bLightOwner = lightOwner;
			surf.bShadowCaster = !lightOwner;
			surf.startVertIndex = MeshVertices.Size();
			surf.numVerts = batch.triangles.Size();
			MeshVertices.Append(batch.triangles);
			FinishSurface(surf);
		}
	}
}

// Everything the mesh depends on that changes during play: sector planes,
// 3D floor states, polyobjects and static model placement.
uint64_t DoomLevelMesh::GeometryKey(FLevelLocals &doomMap)
{
	uint64_t hash = 14695981039346656037ull;
	auto add = [&](double value)
	{
		uint64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		hash = (hash ^ bits) * 1099511628211ull;
	};
	for (auto &sector : doomMap.sectors)
	{
		add(sector.floorplane.fD());
		add(sector.floorplane.Normal().Z);
		add(sector.ceilingplane.fD());
		add(sector.ceilingplane.Normal().Z);
		for (F3DFloor *rover : sector.e->XFloor.ffloors)
		{
			add(rover->flags);
			add(rover->alpha);
		}
	}
	for (auto &poly : doomMap.Polyobjects)
	{
		add(poly.CenterSpot.pos.X);
		add(poly.CenterSpot.pos.Y);
		add(poly.Angle.Degrees());
	}
	if (r_staticmodels > 0)
	{
		auto it = doomMap.GetThinkerIterator<AActor>();
		while (AActor *actor = it.Next())
		{
			if (!IsStaticModelActor(actor)) continue;
			add(actor->X());
			add(actor->Y());
			add(actor->Z());
			add(actor->Angles.Yaw.Degrees());
			add(actor->AttachedLights.Size());
		}
	}
	return hash;
}

bool DoomLevelMesh::Update(FLevelLocals *level, bool trackChanges, bool immediate)
{
	DoomLevelMesh *mesh = level->levelMesh;
	if (mesh)
	{
		// Rebuilding is throttled to a few times a second while things move.
		if (!trackChanges) return false;
		uint64_t now = I_msTime();
		if (!immediate && now - mesh->LastCheck < 100) return false;
		mesh->LastCheck = now;
		if (GeometryKey(*level) == mesh->Key) return false;
	}

	uint64_t start = I_msTime();
	delete level->levelMesh;
	level->levelMesh = new DoomLevelMesh(*level);
	if (!mesh)
	{
		DPrintf(DMSG_NOTIFY, "Level mesh: %u triangles (%u shadow casting) in %u ms\n",
			level->levelMesh->MeshElements.Size() / 3, level->levelMesh->ShadowElements.Size() / 3, unsigned(I_msTime() - start));
	}
	return true;
}

void DoomLevelMesh::Invalidate(FLevelLocals *level)
{
	delete level->levelMesh;
	level->levelMesh = nullptr;
	maskCache.clear();
}

bool DoomLevelMesh::Is3DFloorOpaque(const F3DFloor *rover)
{
	// Water and other non-solid, see-through or fog 3D floors let light through.
	if (!(rover->flags & FF_EXISTS) || !(rover->flags & FF_SOLID)) return false;
	if (rover->flags & (FF_SWIMMABLE | FF_TRANSLUCENT | FF_ADDITIVETRANS | FF_FOG)) return false;
	return rover->alpha >= 255;
}

bool DoomLevelMesh::IsTopSideSky(sector_t* frontsector, sector_t* backsector, side_t* side)
{
	return IsSkyPlane(frontsector, sector_t::ceiling) && IsSkyPlane(backsector, sector_t::ceiling);
}

bool DoomLevelMesh::IsBottomSideSky(sector_t* frontsector, sector_t* backsector, side_t* side)
{
	return IsSkyPlane(frontsector, sector_t::floor) && IsSkyPlane(backsector, sector_t::floor);
}

bool DoomLevelMesh::IsTopSideVisible(side_t* side)
{
	auto tex = TexMan.GetGameTexture(side->GetTexture(side_t::top), true);
	return tex && tex->isValid();
}

bool DoomLevelMesh::IsBottomSideVisible(side_t* side)
{
	auto tex = TexMan.GetGameTexture(side->GetTexture(side_t::bottom), true);
	return tex && tex->isValid();
}

// Sky flats (sky ceilings, and abyss or sea floors below cliffs) and skybox planes.
bool DoomLevelMesh::IsSkyPlane(sector_t* sector, int plane)
{
	if (sector->GetTexture(plane) == skyflatnum) return true;
	FSectorPortal *portal = sector->GetPortal(plane);
	return portal && portal->mType == PORTS_SKYVIEWPOINT && !(portal->mFlags & PORTSF_SKYFLATONLY) && portal->mSkybox != nullptr;
}

bool DoomLevelMesh::IsDegenerate(const FVector3 &v0, const FVector3 &v1, const FVector3 &v2)
{
	// A degenerate triangle has a zero cross product for two of its sides.
	float ax = v1.X - v0.X;
	float ay = v1.Y - v0.Y;
	float az = v1.Z - v0.Z;
	float bx = v2.X - v0.X;
	float by = v2.Y - v0.Y;
	float bz = v2.Z - v0.Z;
	float crossx = ay * bz - az * by;
	float crossy = az * bx - ax * bz;
	float crossz = ax * by - ay * bx;
	float crosslengthsqr = crossx * crossx + crossy * crossy + crossz * crossz;
	return crosslengthsqr <= 1.e-6f;
}
