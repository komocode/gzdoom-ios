/*
** hw_sun.cpp
** Directional sun light with a baked shadow map. See hw_sun.h.
*/

#include "hw_sun.h"
#include "hw_irradiance.h"
#include "hw_viewpointuniforms.h"
#include "doom_levelmesh.h"
#include "g_levellocals.h"
#include "p_lnspec.h"
#include "v_video.h"
#include "printf.h"
#include "i_time.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "texturemanager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#endif

CVARD(Bool, r_sun, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Draws the sun of maps that have one (MAPINFO sun)")
CUSTOM_CVARD(Int, r_sun_shadowsize, 4096, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Larger dimension of the sun's shadow map in texels (512-8192)")
{
	if (self < 512) self = 512;
	else if (self > 8192) self = 8192;
}
CUSTOM_CVARD(Float, r_sun_weapon, 0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "How much sunlight the HUD weapon takes where the player stands in the sun (0-1)")
{
	if (self < 0.f) self = 0.f;
	else if (self > 1.f) self = 1.f;
}

namespace
{

// A triangle corner in shadow map space: texel x, texel y, depth along the sunlight.
struct SunVertex
{
	double x, y;
	float d;
};

struct SunBake
{
	static constexpr int BandRows = 16;
	static constexpr int MaxSize = 8192;

	// Light space, in Doom axes: D runs along the sunlight, U across it
	// horizontally and V completes the frame. Texel (0, 0) starts at originU/V.
	FVector3 axisU, axisV, axisD;
	double originU = 0, originV = 0, originD = 0;
	double texel = 1.0;
	int width = 0, height = 0;
	float yaw = 0.f, elevation = 0.f;
	int size = 0;

	std::vector<SunVertex> verts;	// three per triangle
	std::vector<uint32_t> binStart, binTris;
	std::vector<float> depth;

	std::atomic<int> nextBand{ 0 };
	std::atomic<bool> cancel{ false };
	std::atomic<bool> done{ false };
	std::vector<std::thread> workers;
	std::thread coordinator;
	uint64_t startTime = 0;
	uint64_t setupTime = 0;		// main thread: collecting and projecting the casters
	uint64_t rasterTime = 0;	// workers: binning and rasterising
	unsigned triangles = 0;
	unsigned threads = 1;

	~SunBake()
	{
		cancel = true;
		if (coordinator.joinable()) coordinator.join();
	}

	int Bands() const { return (height + BandRows - 1) / BandRows; }

	// Every triangle goes to the bands of rows it may touch, so the workers
	// own disjoint rows and need no synchronisation.
	void Bin()
	{
		int bands = Bands();
		size_t count = verts.size() / 3;
		std::vector<uint32_t> first(count), last(count);
		binStart.assign(bands + 1, 0);
		for (size_t i = 0; i < count; i++)
		{
			const SunVertex *v = &verts[i * 3];
			double minY = std::min(v[0].y, std::min(v[1].y, v[2].y)), maxY = std::max(v[0].y, std::max(v[1].y, v[2].y));
			int b0 = std::clamp(int(std::floor(minY - 1.0)) / BandRows, 0, bands - 1);
			int b1 = std::clamp(int(std::floor(maxY + 1.0)) / BandRows, 0, bands - 1);
			first[i] = b0;
			last[i] = b1;
			for (int b = b0; b <= b1; b++) binStart[b + 1]++;
		}
		for (int b = 0; b < bands; b++) binStart[b + 1] += binStart[b];
		binTris.resize(binStart[bands]);
		std::vector<uint32_t> fill(binStart.begin(), binStart.end() - 1);
		for (size_t i = 0; i < count; i++)
			for (uint32_t b = first[i]; b <= last[i]; b++) binTris[fill[b]++] = uint32_t(i);
	}

	// Scanline rasterisation of the texel centres inside the triangle, grown by
	// a fraction of a texel so T-junctions and shared edges never leave a crack.
	// Depth comes from the triangle's plane, clamped to its corners' range.
	void RasterTriangle(const SunVertex &a, const SunVertex &b, const SunVertex &c, int rowBegin, int rowEnd)
	{
		const double grow = 0.05;
		double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
		if (std::abs(area) < 1e-7) return;	// seen edge-on
		double dzdx = ((double(b.d) - a.d) * (c.y - a.y) - (double(c.d) - a.d) * (b.y - a.y)) / area;
		double dzdy = ((b.x - a.x) * (double(c.d) - a.d) - (c.x - a.x) * (double(b.d) - a.d)) / area;
		float dmin = std::min(a.d, std::min(b.d, c.d)), dmax = std::max(a.d, std::max(b.d, c.d));

		// Edge functions A x + B y + C, positive inside, relative to corner a.
		const SunVertex *p[3] = { &a, &b, &c };
		double A[3], B[3], C[3];
		double sign = area > 0 ? 1.0 : -1.0;
		for (int i = 0; i < 3; i++)
		{
			const SunVertex &p0 = *p[i], &p1 = *p[(i + 1) % 3];
			A[i] = -(p1.y - p0.y) * sign;
			B[i] = (p1.x - p0.x) * sign;
			C[i] = -(A[i] * (p0.x - a.x) + B[i] * (p0.y - a.y)) + grow * (std::abs(A[i]) + std::abs(B[i]));
		}
		double minX = std::min(a.x, std::min(b.x, c.x)) - grow, maxX = std::max(a.x, std::max(b.x, c.x)) + grow;
		double minY = std::min(a.y, std::min(b.y, c.y)) - grow, maxY = std::max(a.y, std::max(b.y, c.y)) + grow;
		int y0 = std::max(rowBegin, int(std::ceil(minY - 0.5)));
		int y1 = std::min(rowEnd - 1, int(std::floor(maxY - 0.5)));
		for (int ty = y0; ty <= y1; ty++)
		{
			double yc = ty + 0.5 - a.y;
			double xl = minX - a.x, xr = maxX - a.x;
			bool empty = false;
			for (int i = 0; i < 3; i++)
			{
				double k = B[i] * yc + C[i];
				if (A[i] > 1e-12) xl = std::max(xl, -k / A[i]);
				else if (A[i] < -1e-12) xr = std::min(xr, -k / A[i]);
				else if (k < 0) { empty = true; break; }
			}
			if (empty) continue;
			int x0 = std::max(0, int(std::ceil(xl + a.x - 0.5)));
			int x1 = std::min(width - 1, int(std::floor(xr + a.x - 0.5)));
			if (x0 > x1) continue;
			float *row = depth.data() + size_t(ty) * width;
			double base = a.d + dzdy * yc + dzdx * (x0 + 0.5 - a.x);
			for (int tx = x0; tx <= x1; tx++)
			{
				float d = std::clamp(float(base + dzdx * (tx - x0)), dmin, dmax);
				if (d < row[tx]) row[tx] = d;
			}
		}
	}

	void Worker()
	{
#ifdef __APPLE__
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
		int bands = Bands();
		while (!cancel)
		{
			int band = nextBand++;
			if (band >= bands) break;
			int rowBegin = band * BandRows, rowEnd = std::min(height, rowBegin + BandRows);
			for (uint32_t k = binStart[band]; k < binStart[band + 1]; k++)
			{
				const SunVertex *v = &verts[size_t(binTris[k]) * 3];
				RasterTriangle(v[0], v[1], v[2], rowBegin, rowEnd);
			}
		}
	}

	void Run()
	{
#ifdef __APPLE__
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
		uint64_t start = I_msTime();
		Bin();
		if (cancel) return;
		depth.assign(size_t(width) * height, 1e30f);
		for (unsigned i = 0; i < threads; i++)
			workers.emplace_back([this] { Worker(); });
		for (auto &w : workers) w.join();
		workers.clear();
		verts.clear();
		verts.shrink_to_fit();
		binTris.clear();
		binTris.shrink_to_fit();
		rasterTime = I_msTime() - start;
		if (!cancel) done = true;
	}
};

std::unique_ptr<SunBake> job;

// The shadow map in use and the sun it was baked for.
struct SunMap
{
	bool attempted = false;		// a bake finished or failed for yaw/elevation/size
	bool valid = false;			// a shadow map is uploaded
	float yaw = 0.f, elevation = 0.f;
	int size = 0;
	FVector3 axisU, axisV, axisD;
	double originU = 0, originV = 0, originD = 0;
	double texel = 1.0;
	int width = 0, height = 0;
	std::vector<float> depth;	// CPU copy for the HUD weapon
} current;

bool rebakeRequested;
float hudVisibility = 1.f;
uint64_t hudTime = 0;

FVector3 SunDirection(float yaw, float elevation)
{
	double y = yaw * (M_PI / 180.0), e = elevation * (M_PI / 180.0);
	return FVector3(float(std::cos(e) * std::cos(y)), float(std::cos(e) * std::sin(y)), float(std::sin(e)));
}

bool Available()
{
	// Metal uses the Vulkan resource model; the GL backends have no sun.
	return screen && screen->IsVulkan();
}

// Compiles the sun variants of the common scene shaders, and of the user shaders
// (GLDEFS) on the level's walls and flats, ahead of the first sunlit frame.
void PrecacheShaders(FLevelLocals *level)
{
	TArray<int> userShaders;
	auto add = [&](FTextureID id)
	{
		auto tex = TexMan.GetGameTexture(id);
		int index = tex ? tex->GetShaderIndex() : 0;
		if (index >= FIRST_USER_SHADER && userShaders.Find(index) == userShaders.Size())
			userShaders.Push(index);
	};
	for (auto &side : level->sides)
	{
		for (int part : { side_t::top, side_t::mid, side_t::bottom })
			add(side.GetTexture(part));
	}
	for (auto &sector : level->sectors)
	{
		add(sector.GetTexture(sector_t::floor));
		add(sector.GetTexture(sector_t::ceiling));
	}
	screen->PrecacheSunShaders(userShaders);
}

void StartBake(FLevelLocals *level)
{
	const FSunSettings &sun = level->sun;
	current.attempted = true;
	// Usually done by Sun_LevelLoaded already; not when a script turned the sun on.
	PrecacheShaders(level);
	current.yaw = sun.Yaw;
	current.elevation = sun.Elevation;
	current.size = r_sun_shadowsize;
	rebakeRequested = false;

	uint64_t meshStart = I_msTime();
	DoomLevelMesh::Update(level, false);
	DoomLevelMesh *mesh = level->levelMesh;
	if (!mesh || mesh->MeshElements.Size() == 0)
	{
		DPrintf(DMSG_NOTIFY, "SUN: no level geometry; the sun shines unshadowed\n");
		return;
	}

	auto bake = std::make_unique<SunBake>();
	bake->startTime = I_msTime();
	uint64_t meshTime = bake->startTime - meshStart;
	bake->yaw = sun.Yaw;
	bake->elevation = sun.Elevation;
	bake->size = current.size;
	FVector3 L = SunDirection(sun.Yaw, sun.Elevation);
	double yaw = sun.Yaw * (M_PI / 180.0);
	bake->axisD = -L;
	bake->axisU = FVector3(float(-std::sin(yaw)), float(std::cos(yaw)), 0.f);
	bake->axisV = (bake->axisD ^ bake->axisU).Unit();

	// Casters: everything solid of the playable map. Sky surfaces, horizon and
	// portal lines and sectors outside it (control sectors, skybox areas) are left out.
	// So are sector floors (not 3D floors): nothing visible lies below one, and
	// sunlight entering the ground through it leaves through a wall or a ceiling,
	// which cast. Floors can't shadow themselves that way, and the huge floors
	// of horizon seas don't widen the map.
	std::vector<bool> playable = GI_PlayableSectors(level);
	auto casts = [&](const Surface &s) {
		if (s.bSky) return false;
		if (s.type == ST_FLOOR && !s.controlSector) return false;
		if (s.sector && !playable[s.sector->Index()]) return false;
		if (s.type != ST_FLOOR && s.type != ST_CEILING && s.type != ST_MODEL && s.typeIndex >= 0)
		{
			line_t *line = level->sides[s.typeIndex].linedef;
			if (line->special == Line_Horizon || line->isVisualPortal()) return false;
		}
		return true;
	};
	std::vector<char> surfaceCasts(mesh->Surfaces.Size());
	for (unsigned i = 0; i < mesh->Surfaces.Size(); i++) surfaceCasts[i] = casts(mesh->Surfaces[i]);

	double minU = 1e30, maxU = -1e30, minV = 1e30, maxV = -1e30, minD = 1e30;
	std::vector<SunVertex> &verts = bake->verts;
	verts.reserve(mesh->MeshElements.Size());
	for (unsigned i = 0; i + 2 < mesh->MeshElements.Size(); i += 3)
	{
		if (!surfaceCasts[mesh->MeshSurfaces[i / 3]]) continue;
		for (int k = 0; k < 3; k++)
		{
			const FVector3 &p = mesh->MeshVertices[mesh->MeshElements[i + k]];
			double u = double(p.X) * bake->axisU.X + double(p.Y) * bake->axisU.Y + double(p.Z) * bake->axisU.Z;
			double v = double(p.X) * bake->axisV.X + double(p.Y) * bake->axisV.Y + double(p.Z) * bake->axisV.Z;
			double d = double(p.X) * bake->axisD.X + double(p.Y) * bake->axisD.Y + double(p.Z) * bake->axisD.Z;
			minU = std::min(minU, u); maxU = std::max(maxU, u);
			minV = std::min(minV, v); maxV = std::max(maxV, v);
			minD = std::min(minD, d);
			verts.push_back({ u, v, float(d) });
		}
	}
	if (verts.empty())
	{
		DPrintf(DMSG_NOTIFY, "SUN: nothing casts shadows; the sun shines unshadowed\n");
		return;
	}

	// Square texels; a border keeps the filter inside the map. Small maps don't
	// need texels below a map unit.
	const int border = 4;
	double extentU = maxU - minU, extentV = maxV - minV;
	double texel = std::max(std::max(extentU, extentV) / std::max(current.size - 2 * border, 64), 1.0);
	bake->texel = texel;
	bake->width = std::min(int(std::ceil(extentU / texel)) + 2 * border, SunBake::MaxSize);
	bake->height = std::min(int(std::ceil(extentV / texel)) + 2 * border, SunBake::MaxSize);
	bake->originU = minU - border * texel;
	bake->originV = minV - border * texel;
	// Depths are stored relative to the nearest caster to keep float precision.
	bake->originD = minD - 16.0;

	// Ceilings that aren't sky get a lid a little above them, in the solid over
	// the ceiling. Without it, the filter of a wall facing the sun under a ceiling
	// reaches rays that pass just above the ceiling's edge and lights a thin seam
	// along the top of the wall. The lid sits higher than the filter reaches.
	double lid = std::clamp((sun.Softness + 1.0) * texel / std::max(std::cos(sun.Elevation * (M_PI / 180.0)), 0.25) + 1.0, 2.0, 48.0);
	size_t casterVerts = verts.size();
	for (unsigned i = 0; i + 2 < mesh->MeshElements.Size(); i += 3)
	{
		const Surface &s = mesh->Surfaces[mesh->MeshSurfaces[i / 3]];
		if (!surfaceCasts[mesh->MeshSurfaces[i / 3]] || s.type != ST_CEILING || s.controlSector) continue;
		for (int k = 0; k < 3; k++)
		{
			FVector3 p = mesh->MeshVertices[mesh->MeshElements[i + k]];
			double z = p.Z + lid;
			verts.push_back({ p.X * bake->axisU.X + p.Y * bake->axisU.Y + z * bake->axisU.Z,
				p.X * bake->axisV.X + p.Y * bake->axisV.Y + z * bake->axisV.Z,
				float(p.X * bake->axisD.X + p.Y * bake->axisD.Y + z * bake->axisD.Z) });
		}
	}
	unsigned lids = unsigned((verts.size() - casterVerts) / 3);

	for (auto &v : verts)
	{
		v.x = (v.x - bake->originU) / texel;
		v.y = (v.y - bake->originV) / texel;
		v.d = float(v.d - bake->originD);
	}

	unsigned threads = std::max(1u, std::thread::hardware_concurrency());
	bake->threads = threads > 2 ? threads - 1 : threads;
	bake->triangles = unsigned(verts.size() / 3);
	bake->setupTime = I_msTime() - bake->startTime;
	SunBake *raw = bake.get();
	bake->coordinator = std::thread([raw] { raw->Run(); });
	DPrintf(DMSG_NOTIFY, "SUN: baking %dx%d shadow map (%.2f units per texel, %u triangles with %u ceiling lids) for yaw %.1f, elevation %.1f on %u threads%s\n",
		bake->width, bake->height, texel, bake->triangles, lids, sun.Yaw, sun.Elevation, bake->threads,
		meshTime > 1 ? FStringf(" (level mesh built in %u ms)", unsigned(meshTime)).GetChars() : "");
	job = std::move(bake);
}

void Upload()
{
	SunBake &b = *job;
	if (!screen->InitSunShadowMap(b.width, b.height, b.depth.data()))
	{
		current.valid = false;
		return;
	}
	current.valid = true;
	current.yaw = b.yaw;
	current.elevation = b.elevation;
	current.size = b.size;
	current.axisU = b.axisU;
	current.axisV = b.axisV;
	current.axisD = b.axisD;
	current.originU = b.originU;
	current.originV = b.originV;
	current.originD = b.originD;
	current.texel = b.texel;
	current.width = b.width;
	current.height = b.height;
	current.depth = std::move(b.depth);
	// The bake's own work; the map is picked up by the next frame after it.
	Printf(PRINT_LOG, "SUN: baked %dx%d in %u ms (%.2f units per texel, %u triangles; %u ms setup, %u ms raster)\n",
		b.width, b.height, unsigned(b.setupTime + b.rasterTime), b.texel, b.triangles, unsigned(b.setupTime), unsigned(b.rasterTime));
	DPrintf(DMSG_NOTIFY, "SUN: shadow map in use %u ms after the bake started\n", unsigned(I_msTime() - b.startTime));
}

void ReleaseMap()
{
	if (current.valid && screen) screen->InitSunShadowMap(0, 0, nullptr);
	current = SunMap();
}

// Fraction of the sun that reaches a point, from the CPU copy (3x3 filter).
float Visibility(const DVector3 &pos)
{
	if (!current.valid) return 1.f;
	double u = (pos.X * current.axisU.X + pos.Y * current.axisU.Y + pos.Z * current.axisU.Z - current.originU) / current.texel;
	double v = (pos.X * current.axisV.X + pos.Y * current.axisV.Y + pos.Z * current.axisV.Z - current.originV) / current.texel;
	double d = pos.X * current.axisD.X + pos.Y * current.axisD.Y + pos.Z * current.axisD.Z - current.originD - 2.0 * current.texel;
	int cx = int(std::floor(u)), cy = int(std::floor(v));
	float lit = 0.f;
	for (int y = cy - 1; y <= cy + 1; y++) for (int x = cx - 1; x <= cx + 1; x++)
	{
		if (x < 0 || y < 0 || x >= current.width || y >= current.height) { lit += 1.f; continue; }
		lit += current.depth[size_t(y) * current.width + x] >= d ? 1.f : 0.f;
	}
	return lit / 9.f;
}

} // namespace

void Sun_LevelLoaded(FLevelLocals *level)
{
	Sun_LevelUnloaded();
	// The sun variants of the scene shaders compile during the load instead of
	// on the first sunlit frame.
	if (Available() && level && r_sun && level->sun.Enabled)
	{
		uint64_t start = I_msTime();
		PrecacheShaders(level);
		DPrintf(DMSG_NOTIFY, "SUN: shader variants ready in %u ms\n", unsigned(I_msTime() - start));
	}
}

void Sun_LevelUnloaded()
{
	job.reset();
	ReleaseMap();
	rebakeRequested = false;
	hudVisibility = 1.f;
	hudTime = 0;
}

void Sun_Update(FLevelLocals *level)
{
	if (!Available() || !level || !r_sun || !level->sun.Enabled) return;
	const FSunSettings &sun = level->sun;
	if (job && job->done)
	{
		Upload();
		job.reset();
	}
	// Static models settle on the first tic, so wait for it before capturing.
	if (!job && level->maptime >= 1 && (!current.attempted || rebakeRequested || current.yaw != sun.Yaw ||
		current.elevation != sun.Elevation || current.size != *r_sun_shadowsize))
	{
		StartBake(level);
	}
}

void Sun_SetUniforms(FLevelLocals *level, HWViewpointUniforms &uniforms)
{
	uniforms.mSunDir = { 0, 0, 0, 0 };
	uniforms.mSunColor = { 0, 0, 0, 0 };
	uniforms.mSunParams = { 0, 0, 0, 0 };
	uniforms.mSunParams2 = { 0, 0, 0, 0 };
	if (!Available() || !level || !r_sun) return;
	const FSunSettings &sun = level->sun;
	// No sun until its first bake is done; a failed bake shines unshadowed.
	if (!sun.Enabled || sun.Intensity <= 0.f || !current.attempted || (job && !current.valid)) return;

	FVector3 L = current.valid ? SunDirection(current.yaw, current.elevation) : SunDirection(sun.Yaw, sun.Elevation);
	PalEntry c = sun.Color;
	// The scene buffer is gamma 2 encoded, so linear is the square.
	auto lin = [&](int v) { float f = v / 255.f; return f * f * sun.Intensity; };
	uniforms.mSunDir = { L.X, L.Z, L.Y, 1.f };
	uniforms.mSunColor = { lin(c.r), lin(c.g), lin(c.b), sun.Specular };
	uniforms.mSunParams = { 0.f, 0.f, sun.Softness, 0.f };
	if (current.valid)
	{
		float inv = float(1.0 / current.texel);
		const FVector3 &U = current.axisU, &V = current.axisV, &D = current.axisD;
		// Rows take the shaders' (x, height, y) positions.
		uniforms.mSunShadowU = { U.X * inv, U.Z * inv, U.Y * inv, float(-current.originU / current.texel) };
		uniforms.mSunShadowV = { V.X * inv, V.Z * inv, V.Y * inv, float(-current.originV / current.texel) };
		uniforms.mSunShadowD = { D.X, D.Z, D.Y, float(-current.originD) };
		uniforms.mSunParams = { float(current.width), float(current.height), sun.Softness, float(current.texel) };
	}
}

void Sun_SetHUDUniforms(HWViewpointUniforms &uniforms, const VSMatrix &viewMatrix, const DVector3 &eye)
{
	if (uniforms.mSunDir.W <= 0.f) return;
	// Ease in and out of shadows instead of popping.
	float target = Visibility(eye);
	uint64_t now = I_msTime();
	float dt = hudTime ? std::min(float(now - hudTime) * 0.001f, 0.25f) : 1.f;
	hudTime = now;
	hudVisibility += (target - hudVisibility) * std::min(dt / 0.12f, 1.f);

	FLOATTYPE in[4] = { uniforms.mSunDir.X, uniforms.mSunDir.Y, uniforms.mSunDir.Z, 0 }, out[4];
	VSMatrix view = viewMatrix;
	view.multMatrixPoint(in, out);
	FVector3 dir = FVector3(float(out[0]), float(out[1]), float(out[2])).Unit();
	// The weapon takes r_sun_weapon of the sun: flat sprites in full sun look pasted on.
	float scale = hudVisibility * r_sun_weapon;
	uniforms.mSunDir = { dir.X, dir.Y, dir.Z, 1.f };
	uniforms.mSunColor = { uniforms.mSunColor.X * scale, uniforms.mSunColor.Y * scale, uniforms.mSunColor.Z * scale, uniforms.mSunColor.W };
	uniforms.mSunParams = { 0.f, 0.f, 0.f, 0.f };
	uniforms.mSunParams2 = { 1.f, 1.f, 0.f, 0.f };
	if (scale <= 0.001f) uniforms.mSunDir.W = 0.f;
}

CCMD(r_sun_rebake)
{
	// Rebuild the level mesh too, so the bake sees doors, lifts and
	// polyobjects where they are now.
	if (primaryLevel) DoomLevelMesh::Invalidate(primaryLevel);
	rebakeRequested = true;
}
