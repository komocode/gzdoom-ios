#pragma once

#include "vectors.h"

class AActor;
struct HWDrawInfo;
struct FLevelLocals;
class FRenderState;
struct FTraceResults;

// Visual-only GPU particles ("FX particles").
//
// Game particles (P_DrawSplash, rail trails, ZScript particles) are part of the
// simulation and stay as they are. These particles only decorate events the
// game already produces: bullet impacts, blood, explosions and projectile
// impacts. They never read or advance the game's random number generators and
// keep no state that the game, savegames or network play can see.
//
// Each particle is written once, at spawn time, into a ring of persistent
// vertex memory: start position and time, velocity, gravity, drag, lifetime,
// size, colour and shape. The vertex shader evaluates the motion in closed
// form every frame, so the CPU does no per-frame simulation at all:
//
//   v(t) = (v0 - g/k) e^(-k t) + g/k
//   p(t) = p0 + (g/k) t + (v0 - g/k) (1 - e^(-k t)) / k
//
// Particles that fall come to rest on the floor (or 3D floor) below where
// their trajectory lands, sparks and tracers stretch along their screen-space
// velocity, and smoke and dust take the colour of the surface that was hit.
// Non-emissive particles are lit in the vertex shader by the sector light at
// spawn, the baked GI, the strongest dynamic lights near the camera and
// lightning, and all of them take the view sector's fog and fade softly into
// the opaque scene when its depth is available (r_softparticles).
//
// The CPU only appends to the ring: a slot is written once, when a particle is
// spawned, and never modified while it can still be drawn, so frames still in
// flight on the GPU keep reading consistent data. When the ring is full the
// oldest slot is reused; with 16384 slots that is long dead.
//
// Game code can spawn them too (LevelLocals.SpawnFXParticles, FXEmitter in
// ZScript). They stay visual-only: their own random generator, no game state.

// Records the surface hit by the hitscan trace that is about to spawn a puff.
void FX_SetImpactSurface(const FTraceResults &trace);
void FX_ClearImpactSurface();

// A hitscan attack: tracer, ejected casing and muzzle smoke (once per shot).
void FX_HitscanFired(AActor *shooter, const DVector3 &start, const DVector3 &end, DAngle angle);
void FX_PuffSpawned(AActor *source, AActor *puff, const DVector3 &pos, DAngle hitdir, int flags);
void FX_BloodSpawned(AActor *originator, const DVector3 &pos, DAngle dir, int damage);
void FX_Explosion(AActor *bombspot, double distance, int damage);
void FX_MissileExploded(AActor *missile);

void FX_DrawParticles(HWDrawInfo *di, FRenderState &state);

// Uploads the strongest dynamic lights near the camera for lit particles and
// weather and returns the light list index for SetLightIndex (-1: none).
int FX_ParticleLightList(HWDrawInfo *di);

// Built-in particle shapes (no textures; the fragment shader draws them).
enum EFXShape
{
	FXS_Glow,		// hot core, soft halo: sparks, embers
	FXS_Smoke,		// noisy soft puff: smoke, dust, steam
	FXS_Chunk,		// hard-edged debris
	FXS_Droplet,	// dense drop with a glint: blood, water
	FXS_Ring,		// thin ring: foam, shock waves
	FXS_Mote,		// small soft dot that fades in and out: dust motes, pollen
	FXS_Count
};

enum EFXFlags
{
	FXF_Additive = 1,		// additive blend, emissive (sparks, embers, fire)
	FXF_Unlit = 2,			// alpha blended but emissive: not lit by the scene
	FXF_RestOnFloor = 4,	// land on the floor or 3D floor below the trajectory
	FXF_NoDistanceScale = 8,	// no thinning with camera distance, for effects seen from afar (FX_SpawnScriptedParticles)
};

// Parameters of LevelLocals.SpawnFXParticles (ZScript struct FXParticleParams).
struct FFXParticleParams
{
	int color1;			// colour, randomly mixed toward color2 per particle
	int color2;			// 0 = color1
	int count;
	int shape;			// EFXShape
	int flags;			// EFXFlags
	DVector3 pos;
	DVector3 spread;	// random offset within +- spread on each axis
	DVector3 dir;		// direction of the velocity cone (zero: any direction)
	double coneAngle;	// degrees off dir
	double speedMin, speedMax;
	double gravity;		// downward acceleration in map units/s^2 (negative rises)
	double drag;		// velocity damping per second
	double lifeMin, lifeMax;
	double sizeStart, sizeEnd;
	double alpha;
	double brightness;	// emissive intensity, or albedo scale for lit particles
	double stretch;		// streak length in seconds of motion (0: round)
	double delay;		// random start delay up to this many seconds
};

void FX_SpawnScripted(FLevelLocals *level, const FFXParticleParams &params);
// LevelLocals.SpawnFXParticles: FX_SpawnScripted plus the FXF_NoDistanceScale flag.
void FX_SpawnScriptedParticles(FLevelLocals *level, const FFXParticleParams &params);
