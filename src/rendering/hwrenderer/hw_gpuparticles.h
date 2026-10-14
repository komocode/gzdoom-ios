#pragma once

#include "vectors.h"

class AActor;
struct HWDrawInfo;
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
// Particles come to rest on the floor they were spawned above, sparks and
// tracers stretch along their screen-space velocity, and smoke and dust take the
// colour of the surface that was hit.

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
