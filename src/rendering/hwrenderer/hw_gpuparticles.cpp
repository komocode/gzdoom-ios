/*
** hw_gpuparticles.cpp
** Visual-only, stateless GPU particles. See hw_gpuparticles.h.
*/

#include "hw_gpuparticles.h"
#include "hw_irradiance.h"
#include "hw_modernrender.h"
#include "hw_renderstate.h"
#include "scene/hw_drawinfo.h"
#include "flatvertices.h"
#include "buffers.h"
#include "g_levellocals.h"
#include "p_local.h"
#include "p_trace.h"
#include "p_3dfloors.h"
#include "a_dynlight.h"
#include "actor.h"
#include "texturemanager.h"
#include "v_video.h"
#include "doomdef.h"
#include "hw_dynlightdata.h"
#include "hw_lightbuffer.h"
#include "scene/hw_drawstructs.h"
#include "c_dispatch.h"
#include "c_commandline.h"
#include "printf.h"
#include "i_interface.h"
#include "menustate.h"

#include <algorithm>
#include <cmath>

EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Bool, cl_capfps)
extern bool AppActive;

CUSTOM_CVARD(Float, r_gpuparticles_intensity, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG,
	"particle emission and smoke density multiplier (0.25-4); does not extend lifetimes")
{
	if (!std::isfinite(float(self))) self = 1.f;
	else if (self < 0.25f || self > 4.f) self = std::clamp(float(self), 0.25f, 4.f);
}

namespace
{

constexpr int MaxParticles = 16384;

// One corner of a particle quad. Every field is constant for the particle's
// lifetime; the vertex shader derives the current state from the elapsed time.
// Positions are in shader space: x, height, y.
struct FXVertex
{
	float x, y, z, startTime;      // VATTR_VERTEX
	float vx, vy, vz, gravity;     // VATTR_NORMAL
	float life, size0, size1, drag; // VATTR_VERTEX2
	float brightness, floorZ, streak; // VATTR_LIGHTMAP: > 0 additive emission, < 0 alpha-blended albedo scale
	float u, v;                    // VATTR_TEXCOORD: corner + 2 * (shape + 16 * flags)
	uint8_t r, g, b, a;            // VATTR_COLOR
	float nx, ny, nz, planeD;      // VATTR_NORMAL2: plane of the surface the particle came from, for the soft fade
	float radiusX, radiusY, radiusZ, volumeSeed; // VATTR_BONEWEIGHT: analytic ellipsoid semiaxes
};

static_assert(sizeof(FXVertex) == 104, "particle vertex layout");

enum EShape
{
	SHAPE_GLOW,    // sparks, embers, flashes
	SHAPE_SMOKE,   // dust and smoke, noisy soft edge
	SHAPE_CHUNK,   // hard-edged debris
	SHAPE_DROPLET, // blood
	SHAPE_RING,    // see EFXShape
	SHAPE_MOTE,
	// 6..11 are weather shapes in the shared shader. These are internal;
	// the public ZScript shape numbers remain unchanged.
	SHAPE_FUNNEL = 12,
	SHAPE_POWDER = 13,
};

enum
{
	PF_UNLIT = 1,  // alpha blended, but not lit by the scene
	PF_BOUNCE = 2, // flags above bit 1 contain the first floor contact in 1/128 s
};

struct Particle
{
	FVector3 pos;
	FVector3 vel;
	float gravity = 0;
	float drag = 1;
	float life = 1;
	float size0 = 1, size1 = 1;
	float brightness = 1;
	float floorZ = -1e6f;
	float streak = 0;
	float delay = 0;
	FVector3 planeNormal = { 0, 0, 0 };
	float planeD = -1e6f; // no plane: never faded
	FVector3 color = { 1, 1, 1 };
	float alpha = 1;
	int shape = SHAPE_GLOW;
	int flags = 0;
	FVector3 radii = { 1, 1, 1 };
	float volumeSeed = 0;
};

// Particles are numbered in emission order; particle n lives in slot n % MaxParticles.
struct Ring
{
	int64_t head = 0;
	int64_t tail = 0;
	float expiry[MaxParticles];
	int lastMapTime = 0;
	float lastNow = 0;
	FLevelLocals *level = nullptr;

	// The first particle of the last few draws. The GPU may still be drawing
	// those frames (frames in flight), so their slots are not overwritten.
	static constexpr int History = 8;
	int64_t drawn[History] = {};
	int drawIndex = 0;
	uint64_t drawFrame = ~uint64_t(0);

	bool Empty() const { return head == tail; }
	int Slot(int64_t n) const { return int(n % MaxParticles); }
	int64_t OldestDrawn() const { return *std::min_element(drawn, drawn + History); }
	void Reset() { head = tail = 0; drawFrame = ~uint64_t(0); for (auto &d : drawn) d = 0; }
};

Ring ring;

// Emission is bounded before any sector queries or buffer writes. The cadence
// is the measured interval between rendered frames, including presentation;
// it is deliberately not labelled a GPU duration. Only mode 3 adapts to it.
struct EmissionBudget
{
	int tic = -1, used = 0, smoke = 0, peak = 0;
	uint64_t emitted = 0, dropped = 0, distanceCulled = 0, impacts = 0;
	uint64_t frame = 0;
	int progressTic = -1, samples = 0, slowSamples = 0, longSamples = 0;
	float frameMs = 16.67f, baselineMs = 16.67f, quality = 1.f, eventScale = 1.f;
	float volumeUntil = -1e6f;
	FVector3 camera = { 0, 0, 0 };
	bool cameraValid = false;
	bool cameraOrthographic = false;

	int Limit() const { return int((r_gpuparticles >= 3 ? 640 : r_gpuparticles == 2 ? 384 : 192) * r_gpuparticles_intensity); }
	int SmokeLimit() const { return int(96 * r_gpuparticles_intensity * (r_gpuparticles >= 3 ? quality : 1.f)); }
	void ResetCounters() { emitted = dropped = distanceCulled = impacts = 0; peak = 0; }
};

EmissionBudget budget;

enum class ImpactMaterial { Stone, Metal, Wood, Glass, Soft };

struct ImpactSurface
{
	bool valid = false;
	FVector3 normal;
	FVector3 albedo;
	FVector3 point;
	ImpactMaterial material = ImpactMaterial::Stone;

	void SetPlane(Particle &p) const
	{
		p.planeNormal = normal;
		p.planeD = normal | point;
	}
};

ImpactSurface pendingSurface;

// Particles have their own generator: the game's generators must not advance.
uint32_t rngState = 0x2545F491;

float Rand()
{
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return (rngState >> 8) * (1.f / 16777216.f);
}

float Rand(float lo, float hi) { return lo + (hi - lo) * Rand(); }

FVector3 RandUnit()
{
	float z = Rand(-1.f, 1.f);
	float a = Rand(0.f, 6.2831853f);
	float r = std::sqrt(std::max(0.f, 1.f - z * z));
	return { r * std::cos(a), r * std::sin(a), z };
}

FVector3 Normalized(const FVector3 &v)
{
	float l = v.Length();
	return l > 1e-6f ? v / l : FVector3(0, 0, 1);
}

FVector3 Vary(const FVector3 &c, float amount)
{
	float s = 1.f + Rand(-amount, amount);
	return { std::clamp(c.X * s, 0.f, 1.f), std::clamp(c.Y * s, 0.f, 1.f), std::clamp(c.Z * s, 0.f, 1.f) };
}

bool Enabled()
{
	return r_gpuparticles > 0 && screen != nullptr;
}

int AdmitCount(int requested)
{
	const int available = std::max(0, std::min(budget.Limit() - budget.used, MaxParticles - int(ring.head - ring.OldestDrawn())));
	const int admitted = std::min(requested, available);
	budget.dropped += requested - admitted;
	return admitted;
}

// Authored counts and native singletons use the same multiplier exactly once.
// Essential layers retain one particle under distance/cadence thinning.
int RequestedCount(float base, bool essential = false)
{
	return std::max(essential ? 1 : 0, int(base * r_gpuparticles_intensity * budget.eventScale + 0.5f));
}

int Count(float base)
{
	return AdmitCount(RequestedCount(base * std::min<int>(r_gpuparticles, 2)));
}

int AdmitSmokeCount(int requested)
{
	int count = AdmitCount(requested);
	int admitted = std::min(count, std::max(0, budget.SmokeLimit() - budget.smoke));
	budget.dropped += count - admitted;
	return admitted;
}

int SmokeCount(float base)
{
	return AdmitSmokeCount(RequestedCount(base * std::min<int>(r_gpuparticles, 2)));
}

float DistanceScale(const FVector3 &pos)
{
	if (!budget.cameraValid) return 1.f;
	float distance = (pos - budget.camera).Length();
	if (distance >= 4096.f) return 0.f;
	return std::clamp(768.f / std::max(distance, 768.f), 0.15f, 1.f);
}

ImpactMaterial MaterialFor(FTextureID id)
{
	auto texture = TexMan.GetGameTexture(id);
	if (!texture) return ImpactMaterial::Stone;
	FString name = texture->GetName();
	name.ToUpper();
	// Named material families are intentional; a metalness *map* may contain
	// all-black values and by itself is not evidence that a surface is steel.
	if (name.IndexOf("METAL") >= 0 || name.IndexOf("STEEL") >= 0 || name.IndexOf("GRATE") >= 0 ||
		name.IndexOf("PIPE") >= 0 || name.IndexOf("BARS") >= 0 || name.Compare("WCPANEL") == 0 || name.Compare("WCTRIM") == 0 ||
		name.Compare("WCCRAT") == 0 || name.Compare("WCFLOOR") == 0 || name.Compare("WCCEIL") == 0 || name.Compare("WCEXIT") == 0) return ImpactMaterial::Metal;
	if (name.IndexOf("WOOD") >= 0 || name.IndexOf("CRAT") >= 0) return ImpactMaterial::Wood;
	if (name.IndexOf("GLASS") >= 0) return ImpactMaterial::Glass;
	if (name.IndexOf("WATER") >= 0 || name.Compare("WCSEA") == 0 || name.IndexOf("SLIME") >= 0) return ImpactMaterial::Soft;
	return ImpactMaterial::Stone;
}

// Approximate brightness of an alpha-blended particle from Doom's light level,
// matched to how bright nearby walls and floors render.
float LightFactor(const sector_t *sector)
{
	float l = sector ? sector->lightlevel / 255.f : 1.f;
	return std::clamp(l * 1.5f - 0.1f, 0.08f, 1.f);
}

float GameTime(FLevelLocals *level)
{
	return level->totaltime / float(TICRATE);
}

bool CreateBuffers()
{
	if (screen->mFXParticleVertices) return true;

	auto vb = screen->CreateVertexBuffer();
	auto ib = screen->CreateIndexBuffer();
	if (!vb || !ib)
	{
		delete vb;
		delete ib;
		return false;
	}

	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_VERTEX, VFmt_Float4, (int)myoffsetof(FXVertex, x) },
		{ 0, VATTR_NORMAL, VFmt_Float4, (int)myoffsetof(FXVertex, vx) },
		{ 0, VATTR_VERTEX2, VFmt_Float4, (int)myoffsetof(FXVertex, life) },
		{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FXVertex, brightness) },
		{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FXVertex, u) },
		{ 0, VATTR_COLOR, VFmt_Byte4, (int)myoffsetof(FXVertex, r) },
		{ 0, VATTR_NORMAL2, VFmt_Float4, (int)myoffsetof(FXVertex, nx) },
		{ 0, VATTR_BONEWEIGHT, VFmt_Float4, (int)myoffsetof(FXVertex, radiusX) },
	};
	vb->SetData(MaxParticles * 4 * sizeof(FXVertex), nullptr, BufferUsageType::Persistent);
	vb->SetFormat(1, 8, sizeof(FXVertex), format);
	memset(vb->Memory(), 0, MaxParticles * 4 * sizeof(FXVertex));

	TArray<uint32_t> indices(MaxParticles * 6, true);
	for (uint32_t i = 0; i < MaxParticles; i++)
	{
		static const uint32_t quad[6] = { 0, 1, 2, 2, 1, 3 };
		for (int j = 0; j < 6; j++) indices[i * 6 + j] = i * 4 + quad[j];
	}
	ib->SetData(indices.Size() * sizeof(uint32_t), indices.Data(), BufferUsageType::Static);

	screen->mFXParticleVertices = vb;
	screen->mFXParticleIndices = ib;
	ring.Reset();
	return true;
}

// Only the range [tail, head) is drawn, so forgetting the particles is enough;
// their slots are rewritten as new particles are spawned.
void ClearRing()
{
	// Only the live range is drawn, so emptying it is enough. The slots stay
	// untouched for frames that may still be drawing them.
	ring.tail = ring.head;
}

uint8_t ToByte(float v) { return (uint8_t)std::clamp(int(v * 255.f + 0.5f), 0, 255); }

// Where a falling particle comes to rest: follows the closed-form trajectory
// in a few steps and returns the floor or 3D floor top under the first sample
// that is below it, so debris thrown off a ledge lands on the ground below.
float LandingFloor(const Particle &p)
{
	FLevelLocals *level = ring.level;
	float k = std::max(p.drag, 0.01f);
	FVector3 terminal(0, 0, p.gravity / k);
	const int flags = FFCF_3DRESTRICT | FFCF_NOPORTALS;
	sector_t *sec = level->PointInSector(DVector2(p.pos.X, p.pos.Y));
	float floor = float(NextLowestFloorAt(sec, p.pos.X, p.pos.Y, p.pos.Z + 1, flags));
	const int steps = 6;
	for (int i = 1; i <= steps; i++)
	{
		float t = p.life * i / steps;
		FVector3 pos = p.pos + terminal * t + (p.vel - terminal) * ((1.f - std::exp(-k * t)) / k);
		sec = level->PointInSector(DVector2(pos.X, pos.Y));
		float below = float(NextLowestFloorAt(sec, pos.X, pos.Y, std::max(pos.Z, floor) + 1, flags));
		if (pos.Z <= below) return below;
		floor = below;
	}
	return floor;
}

// Solve once at emission. Both diminishing bounces and settling then run on
// the GPU without CPU simulation or changes to persistent vertex memory.
float LandingTime(const Particle &p, float floorZ)
{
	const float k = std::max(p.drag, 0.01f), terminal = p.gravity / k;
	const float rest = floorZ + p.size1 * 0.5f;
	auto height = [&](float t) { return p.pos.Z + terminal * t + (p.vel.Z - terminal) * ((1.f - std::exp(-k * t)) / k); };
	if (height(p.life) > rest) return -1.f;
	float lo = 0, hi = p.life;
	for (int i = 0; i < 12; i++)
	{
		float mid = (lo + hi) * 0.5f;
		if (height(mid) > rest) lo = mid; else hi = mid;
	}
	return hi;
}

void Emit(const Particle &p, float now)
{
	const bool volume = p.shape == SHAPE_FUNNEL || p.shape == SHAPE_POWDER;
	const bool smoke = p.shape == SHAPE_SMOKE || volume;
	if (budget.used >= budget.Limit() || (smoke && budget.smoke >= budget.SmokeLimit()))
	{
		budget.dropped++;
		return;
	}
	if (DistanceScale(p.pos) <= 0.f) { budget.distanceCulled++; return; }
	FXVertex *vertices = static_cast<FXVertex*>(screen->mFXParticleVertices->Memory());
	if (!vertices) return;

	// Full: drop the new particle rather than overwrite one a frame may still draw.
	if (ring.head - ring.OldestDrawn() >= MaxParticles) { budget.dropped++; return; }
	if (ring.head - ring.tail >= MaxParticles) ring.tail++;
	int slot = ring.Slot(ring.head++);
	budget.used++;
	budget.smoke += smoke;
	budget.emitted++;
	budget.peak = std::max(budget.peak, int(ring.head - ring.tail));

	float start = now + p.delay;
	ring.expiry[slot] = start + p.life;
	if (volume) budget.volumeUntil = std::max(budget.volumeUntil, start + p.life);

	float floorZ = p.floorZ;
	if (floorZ > -1e5f && p.gravity < -100.f && ring.level) floorZ = LandingFloor(p);
	int flags = p.flags;
	if (p.shape == SHAPE_CHUNK && p.gravity < -100.f && floorZ > -1e5f)
	{
		float hit = LandingTime(p, floorZ);
		if (hit >= 0 && hit < 16.f) flags |= PF_BOUNCE | (std::max(1, int(hit * 128.f + 0.5f)) << 2);
	}

	FXVertex v;
	v.x = p.pos.X; v.y = p.pos.Z; v.z = p.pos.Y;
	v.startTime = start;
	v.vx = p.vel.X; v.vy = p.vel.Z; v.vz = p.vel.Y;
	v.gravity = p.gravity;
	v.life = p.life;
	v.size0 = p.size0;
	v.size1 = p.size1;
	v.drag = std::max(p.drag, 0.01f);
	v.brightness = p.brightness;
	v.floorZ = floorZ;
	v.streak = p.streak;
	v.r = ToByte(p.color.X);
	v.g = ToByte(p.color.Y);
	v.b = ToByte(p.color.Z);
	v.a = ToByte(p.alpha);
	v.nx = p.planeNormal.X; v.ny = p.planeNormal.Z; v.nz = p.planeNormal.Y;
	v.planeD = p.planeD;
	v.radiusX = p.radii.X; v.radiusY = p.radii.Y; v.radiusZ = p.radii.Z;
	v.volumeSeed = p.volumeSeed;

	for (int c = 0; c < 4; c++)
	{
		v.u = float(c & 1) + 2.f * float(p.shape + 16 * flags);
		v.v = float(c >> 1);
		vertices[slot * 4 + c] = v;
	}
}

bool BeginSpawn(FLevelLocals *level, float &now, const DVector3 &at)
{
	if (!Enabled() || level == nullptr || level != primaryLevel) return false;
	if (!CreateBuffers()) return false;
	now = GameTime(level);
	if (ring.level != level || level->maptime < ring.lastMapTime || now < ring.lastNow - 1.f)
	{
		ClearRing();
		budget = EmissionBudget();
		ring.lastMapTime = level->maptime;
		ring.lastNow = now;
	}
	ring.level = level;
	if (budget.tic != level->totaltime)
	{
		budget.tic = level->totaltime;
		budget.used = budget.smoke = 0;
	}
	float distance = DistanceScale(FVector3(at));
	if (distance <= 0.f) { budget.distanceCulled++; return false; }
	float pressure = std::clamp((MaxParticles - float(ring.head - ring.tail)) / (MaxParticles * 0.35f), 0.15f, 1.f);
	budget.eventScale = distance * pressure * (r_gpuparticles >= 3 ? budget.quality : 1.f);
	return true;
}

void EmitSparks(const DVector3 &at, const FVector3 &normal, int count, const FVector3 &tint, float floorZ, float now, float speed = 1.f, const ImpactSurface *surface = nullptr)
{
	for (int i = 0; i < count; i++)
	{
		Particle p;
		p.pos = FVector3(at);
		p.vel = Normalized(normal + RandUnit() * 0.8f) * Rand(180.f, 480.f) * speed;
		p.gravity = -800.f;
		p.drag = 2.f;
		p.life = Rand(0.12f, 0.42f);
		p.size0 = Rand(0.7f, 1.1f);
		p.size1 = 0.25f;
		p.brightness = Rand(3.5f, 6.f);
		p.color = { tint.X, std::clamp(tint.Y * Rand(0.85f, 1.05f), 0.f, 1.f), std::clamp(tint.Z * Rand(0.7f, 1.1f), 0.f, 1.f) };
		p.streak = 0.03f;
		p.floorZ = floorZ;
		if (surface) surface->SetPlane(p);
		Emit(p, now);
	}
}

void EmitFlash(const DVector3 &at, float size, const FVector3 &tint, float brightness, float life, float now)
{
	Particle p;
	p.pos = FVector3(at);
	p.vel = { 0, 0, 0 };
	p.life = life;
	p.size0 = size;
	p.size1 = size * 0.3f;
	// One brighter additive core is equivalent to coincident flash copies.
	p.brightness = brightness * r_gpuparticles_intensity;
	p.color = tint;
	Emit(p, now);
}

// Continuous, deterministic variation along one shot. It never touches the
// gameplay RNG and keeps adjacent sections connected instead of scattering dots.
float FunnelNoise(float x, float seed)
{
	return std::sin(x * 1.73f + seed) * 0.62f + std::sin(x * 3.11f + seed * 1.37f) * 0.38f;
}

void EmitConcreteVolume(const ImpactSurface &surface, float floorZ, float light, float now)
{
	const FVector3 axis = surface.normal;
	const FVector3 tangent = Normalized(std::fabs(axis.Z) < 0.9f ? FVector3(-axis.Y, axis.X, 0) : FVector3(0, -axis.Z, axis.Y));
	const FVector3 bitangent = axis ^ tangent;
	const FVector3 origin = surface.point + axis * 0.6f;
	const float seed = Rand(0.f, 100.f), life = Rand(0.83f, 1.05f);
	const float opacity = Rand(0.76f, 1.24f), length = Rand(45.f, 61.f);
	const float width = Rand(0.88f, 1.12f), flare = Rand(0.82f, 1.18f);
	// Calibrated in the actual level after sector/GI lighting and tone mapping:
	// the offline room's lower radiance made the native pressure body vanish.
	const FVector3 dust = (surface.albedo * 0.18f + FVector3(0.19f, 0.195f, 0.19f)) * 1.6f;
	const float extinctionScale = 2.5f;
	// Select a complete chain before emission. Low budgets use wider axial
	// sections, never a truncated cone with its tip missing.
	const int requested = budget.eventScale > 0.72f && Rand() > 0.25f ? 16 : budget.eventScale > 0.35f ? 8 : 4;
	const int available = AdmitSmokeCount(requested);
	const int sections = available >= 16 ? 16 : available >= 8 ? 8 : available >= 4 ? 4 : 0;
	budget.dropped += available - sections;
	const float spacing = length / std::max(sections, 1);
	for (int i = 0; i < sections; i++)
	{
		const float u = (i + 0.5f) / sections;
		const float radius = (0.4f + 11.52f * std::pow(u, flare)) * width;
		const float rx = radius * (1.f + 0.45f * FunnelNoise(u * 4.8f, seed) + 0.18f * FunnelNoise(u * 11.2f, seed + 1));
		const float ry = radius * (1.f + 0.42f * FunnelNoise(u * 4.8f + 0.4f, seed + 2) + 0.18f * FunnelNoise(u * 11.2f, seed + 3));
		Particle p;
		p.shape = SHAPE_FUNNEL;
		p.pos = origin;
		// For this shape velocity stores the final displacement, evaluated by
		// the GPU with the approved fast pressure burst / gentle easing tail.
		p.vel = axis * (u * length + spacing * 0.24f * FunnelNoise(u * 7.3f, seed + 6))
			+ tangent * (rx * 0.4f * FunnelNoise(u * 3.1f, seed + 4))
			+ bitangent * (ry * 0.4f * FunnelNoise(u * 3.9f, seed + 5));
		p.radii = { rx, ry, spacing * 1.45f };
		p.life = life;
		p.brightness = -std::max(light, 0.12f);
		p.color = dust;
		p.alpha = std::min(opacity, 1.f);
		p.streak = (0.95f + 0.28f * FunnelNoise(u * 6.2f, seed + 7)) * (2.6f - u * 0.5f)
			/ ((16.f / 15.f) * 1.45f * 16.f) * std::max(opacity, 1.f) * extinctionScale * r_gpuparticles_intensity;
		p.floorZ = floorZ;
		p.volumeSeed = seed;
		surface.SetPlane(p);
		Emit(p, now);
	}

	// Small 3D powder volumes carry the concrete detail. Their mass
	// falls as they expand; no screen-size floor or luminous confetti.
	for (int i = 0, n = AdmitSmokeCount(RequestedCount(6, true)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_POWDER;
		p.pos = origin;
		p.vel = Normalized(axis + tangent * Rand(-0.55f, 0.55f) + bitangent * Rand(-0.5f, 0.5f)) * Rand(44.8f, 83.2f);
		p.gravity = 4.8f;
		p.drag = 4.8f;
		p.delay = Rand(0.f, 0.035f);
		p.life = Rand(0.42f, 0.72f);
		const float radius = Rand(0.88f, 1.28f);
		p.radii = { radius * Rand(0.8f, 1.15f), radius * Rand(0.6f, 1.f), radius * Rand(1.2f, 1.7f) };
		p.brightness = -std::max(light, 0.12f);
		p.color = dust * Rand(0.85f, 1.f);
		p.streak = (30.f / 16.f) * opacity * extinctionScale;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}
	for (int i = 0, n = AdmitCount(RequestedCount(2, true)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_CHUNK;
		p.pos = origin;
		p.vel = Normalized(axis + RandUnit() * 0.55f) * Rand(72.f, 120.f);
		p.gravity = -128.f;
		p.drag = 2.2f;
		p.life = Rand(0.24f, 0.44f);
		p.size0 = Rand(0.22f, 0.42f);
		p.size1 = 0.12f;
		p.brightness = -std::max(light, 0.12f);
		p.color = dust * 0.4f;
		p.alpha = 0.35f;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}
}

void EmitCinematicImpact(const ImpactSurface &surface, float floorZ, float light, float now)
{
	budget.impacts++;
	if (surface.material == ImpactMaterial::Stone && screen->SceneDepthReadable() && !budget.cameraOrthographic)
	{
		EmitConcreteVolume(surface, floorZ, light, now);
		return;
	}
	const bool metal = surface.material == ImpactMaterial::Metal;
	const bool wood = surface.material == ImpactMaterial::Wood;
	const bool glass = surface.material == ImpactMaterial::Glass;
	const bool soft = surface.material == ImpactMaterial::Soft;
	const FVector3 normal = surface.normal;
	// The plume starts at the actual bullet hole, not the actor puff's offset.
	const FVector3 origin = surface.point + normal * 1.2f;
	FVector3 chip = metal ? surface.albedo * 0.65f + FVector3(0.25f, 0.27f, 0.28f)
		: wood ? surface.albedo * 0.7f + FVector3(0.19f, 0.12f, 0.055f)
		: surface.albedo * 0.55f + FVector3(0.38f, 0.37f, 0.34f);
	FVector3 dust = surface.albedo * 0.38f + (metal ? FVector3(0.36f, 0.38f, 0.37f) : FVector3(0.45f, 0.44f, 0.41f));

	// The defining effect is the material's pressurized dust/smoke jet. Emit
	// these layers first so clouds retain priority over optional grit/sparks.
	// Thirty base puffs per close hit, short lifetimes and a shared smoke admission
	// limit bound overdraw even during shotgun volleys and scripted collapses.
	for (int layer = 0; layer < 3; layer++)
	for (int i = 0, n = SmokeCount((soft ? 0.5f : 1.f) * (layer == 0 ? 3.f : layer == 1 ? 8.f : 4.f)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_SMOKE;
		FVector3 lateral = RandUnit();
		lateral = lateral - normal * (lateral | normal);
		p.pos = origin + lateral * Rand(0.f, 2.2f) + normal * Rand(0.f, 1.8f);
		p.vel = Normalized(normal + RandUnit() * (layer == 0 ? 0.32f : layer == 1 ? 0.55f : 0.85f)) *
			Rand(layer == 0 ? 150.f : layer == 1 ? 90.f : 35.f, layer == 0 ? 300.f : layer == 1 ? 220.f : 95.f);
		p.gravity = layer == 0 ? -8.f : layer == 1 ? 4.f : 12.f;
		p.drag = layer == 0 ? 4.2f : layer == 1 ? 2.9f : 2.1f;
		p.delay = layer == 0 ? 0.f : layer == 1 ? Rand(0.f, 0.055f) : Rand(0.035f, 0.10f);
		p.life = layer == 0 ? Rand(0.22f, 0.40f) : layer == 1 ? Rand(0.70f, 1.35f) : Rand(1.1f, 1.9f);
		p.size0 = layer == 0 ? Rand(2.f, 4.f) : layer == 1 ? Rand(3.f, 7.f) : Rand(5.f, 9.f);
		p.size1 = layer == 0 ? Rand(15.f, 24.f) : layer == 1 ? Rand(23.f, 36.f) : Rand(28.f, 42.f);
		p.brightness = -std::max(light, 0.12f);
		p.color = Vary(dust, 0.13f);
		p.alpha = (layer == 0 ? 0.48f : layer == 1 ? 0.30f : 0.16f) * (soft ? 0.65f : 1.f);
		p.streak = layer == 0 ? 0.045f : layer == 1 ? 0.018f : 0.f;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}

	// Steel keeps its hot streak shower inside the smoke. Concrete emits
	// non-emissive powder rather than an orange flash on every bullet hole.
	if (metal)
	{
		EmitFlash(DVector3(origin), 3.5f, { 1.f, 0.9f, 0.65f }, 3.f, 0.045f, now);
		EmitSparks(DVector3(origin), normal, Count(14), { 1.f, 0.78f, 0.38f }, floorZ, now, 1.35f, &surface);
	}

	// A few tiny, fast fragments show the surface breaking. They disappear
	// before they can form a persistent polygon carpet around every shot.
	for (int i = 0, n = soft ? 0 : Count(metal || glass ? 1.f : 2.f); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_CHUNK;
		p.pos = origin;
		p.vel = Normalized(normal + RandUnit() * 0.85f + FVector3(0, 0, 0.2f)) * Rand(130.f, 280.f);
		p.gravity = -850.f;
		p.drag = Rand(1.1f, 2.1f);
		p.life = Rand(0.32f, 0.72f);
		p.size0 = Rand(0.25f, 0.7f);
		p.size1 = 0.12f;
		p.brightness = -std::max(light, 0.15f);
		p.color = Vary(chip, 0.3f);
		p.alpha = glass ? 0.55f : 0.85f;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}

	// Dense, soft particulate spray adds detail around and within the volume
	// without large opaque polygons or large translucent screen coverage.
	for (int layer = 0; layer < 2; layer++)
	for (int i = 0, n = Count((soft ? 0.5f : 1.f) * (layer ? 16.f : 40.f)); i < n; i++)
	{
		Particle p;
		p.shape = soft && layer == 0 ? SHAPE_DROPLET : SHAPE_MOTE;
		p.pos = origin;
		p.vel = Normalized(normal + RandUnit() * (layer ? 0.95f : 0.75f)) * Rand(layer ? 45.f : 150.f, layer ? 125.f : 420.f);
		p.gravity = layer ? -8.f : -95.f;
		p.drag = layer ? 2.1f : 3.2f;
		p.life = layer ? Rand(0.9f, 1.8f) : Rand(0.3f, 0.85f);
		p.size0 = layer ? Rand(0.35f, 0.7f) : Rand(0.15f, 0.5f);
		p.size1 = layer ? Rand(0.6f, 1.1f) : 0.10f;
		p.color = Vary(dust, 0.2f);
		p.brightness = -std::max(light, 0.12f);
		p.alpha = layer ? 0.38f : 0.70f;
		p.streak = layer ? 0.f : 0.004f;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}
}

} // namespace

void FX_SetImpactSurface(const FTraceResults &trace)
{
	pendingSurface.valid = false;
	if (!Enabled()) return;

	FTextureID texture;
	FVector3 normal;
	switch (trace.HitType)
	{
	case TRACE_HitWall:
	{
		if (!trace.Line) return;
		DVector2 d = trace.Line->Delta();
		normal = Normalized(FVector3(float(d.Y), float(-d.X), 0.f));
		if (normal.X * trace.HitVector.X + normal.Y * trace.HitVector.Y > 0) normal = -normal;

		if (trace.Tier == TIER_FFloor && trace.ffloor && trace.ffloor->master && trace.ffloor->master->sidedef[0])
		{
			texture = trace.ffloor->master->sidedef[0]->GetTexture(side_t::mid);
		}
		else if (side_t *side = trace.Line->sidedef[trace.Side])
		{
			int part = trace.Tier == TIER_Upper ? side_t::top : trace.Tier == TIER_Lower ? side_t::bottom : side_t::mid;
			texture = side->GetTexture(part);
		}
		break;
	}
	case TRACE_HitFloor:
		normal = { 0, 0, 1 };
		if (trace.ffloor && trace.ffloor->top.texture) texture = *trace.ffloor->top.texture;
		else if (trace.Sector) texture = trace.Sector->GetTexture(sector_t::floor);
		break;
	case TRACE_HitCeiling:
		normal = { 0, 0, -1 };
		if (trace.ffloor && trace.ffloor->bottom.texture) texture = *trace.ffloor->bottom.texture;
		else if (trace.Sector) texture = trace.Sector->GetTexture(sector_t::ceiling);
		break;
	default:
		return;
	}

	pendingSurface.valid = true;
	pendingSurface.normal = normal;
	pendingSurface.albedo = GI_TextureAlbedo(texture);
	pendingSurface.point = FVector3(trace.HitPos);
	pendingSurface.material = MaterialFor(texture);
}

void FX_ClearImpactSurface()
{
	pendingSurface.valid = false;
}

void FX_HitscanFired(AActor *shooter, const DVector3 &start, const DVector3 &end, DAngle angle)
{
	float now;
	if (shooter == nullptr || !BeginSpawn(shooter->Level, now, start)) return;

	DVector3 delta = end - start;
	double length = delta.Length();
	if (length < 1) return;
	FVector3 dir = FVector3(delta / length);

	// Tracer: a fast streak that ends where the shot does. Pellets of one shot
	// each get one, so a shotgun blast fans out.
	const float speed = 5000.f;
	float travel = float(length) - 32.f;
	if (travel > 64.f && Rand() < 0.7f)
	{
		Particle p;
		p.pos = FVector3(start) + dir * 32.f;
		p.vel = dir * speed;
		p.drag = 0.01f;
		p.life = travel / speed;
		p.size0 = p.size1 = 0.6f;
		p.brightness = 4.f * r_gpuparticles_intensity;
		p.color = { 1.f, 0.8f, 0.5f };
		p.alpha = 0.8f;
		p.streak = 0.015f;
		Emit(p, now);
	}

	// Casing and smoke once per shot, not per pellet.
	static AActor *lastShooter;
	static int lastTime = -1;
	if (lastShooter == shooter && lastTime == shooter->Level->totaltime) return;
	lastShooter = shooter;
	lastTime = shooter->Level->totaltime;

	FVector3 forward(float(angle.Cos()), float(angle.Sin()), 0.f);
	FVector3 right(float(angle.Sin()), float(-angle.Cos()), 0.f);
	float light = LightFactor(shooter->Sector);
	float floorZ = float(shooter->floorz);

	// Brass, ejected to the right; it lies on the floor for a while.
	for (int i = 0, n = AdmitCount(RequestedCount(1, true)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_CHUNK;
		p.pos = FVector3(start) + forward * 14.f + right * 6.f - FVector3(0, 0, 6.f);
		p.vel = right * Rand(60.f, 110.f) + forward * Rand(-10.f, 20.f) + FVector3(0, 0, Rand(80.f, 140.f));
		p.gravity = -900.f;
		p.drag = 0.3f;
		p.life = Rand(6.f, 9.f);
		p.size0 = p.size1 = 0.45f;
		p.brightness = -light * 1.3f;
		p.color = Vary({ 0.85f, 0.62f, 0.25f }, 0.1f);
		p.floorZ = floorZ;
		Emit(p, now);
	}

	// A thin wisp of gun smoke drifting up from the muzzle.
	for (int i = 0, n = AdmitSmokeCount(RequestedCount(1, true)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_SMOKE;
		p.pos = FVector3(start) + dir * 30.f;
		p.vel = dir * 30.f + FVector3(0, 0, 12.f);
		p.gravity = 10.f;
		p.drag = 1.5f;
		p.life = Rand(1.f, 1.6f);
		p.size0 = 2.f;
		p.size1 = 10.f;
		p.brightness = -light;
		p.color = { 0.75f, 0.75f, 0.72f };
		p.alpha = 0.25f;
		p.floorZ = floorZ;
		Emit(p, now);
	}
}

void FX_PuffSpawned(AActor *source, AActor *puff, const DVector3 &pos, DAngle hitdir, int flags)
{
	ImpactSurface surface = pendingSurface;
	pendingSurface.valid = false;

	float now;
	if (puff == nullptr || (flags & (PF_TEMPORARY | PF_HITSKY | PF_MELEERANGE))) return;
	if (!surface.valid && !(puff->flags4 & MF4_ALLOWPARTICLES)) return; // only bullet-like puffs
	if (!BeginSpawn(puff->Level, now, pos)) return;

	// Puffs face the shooter, so without a surface the spray goes back that way.
	DAngle back = hitdir + DAngle::fromDeg(180);
	FVector3 normal = surface.valid ? surface.normal : Normalized(FVector3(float(back.Cos()), float(back.Sin()), 0.3f));
	float floorZ = float(puff->floorz);
	float light = LightFactor(puff->Sector);
	if (r_gpuparticles >= 3 && surface.valid)
	{
		EmitCinematicImpact(surface, floorZ, light, now);
		return;
	}

	EmitFlash(pos, 5.f, { 1.f, 0.86f, 0.62f }, 6.f, 0.07f, now);
	EmitSparks(pos, normal, Count(8), { 1.f, 0.85f, 0.55f }, floorZ, now, 1.f, surface.valid ? &surface : nullptr);

	if (!surface.valid) return;

	// Chips of the surface that was hit.
	for (int i = 0, n = Count(5); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_CHUNK;
		p.pos = FVector3(pos);
		p.vel = Normalized(normal + RandUnit() * 0.8f + FVector3(0, 0, 0.3f)) * Rand(90.f, 260.f);
		p.gravity = -1000.f;
		p.drag = 0.4f;
		p.life = Rand(1.f, 1.8f);
		p.size0 = p.size1 = Rand(0.5f, 1.3f);
		p.brightness = -light;
		p.color = Vary(surface.albedo, 0.2f);
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}

	// A lingering cloud of dust in the surface's colour.
	FVector3 dust = surface.albedo * 0.65f + FVector3(0.35f, 0.35f, 0.35f);
	for (int i = 0, n = std::max(2, Count(3)); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_SMOKE;
		p.pos = FVector3(pos);
		p.vel = Normalized(normal + RandUnit() * 0.5f) * Rand(25.f, 70.f);
		p.gravity = 8.f;
		p.drag = 1.6f;
		p.life = Rand(1.8f, 3.4f);
		p.size0 = Rand(3.f, 5.f);
		p.size1 = Rand(18.f, 30.f);
		p.brightness = -light;
		p.color = Vary(dust, 0.08f);
		p.alpha = 0.55f;
		p.floorZ = floorZ;
		surface.SetPlane(p);
		Emit(p, now);
	}
}

void FX_BloodSpawned(AActor *originator, const DVector3 &pos, DAngle dir, int damage)
{
	float now;
	if (originator == nullptr || !BeginSpawn(originator->Level, now, pos)) return;

	PalEntry bc = originator->BloodColor;
	FVector3 color = (bc.r | bc.g | bc.b) ? FVector3(bc.r / 255.f, bc.g / 255.f, bc.b / 255.f) * 0.6f : FVector3(0.45f, 0.02f, 0.02f);
	float light = LightFactor(originator->Sector);
	float floorZ = float(originator->floorz);
	float amount = std::clamp(damage / 10.f, 0.6f, 2.f);

	// dir points back at the attacker: most of the spray leaves through the far side.
	FVector3 toward(float(dir.Cos()), float(dir.Sin()), 0.f);
	for (int i = 0, n = Count(12 * amount); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_DROPLET;
		p.pos = FVector3(pos);
		float side = Rand() < 0.6f ? -1.f : 1.f;
		p.vel = Normalized(toward * (0.8f * side) + RandUnit() * 0.6f + FVector3(0, 0, 0.4f)) * Rand(80.f, 220.f);
		p.gravity = -900.f;
		p.drag = 0.8f;
		p.life = Rand(0.6f, 1.1f);
		p.size0 = Rand(1.f, 2.2f);
		p.size1 = 0.8f;
		p.brightness = -light;
		p.color = Vary(color, 0.2f);
		p.alpha = 0.95f;
		p.streak = 0.015f;
		p.floorZ = floorZ;
		Emit(p, now);
	}

	for (int i = 0, n = Count(2); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_SMOKE;
		p.pos = FVector3(pos);
		p.vel = Normalized(RandUnit() - toward * 0.5f) * 20.f;
		p.gravity = -20.f;
		p.drag = 3.f;
		p.life = Rand(0.5f, 0.8f);
		p.size0 = 4.f;
		p.size1 = 14.f;
		p.brightness = -light;
		p.color = color;
		p.alpha = 0.4f;
		p.floorZ = floorZ;
		Emit(p, now);
	}
}

void FX_Explosion(AActor *bombspot, double distance, int damage)
{
	float now;
	if (bombspot == nullptr || distance <= 0 || !BeginSpawn(bombspot->Level, now, bombspot->Pos())) return;

	float scale = std::clamp(float(distance) / 128.f, 0.5f, 2.5f);
	DVector3 pos = bombspot->Pos();
	pos.Z += std::min(bombspot->Height * 0.5, 16.);
	float floorZ = float(bombspot->floorz);
	float light = std::max(LightFactor(bombspot->Sector), 0.3f);

	// Fireball
	for (int i = 0, n = Count(6); i < n; i++)
	{
		Particle p;
		p.pos = FVector3(pos);
		p.vel = RandUnit() * (Rand(60.f, 140.f) * scale);
		p.gravity = 40.f;
		p.drag = 4.f;
		p.life = Rand(0.25f, 0.45f);
		p.size0 = 10.f * scale;
		p.size1 = 30.f * scale;
		p.brightness = 2.5f;
		p.color = { 1.f, Rand(0.5f, 0.65f), 0.22f };
		p.floorZ = floorZ;
		Emit(p, now);
	}

	// Embers
	for (int i = 0, n = Count(40 * scale); i < n; i++)
	{
		Particle p;
		p.pos = FVector3(pos);
		p.vel = Normalized(RandUnit() + FVector3(0, 0, 0.5f)) * (Rand(150.f, 500.f) * std::sqrt(scale));
		p.gravity = -500.f;
		p.drag = 1.8f;
		p.life = Rand(0.5f, 1.4f);
		p.size0 = 1.4f;
		p.size1 = 0.3f;
		p.brightness = Rand(3.f, 6.f);
		p.color = { 1.f, Rand(0.47f, 0.75f), Rand(0.15f, 0.3f) };
		p.streak = 0.035f;
		p.floorZ = floorZ;
		Emit(p, now);
	}

	// Debris
	for (int i = 0, n = Count(10 * scale); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_CHUNK;
		p.pos = FVector3(pos);
		p.vel = Normalized(RandUnit() + FVector3(0, 0, 0.6f)) * Rand(150.f, 350.f);
		p.gravity = -1000.f;
		p.drag = 0.5f;
		p.life = Rand(1.2f, 2.f);
		p.size0 = p.size1 = Rand(1.2f, 2.8f);
		p.brightness = -light;
		p.color = Vary({ 0.2f, 0.18f, 0.16f }, 0.3f);
		p.floorZ = floorZ;
		Emit(p, now);
	}

	// Smoke rises out of the fireball as it fades.
	for (int i = 0, n = Count(10 * scale); i < n; i++)
	{
		Particle p;
		p.shape = SHAPE_SMOKE;
		p.pos = FVector3(pos);
		p.vel = Normalized(RandUnit() + FVector3(0, 0, 0.6f)) * (Rand(30.f, 110.f) * scale);
		p.gravity = 25.f;
		p.drag = 1.4f;
		p.delay = Rand(0.1f, 0.3f);
		p.life = Rand(2.5f, 4.5f);
		p.size0 = 12.f * scale;
		p.size1 = 50.f * scale;
		p.brightness = -light;
		p.color = Vary({ 0.27f, 0.25f, 0.23f }, 0.15f);
		p.alpha = 0.6f;
		p.floorZ = floorZ;
		Emit(p, now);
	}
}

void FX_MissileExploded(AActor *missile)
{
	float now;
	if (missile == nullptr || missile->AttachedLights.Size() == 0) return;
	if (!BeginSpawn(missile->Level, now, missile->Pos())) return;

	FDynamicLight *light = missile->AttachedLights[0];
	FVector3 color(light->GetRed() / 255.f, light->GetGreen() / 255.f, light->GetBlue() / 255.f);
	float peak = std::max({ color.X, color.Y, color.Z });
	if (peak <= 0.f) return;
	color = color / peak;

	FVector3 back = missile->Vel.LengthSquared() > 0 ? -Normalized(FVector3(missile->Vel)) : FVector3(0, 0, 1);
	DVector3 pos = missile->Pos();
	EmitFlash(pos, 14.f, color, 4.f, 0.1f, now);
	EmitSparks(pos, back, Count(14), color, float(missile->floorz), now, 0.8f);
}

void FX_DrawParticles(HWDrawInfo *di, FRenderState &state)
{
	auto level = di->Level;
	// Created up front rather than at the first shot, so the backend can prepare its pipelines.
	if (!r_gpuparticles || level != primaryLevel || !CreateBuffers()) return;

	float now = (level->totaltime - 1 + float(di->Viewpoint.TicFrac)) / TICRATE;

	// A new map, a new game or a loaded savegame: the old particles no longer apply.
	if (ring.level != level || level->maptime < ring.lastMapTime || now < ring.lastNow - 1.f)
	{
		ClearRing();
		budget = EmissionBudget();
	}
	ring.level = level;
	ring.lastMapTime = level->maptime;
	ring.lastNow = now;
	if (di->outer == nullptr)
	{
		budget.camera = FVector3(di->Viewpoint.Pos);
		budget.cameraValid = true;
		// The shared depth producer currently linearizes perspective depth.
		// Orthographic cameras keep the previous card-based stone recipe.
		budget.cameraOrthographic = di->Viewpoint.bDoOrtho;
		if (screen->FrameTime != budget.frame)
		{
			float elapsed = float(screen->FrameTime - budget.frame);
			const bool active = budget.frame && elapsed > 0.f && level->totaltime != budget.progressTic &&
				AppActive && !paused && !pauseext && menuactive == MENU_Off;
			// Ignore an isolated map-load/compiler stall, but admit repeated
			// long active frames so severe overload still triggers fallback.
			budget.longSamples = active && elapsed >= 125.f ? budget.longSamples + 1 : 0;
			if (active && (elapsed < 125.f || budget.longSamples >= 2))
			{
				float sample = std::min(elapsed, 250.f);
				budget.frameMs += (sample - budget.frameMs) * 0.06f;
				budget.samples++;
				const int currentAdmissions = budget.tic == level->totaltime ? budget.used : 0;
				// A few large analytic volumes can fill the screen. Do not learn
				// their cost as the quiet baseline just because their count is low.
				const bool quiet = now >= budget.volumeUntil && ring.head - ring.tail < MaxParticles / 8 && currentAdmissions < 96;
				const bool loaded = !quiet;
				// Ambient motes and lingering rubble need not drain completely
				// before learning the scene's normal cadence. A small resident
				// range plus low current admission distinguishes that baseline
				// from destruction bursts; stale prior-tic counts do not block it.
				// This also avoids penalizing a 120 fps cap on a 60 Hz display.
				if (quiet) budget.baselineMs += (sample - budget.baselineMs) * 0.08f;
				float requested = cl_capfps ? 1000.f / TICRATE : 1000.f / std::clamp(vid_maxfps > 0 ? int(vid_maxfps) : 60, 1, 120);
				float target = std::max(requested, budget.baselineMs);
				if (r_gpuparticles >= 3)
				{
					if (loaded && budget.samples > TICRATE && budget.frameMs > target * 1.18f) budget.slowSamples++;
					else budget.slowSamples = 0;
					if (budget.slowSamples >= 20) budget.quality = std::max(0.3f, budget.quality - 0.012f);
					else if (budget.frameMs < target * 1.06f) budget.quality = std::min(1.f, budget.quality + 0.003f);
				}
				else budget.quality = 1.f;
			}
			budget.frame = screen->FrameTime;
			budget.progressTic = level->totaltime;
		}
	}

	while (!ring.Empty() && ring.expiry[ring.Slot(ring.tail)] <= now) ring.tail++;
	// Advance protection once per rendered frame, including empty frames.
	// Portal views must not consume eight history entries within one frame.
	if (ring.drawFrame != screen->FrameTime)
	{
		ring.drawn[ring.drawIndex] = ring.tail;
		ring.drawIndex = (ring.drawIndex + 1) % Ring::History;
		ring.drawFrame = screen->FrameTime;
	}
	if (ring.Empty()) return;
	int tail = ring.Slot(ring.tail), head = ring.Slot(ring.head);

	state.SetVertexBuffer(screen->mFXParticleVertices, 0, 0);
	state.SetIndexBuffer(screen->mFXParticleIndices);
	state.SetMaterial(TexMan.GetGameTexture(TexMan.glPart), UF_None, 0, CLAMP_XY_NOMIP, NO_TRANSLATION, -1);
	state.SetTextureMode(TM_NORMAL);
	// The view sector's fog for all of them; lights and GI are applied per vertex.
	sector_t *viewsector = di->Viewpoint.sector;
	if (viewsector) SetFog(state, level, di->lightmode, viewsector->lightlevel, 0, di->isFullbrightScene(), &viewsector->Colormap, false);
	else state.EnableFog(false);
	state.SetLightIndex(FX_ParticleLightList(di));
	state.SetNoSoftLightLevel();
	state.ResetColor();
	state.EnableBrightmap(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetDepthMask(false);
	state.SetDepthFunc(DF_LEqual);

	auto drawRange = [&]()
	{
		if (tail < head)
		{
			state.DrawIndexed(DT_Triangles, tail * 6, (head - tail) * 6);
		}
		else
		{
			state.DrawIndexed(DT_Triangles, tail * 6, (MaxParticles - tail) * 6);
			if (head > 0) state.DrawIndexed(DT_Triangles, 0, head * 6);
		}
	};

	// Soft fade against the opaque scene, scaled up by particle size in the shader.
	FVector4 soft = { di->SceneDepthReady && r_softparticles ? 4.f : 0.f, di->SceneDepthReady ? 1.f : 0.f, 0, 0 };

	// Smoke, dust, debris and blood first, then the emissive particles on top.
	state.SetRenderStyle(STYLE_Translucent);
	state.SetParticleParams(now, false);
	state.SetParticleExtra(soft, { 0, 0, 0, 0 });
	drawRange();

	state.SetRenderStyle(STYLE_Add);
	state.SetParticleParams(now, true);
	state.SetParticleExtra(soft, { 0, 0, 0, 0 });
	drawRange();

	state.SetParticleExtra({ 0, 0, 0, 0 }, { 0, 0, 0, 0 });
	state.SetLightIndex(-1);
	state.SetMaterialParams(0, 0);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetDepthMask(true);
	state.EnableFog(true);
	state.SetVertexBuffer(screen->mVertexData);
}

CCMD(r_gpuparticles_status)
{
	if (argv.argc() > 1 && stricmp(argv[1], "reset") == 0) budget.ResetCounters();
	// Expose cadence validity separately: background rendering intentionally
	// leaves the last measured interval frozen and is not a fresh benchmark.
	Printf("FX particles: mode=%d active=%d capacity=%d emitted=%llu dropped=%llu tick=%d/%d quality=%.3f frame_ms=%.2f impacts=%llu distance_culled=%llu peak=%d timing_samples=%d timing_active=%d intensity=%.2f\n",
		int(r_gpuparticles), int(ring.head - ring.tail), MaxParticles,
		(unsigned long long)budget.emitted, (unsigned long long)budget.dropped,
		budget.used, budget.Limit(), budget.quality, budget.frameMs,
		(unsigned long long)budget.impacts, (unsigned long long)budget.distanceCulled, budget.peak,
		budget.samples, int(AppActive && !paused && !pauseext && menuactive == MENU_Off), float(r_gpuparticles_intensity));
}

int FX_ParticleLightList(HWDrawInfo *di)
{
	FLevelLocals *level = di->Level;
	if (!level->HasDynamicLights || di->isFullbrightScene() || di->Viewpoint.sector == nullptr) return -1;

	// The strongest lights near the camera, as for the volumetric pass.
	struct Candidate { float score; FDynamicLight *light; };
	Candidate best[8];
	int count = 0;
	auto &vp = di->Viewpoint;
	DVector3 forward(vp.Angles.Yaw.Cos() * vp.Angles.Pitch.Cos(), vp.Angles.Yaw.Sin() * vp.Angles.Pitch.Cos(), -vp.Angles.Pitch.Sin());
	for (FDynamicLight *light = level->lights; light; light = light->next)
	{
		if (!light->IsActive() || light->IsSubtractive()) continue;
		float radius = light->GetRadius();
		if (radius <= 0) continue;
		DVector3 delta = light->Pos - vp.Pos;
		double dist = delta.Length();
		if (dist > 2048.0 + radius || (delta | forward) < -radius) continue;
		float brightness = std::max({ light->GetRed(), light->GetGreen(), light->GetBlue() }) / 255.f;
		if (brightness <= 0.02f) continue;
		Candidate c = { brightness * radius / float(dist + 64.0), light };
		if (count < 8) best[count++] = c;
		else
		{
			auto weakest = std::min_element(best, best + count, [](const Candidate &a, const Candidate &b) { return a.score < b.score; });
			if (c.score > weakest->score) *weakest = c;
		}
	}
	if (count == 0) return -1;

	FDynLightData data;
	data.Clear();
	int group = di->Viewpoint.sector->PortalGroup;
	for (int i = 0; i < count; i++) AddLightToList(data, group, best[i].light, false);
	return screen->mLights->UploadLights(data);
}

void FX_SpawnScripted(FLevelLocals *level, const FFXParticleParams &params)
{
	float now;
	if (params.count <= 0 || !BeginSpawn(level, now, params.pos)) return;

	int count = std::min(params.count, 1024);
	// Preserve tiny authored essentials (such as a single muzzle core) while
	// thinning dense layers. BeginSpawn already rejected zero-distance scale;
	// the hard ring/tic admission below still has final authority.
	count = RequestedCount(float(count), true);
	count = params.shape == SHAPE_SMOKE ? AdmitSmokeCount(count) : AdmitCount(count);
	if (count == 0) return;
	bool additive = !!(params.flags & FXF_Additive);
	bool unlit = !!(params.flags & FXF_Unlit);
	sector_t *sector = level->PointInSector(params.pos.XY());
	float light = LightFactor(sector);
	float floorZ = (params.flags & FXF_RestOnFloor) ? float(NextLowestFloorAt(sector, params.pos.X, params.pos.Y, params.pos.Z + 1, FFCF_3DRESTRICT | FFCF_NOPORTALS)) : -1e6f;
	float brightness = float(params.brightness);
	if (additive || unlit) brightness = std::max(brightness, 0.01f);

	// Velocity cone around dir.
	FVector3 axis = FVector3(params.dir);
	bool cone = axis.LengthSquared() > 1e-6f;
	FVector3 tangent, bitangent;
	if (cone)
	{
		axis = Normalized(axis);
		tangent = Normalized(std::fabs(axis.Z) < 0.9f ? FVector3(-axis.Y, axis.X, 0) : FVector3(0, -axis.Z, axis.Y));
		bitangent = axis ^ tangent;
	}
	float cosMax = float(std::cos(std::clamp(params.coneAngle, 0.0, 180.0) * (M_PI / 180.0)));

	PalEntry c1 = params.color1, c2 = params.color2 ? params.color2 : params.color1;
	FVector3 color1(c1.r / 255.f, c1.g / 255.f, c1.b / 255.f), color2(c2.r / 255.f, c2.g / 255.f, c2.b / 255.f);

	for (int i = 0; i < count; i++)
	{
		Particle p;
		p.shape = std::clamp(params.shape, 0, FXS_Count - 1);
		p.pos = FVector3(params.pos) + FVector3(Rand(-1.f, 1.f) * float(params.spread.X), Rand(-1.f, 1.f) * float(params.spread.Y), Rand(-1.f, 1.f) * float(params.spread.Z));
		FVector3 dir;
		if (cone)
		{
			float cosT = 1.f - Rand() * (1.f - cosMax);
			float sinT = std::sqrt(std::max(0.f, 1.f - cosT * cosT));
			float phi = Rand(0.f, 6.2831853f);
			dir = axis * cosT + (tangent * std::cos(phi) + bitangent * std::sin(phi)) * sinT;
		}
		else dir = RandUnit();
		p.vel = dir * Rand(float(params.speedMin), float(std::max(params.speedMax, params.speedMin)));
		p.gravity = -float(params.gravity);
		p.drag = std::max(float(params.drag), 0.01f);
		p.life = std::max(Rand(float(params.lifeMin), float(std::max(params.lifeMax, params.lifeMin))), 0.02f);
		p.size0 = std::max(float(params.sizeStart), 0.05f);
		p.size1 = std::max(float(params.sizeEnd), 0.05f);
		if (p.shape == SHAPE_CHUNK)
		{
			// A fracture produces abundant grit and fewer large slabs. Keep
			// each chip's size ratio stable over its lifetime, including rest.
			float variation = Rand();
			variation = 0.45f + 0.95f * variation * variation;
			p.size0 *= variation;
			p.size1 *= variation;
		}
		p.delay = Rand(0.f, std::max(float(params.delay), 0.f));
		float m = Rand();
		p.color = color1 * (1.f - m) + color2 * m;
		p.alpha = std::clamp(float(params.alpha), 0.f, 1.f);
		p.streak = std::max(float(params.stretch), 0.f);
		p.floorZ = floorZ;
		if (additive) p.brightness = brightness;
		else if (unlit)
		{
			p.brightness = -brightness;
			p.flags = PF_UNLIT;
		}
		else p.brightness = -std::max(light * brightness, 0.01f);
		Emit(p, now);
	}
}

// Entry point of LevelLocals.SpawnFXParticles. FXF_NoDistanceScale (sea spray
// at the foot of distant cliffs) skips the thinning with camera distance and
// culls at 8192 units instead of 4096; the per-tic, smoke and ring limits
// still apply. DistanceScale treats an unknown camera as near, so the camera
// is hidden from it while such an emitter spawns.
void FX_SpawnScriptedParticles(FLevelLocals *level, const FFXParticleParams &params)
{
	if (!(params.flags & FXF_NoDistanceScale) || !budget.cameraValid)
	{
		FX_SpawnScripted(level, params);
		return;
	}
	if ((FVector3(params.pos) - budget.camera).Length() >= 8192.f)
	{
		budget.distanceCulled++;
		return;
	}
	FVector3 camera = budget.camera;
	budget.cameraValid = false;
	FX_SpawnScripted(level, params);
	// A new level resets the budget, camera included; keep that reset.
	if (budget.camera == camera) budget.cameraValid = true;
}
