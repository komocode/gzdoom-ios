// Visual-only GPU particles and weather. Nothing here touches game state or the
// game's random numbers, so it is safe in netplay and demos.

enum EWeatherType
{
	WEATHER_None,
	WEATHER_Rain,
	WEATHER_Snow,
};

// Built-in GPU particle shapes (FXParticleParams.shape)
enum EFXShape
{
	FXS_Glow,		// hot core, soft halo: sparks, embers
	FXS_Smoke,		// noisy soft puff: smoke, dust, steam, mist
	FXS_Chunk,		// hard-edged debris
	FXS_Droplet,	// dense drop with a glint: blood, water
	FXS_Ring,		// thin ring: foam, shock waves
	FXS_Mote,		// small soft dot that fades in and out: dust motes, pollen
};

// FXParticleParams.flags
enum EFXFlags
{
	FXF_Additive = 1,		// additive and emissive (sparks, embers, fire)
	FXF_Unlit = 2,			// alpha blended but not lit by the scene
	FXF_RestOnFloor = 4,	// land on the floor or 3D floor under the trajectory
	FXF_NoDistanceScale = 8,	// no thinning with camera distance, for effects seen from afar
};

// Emits visual-only GPU particles continuously (Rate per second) and/or in
// bursts (BurstCount every BurstInterval tics). Override SetupParticles to
// describe them. With a master it follows that actor at FollowOffset.
// Activate/Deactivate (or +DORMANT) switch it on and off.
class FXEmitter : Actor
{
	double Rate;
	int BurstCount;
	int BurstInterval;
	Vector3 FollowOffset;
	double Accumulator;

	property Rate: Rate;
	property Burst: BurstCount, BurstInterval;

	Default
	{
		+NOINTERACTION
		+NOBLOCKMAP
		+NOGRAVITY
		+DONTSPLASH
		+NOTONAUTOMAP
		RenderStyle "None";
		Radius 1;
		Height 1;
		FXEmitter.Rate 10;
		FXEmitter.Burst 0, 0;
	}

	// Pos, count and neutral defaults are filled in before this is called.
	virtual void SetupParticles(out FXParticleParams p) {}

	override void Activate(Actor activator)
	{
		bDormant = false;
	}

	override void Deactivate(Actor activator)
	{
		bDormant = true;
	}

	void Emit(int count)
	{
		FXParticleParams p;
		p.count = count;
		p.pos = Pos;
		p.color1 = Color(255, 255, 255, 255);
		p.speedMin = 0;
		p.speedMax = 0;
		p.drag = 1;
		p.lifeMin = 1;
		p.lifeMax = 1;
		p.sizeStart = 2;
		p.sizeEnd = 2;
		p.alpha = 1;
		p.brightness = 1;
		// Spread a tic's particles over the tic so continuous streams look smooth.
		p.delay = BurstInterval > 0 ? 0 : 1. / TICRATE;
		SetupParticles(p);
		Level.SpawnFXParticles(p);
	}

	override void Tick()
	{
		Super.Tick();
		if (bDormant || isFrozen()) return;
		if (master) SetOrigin(master.Vec3Offset(FollowOffset.X, FollowOffset.Y, FollowOffset.Z), true);

		Accumulator += Rate / TICRATE;
		int count = int(Accumulator);
		if (count > 0)
		{
			Accumulator -= count;
			Emit(count);
		}
		if (BurstInterval > 0 && BurstCount > 0 && (Level.maptime + int(SpawnPoint.X + SpawnPoint.Y)) % BurstInterval == 0)
		{
			Emit(BurstCount);
		}
	}
}

// Embers rising from a torch or brazier.
class FXTorchEmbers : FXEmitter
{
	Default
	{
		FXEmitter.Rate 14;
	}

	override void SetupParticles(out FXParticleParams p)
	{
		p.color1 = Color(255, 255, 140, 40);
		p.color2 = Color(255, 255, 70, 20);
		p.shape = FXS_Glow;
		p.flags = FXF_Additive;
		p.spread = (4, 4, 2);
		p.dir = (0, 0, 1);
		p.coneAngle = 30;
		p.speedMin = 15;
		p.speedMax = 45;
		p.gravity = -70;
		p.drag = 0.9;
		p.lifeMin = 0.8;
		p.lifeMax = 2.0;
		p.sizeStart = 0.9;
		p.sizeEnd = 0.3;
		p.brightness = 3;
		p.stretch = 0.02;
	}
}

// Dust motes drifting in the air, lit by nearby lights and light cones.
class FXDustMotes : FXEmitter
{
	Default
	{
		FXEmitter.Rate 8;
	}

	override void SetupParticles(out FXParticleParams p)
	{
		p.color1 = Color(255, 235, 225, 200);
		p.shape = FXS_Mote;
		p.spread = (96, 96, 48);
		p.speedMin = 1;
		p.speedMax = 5;
		p.gravity = -1;
		p.drag = 0.2;
		p.lifeMin = 4;
		p.lifeMax = 7;
		p.sizeStart = 0.7;
		p.sizeEnd = 0.7;
		p.alpha = 0.7;
		p.brightness = 1.4;
	}
}

// Steam rising from a vent or grate.
class FXSteamVent : FXEmitter
{
	Default
	{
		FXEmitter.Rate 16;
	}

	override void SetupParticles(out FXParticleParams p)
	{
		p.color1 = Color(255, 220, 222, 225);
		p.shape = FXS_Smoke;
		p.spread = (6, 6, 0);
		p.dir = (0, 0, 1);
		p.coneAngle = 12;
		p.speedMin = 50;
		p.speedMax = 90;
		p.gravity = -30;
		p.drag = 1.3;
		p.lifeMin = 1.4;
		p.lifeMax = 2.4;
		p.sizeStart = 4;
		p.sizeEnd = 24;
		p.alpha = 0.3;
		p.brightness = 1;
	}
}

// Sparks showering down from a broken cable or a grinder; they bounce to rest on the floor.
class FXSparkShower : FXEmitter
{
	Default
	{
		FXEmitter.Rate 30;
	}

	override void SetupParticles(out FXParticleParams p)
	{
		p.color1 = Color(255, 255, 220, 140);
		p.color2 = Color(255, 255, 160, 60);
		p.shape = FXS_Glow;
		p.flags = FXF_Additive | FXF_RestOnFloor;
		p.dir = (0, 0, -1);
		p.coneAngle = 75;
		p.speedMin = 60;
		p.speedMax = 180;
		p.gravity = 800;
		p.drag = 1.2;
		p.lifeMin = 0.4;
		p.lifeMax = 1.1;
		p.sizeStart = 0.8;
		p.sizeEnd = 0.25;
		p.brightness = 4;
		p.stretch = 0.03;
	}
}

// Sea spray: bursts of foam and mist thrown up where waves break.
class FXSeaSpray : FXEmitter
{
	Default
	{
		FXEmitter.Rate 0;
		FXEmitter.Burst 40, 70;
	}

	override void SetupParticles(out FXParticleParams p)
	{
		p.color1 = Color(255, 235, 242, 245);
		p.color2 = Color(255, 200, 215, 225);
		p.shape = FXS_Smoke;
		p.spread = (48, 48, 4);
		p.dir = (0, 0, 1);
		p.coneAngle = 35;
		p.speedMin = 80;
		p.speedMax = 220;
		p.gravity = 350;
		p.drag = 1.5;
		p.lifeMin = 1.0;
		p.lifeMax = 2.2;
		p.sizeStart = 3;
		p.sizeEnd = 16;
		p.alpha = 0.5;
		p.brightness = 1.1;
		p.delay = 0.3;
	}
}
