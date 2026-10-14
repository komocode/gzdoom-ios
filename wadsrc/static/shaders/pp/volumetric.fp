layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Single scattering from dynamic lights in a participating medium: thin air,
// the camera sector's coloured fog and an exponential height fog. Everything
// is in GL world space (x, height, y) along the view ray to the visible
// surface.
//
// Unshadowed point lights use the closed-form inverse-square integral. Spot
// lights and shadowed lights are ray marched inside the part of the ray the
// light can reach (sphere, cone), with equiangular, per-pixel jittered
// samples that TAA and the following blur integrate. Visibility comes from a ray traced
// against the scene (USE_RAYTRACE) or from the 1D shadow maps.
//
// The output is premultiplied: rgb is the in-scattered light plus the height
// fog colour, alpha the height fog opacity over the scene.

#if defined(FULL_RESOLUTION)
#if defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS DepthTexture;
layout(binding=2) uniform sampler2DMS ColorTexture;
#else
layout(binding=0) uniform sampler2D DepthTexture;
layout(binding=2) uniform sampler2D ColorTexture;
#endif
#else
layout(binding=0) uniform sampler2D DepthTexture;	// linear depth
#endif
layout(binding=1) uniform sampler2D ShadowMap;

// Four vec4s per light:
//   position, radius
//   colour, core size of the inverse-square falloff
//   spot beam direction (zero for point lights), unused
//   cos outer angle, cos inner angle, shadow map row (< 0: none), shadowed (> 0)
layout(std430, binding=10) readonly buffer VolumetricLightBuffer
{
	vec4 lights[];
};

#ifdef USE_RAYTRACE
layout(binding=11) uniform accelerationStructureEXT TopLevelAS;
#endif

// Scattering of 1 / MEDIUM_SCALE per map unit counts as much as the thin air
// the pass assumes everywhere.
#define MEDIUM_SCALE 1000.0

// Besides the inverse-square halo around its core, each light fills its
// sphere with a soft glow that fades to its radius, so that shafts stay
// visible away from the light. Large lights glow more than small ones such
// as pickups.
#define SPHERE_GLOW 8.0

float SphereGlow(float radius)
{
	return SPHERE_GLOW * clamp(radius / 768.0, 0.25, 1.5) / radius;
}

float SphereFalloff(float d, float radius)
{
	float w = max(1.0 - d / radius, 0.0);
	return w * w;
}

#if defined(FULL_RESOLUTION)
float normalizeDepth(float depth)
{
	float normalizedDepth = clamp(InverseDepthRangeA * depth + InverseDepthRangeB, 0.0, 1.0);
	return 1.0 / (normalizedDepth * LinearizeDepthA + LinearizeDepthB);
}

float SceneDepth(vec2 uv)
{
	vec2 sceneUV = Offset + uv * Scale;
#if defined(MULTISAMPLE)
	ivec2 ipos = ivec2(max(sceneUV * vec2(textureSize(DepthTexture)), vec2(0.0)));
	return normalizeDepth(texelFetch(ColorTexture, ipos, SampleIndex).a != 0.0 ? texelFetch(DepthTexture, ipos, SampleIndex).x : 1.0);
#else
	ivec2 ipos = ivec2(max(sceneUV * vec2(textureSize(DepthTexture, 0)), vec2(0.0)));
	return normalizeDepth(texelFetch(ColorTexture, ipos, 0).a != 0.0 ? texelFetch(DepthTexture, ipos, 0).x : 1.0);
#endif
}
#else
float SceneDepth(vec2 uv)
{
	return textureLod(DepthTexture, uv, 0.0).x;
}
#endif

//===========================================================================
//
// Medium
//
//===========================================================================

float Hash(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float ValueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(Hash(i), Hash(i + vec3(1.0, 0.0, 0.0)), f.x);
	float b = mix(Hash(i + vec3(0.0, 1.0, 0.0)), Hash(i + vec3(1.0, 1.0, 0.0)), f.x);
	float c = mix(Hash(i + vec3(0.0, 0.0, 1.0)), Hash(i + vec3(1.0, 0.0, 1.0)), f.x);
	float d = mix(Hash(i + vec3(0.0, 1.0, 1.0)), Hash(i + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

// Rolling mist: two octaves of value noise scrolling with the wind, flattened
// vertically, with a mean of about 1.
float MistNoise(vec3 p)
{
	if (FogColor.w <= 0.0) return 1.0;
	vec3 q = p * vec3(1.0, 2.5, 1.0) * FogNoise.w + FogNoise.xyz;
	float n = ValueNoise(q) * 0.65 + ValueNoise(q * 2.03 + vec3(17.0, 5.0, 11.0)) * 0.35;
	return mix(1.0, n * 2.0, FogColor.w);
}

float HeightDensity(float y)
{
	return FogParams.z * exp(-max(y - FogParams.x, 0.0) * FogParams.y);
}

// Integral of the (noise-free) height fog density over t in [0, len] along a
// ray starting h0 above the fog base with vertical direction dy. Below the
// base the density is constant.
float HeightOpticalDepth(float h0, float dy, float len)
{
	float density = FogParams.z;
	if (density <= 0.0) return 0.0;
	float k = FogParams.y;
	if (abs(dy) * k * len < 1e-3)
		return density * exp(-max(h0 + dy * len * 0.5, 0.0) * k) * len;

	float tBase = clamp(-h0 / dy, 0.0, len);
	float below, a0, a1;
	if (dy > 0.0)
	{
		below = tBase;
		a0 = tBase;
		a1 = len;
	}
	else
	{
		below = len - tBase;
		a0 = 0.0;
		a1 = tBase;
	}
	float above = (exp(-k * max(h0 + dy * a0, 0.0)) - exp(-k * max(h0 + dy * a1, 0.0))) / (k * dy);
	return density * (below + above);
}

vec3 Camera;
vec3 Dir;

// Light reaching the camera from distance t along the view ray.
float Transmittance(float t)
{
	return exp(-(HeightOpticalDepth(Camera.y - FogParams.x, Dir.y, t) + FogParams.w * t));
}

// How much the medium at p scatters, relative to thin air.
float Medium(vec3 p)
{
	float density = FogParams.w;
	if (FogParams.z > 0.0) density += HeightDensity(p.y) * MistNoise(p);
	return 1.0 + FogScatter * density * MEDIUM_SCALE;
}

//===========================================================================
//
// Light visibility
//
//===========================================================================

#ifdef USE_RAYTRACE

float Visibility(vec3 p, vec3 lightPos, float shadowRow)
{
	// The acceleration structure is in map space (x, y, height).
	vec3 origin = p.xzy;
	vec3 delta = lightPos.xzy + 0.01 - origin;
	float len = length(delta);
	if (len < 2.0) return 1.0;

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01, delta / len, len - 1.0);
	while (rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 0.0 : 1.0;
}

#else

float ShadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

// The 1D shadow maps hold, per light and direction in the map plane, the
// squared distance to the nearest one-sided line.
float Visibility(vec3 p, vec3 lightPos, float shadowRow)
{
	if (shadowRow < 0.0) return 1.0;
	vec2 dir = p.xz - lightPos.xz + 0.01;
	float dist2 = dot(dir, dir);
	if (dist2 < 1.0) return 1.0;
	float v = (shadowRow + 0.5) / 1024.0;
	return step(dist2, texture(ShadowMap, vec2(ShadowDirToU(dir), v)).x);
}

#endif

//===========================================================================
//
// Spot cone
//
//===========================================================================

bool InsideCone(vec3 w, vec3 axis, float cosOuter)
{
	return dot(w, axis) >= cosOuter * length(w);
}

// Clips [seg.x, seg.y] along the view ray to the light's cone (the root of
// the cone equation; the mirrored half of the double cone is rejected by the
// midpoint test).
vec2 ConeSegment(vec3 apex, vec3 axis, float cosOuter, vec2 seg)
{
	if (cosOuter <= 0.0) return seg;

	vec3 v = Camera - apex;
	float da = dot(Dir, axis);
	float va = dot(v, axis);
	float c2 = cosOuter * cosOuter;
	float A = da * da - c2;
	float B = 2.0 * (da * va - c2 * dot(Dir, v));
	float C = va * va - c2 * dot(v, v);

	float q0 = seg.x, q1 = seg.x;
	float disc = B * B - 4.0 * A * C;
	if (disc >= 0.0 && abs(A) > 1e-6)
	{
		float s = sqrt(disc);
		float r0 = (-B - s) / (2.0 * A);
		float r1 = (-B + s) / (2.0 * A);
		q0 = clamp(min(r0, r1), seg.x, seg.y);
		q1 = clamp(max(r0, r1), seg.x, seg.y);
	}

	float lo = 1e30, hi = -1e30;
	if (q0 > seg.x && InsideCone(v + Dir * (0.5 * (seg.x + q0)), axis, cosOuter)) { lo = seg.x; hi = q0; }
	if (q1 > q0 && InsideCone(v + Dir * (0.5 * (q0 + q1)), axis, cosOuter)) { lo = min(lo, q0); hi = q1; }
	if (seg.y > q1 && InsideCone(v + Dir * (0.5 * (q1 + seg.y)), axis, cosOuter)) { lo = min(lo, q1); hi = seg.y; }
	return vec2(lo, hi);
}

//===========================================================================

void main()
{
	vec2 uv = TexCoord;
	float z = SceneDepth(uv);
	vec3 viewPos = vec3((UVToViewA * uv + UVToViewB) * z, z);
	float dist = min(length(viewPos), MaxDistance);
	vec3 viewDir = normalize(viewPos);
	Camera = vec3(ViewX.w, ViewY.w, ViewZ.w);
	Dir = normalize(ViewX.xyz * viewDir.x + ViewY.xyz * viewDir.y + ViewZ.xyz * viewDir.z);

	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + JitterOffset);

	// Height fog over the scene.
	float fogDepth = 0.0;
	if (FogParams.z > 0.0)
	{
		if (FogColor.w > 0.0)
		{
			// Rolling mist: march the part of the ray inside the fog layer.
			float top = FogParams.x + 6.0 / FogParams.y;
			float t0 = 0.0, t1 = dist;
			if (abs(Dir.y) > 1e-5)
			{
				float tTop = (top - Camera.y) / Dir.y;
				if (Dir.y > 0.0) t1 = min(t1, tTop);
				else t0 = max(t0, tTop);
			}
			else if (Camera.y > top)
			{
				t1 = 0.0;
			}
			if (t1 > t0)
			{
				const int steps = 8;
				float dt = (t1 - t0) / float(steps);
				for (int i = 0; i < steps; i++)
				{
					vec3 p = Camera + Dir * (t0 + (float(i) + jitter) * dt);
					fogDepth += HeightDensity(p.y) * MistNoise(p);
				}
				fogDepth *= dt;
			}
		}
		else
		{
			fogDepth = HeightOpticalDepth(Camera.y - FogParams.x, Dir.y, dist);
		}
	}
	float fogAlpha = 1.0 - exp(-fogDepth);

	vec3 sum = vec3(0.0);
	for (int i = 0; i < LightCount; i++)
	{
		vec4 lightPos = lights[i * 4];
		vec4 lightColor = lights[i * 4 + 1];
		vec3 spotDir = lights[i * 4 + 2].xyz;
		vec4 lightParams = lights[i * 4 + 3];

		vec3 L = lightPos.xyz - Camera;
		float tc = dot(L, Dir);
		float h2 = max(dot(L, L) - tc * tc, 0.0);
		float radius = lightPos.w;
		if (h2 >= radius * radius) continue;

		float halfChord = sqrt(radius * radius - h2);
		vec2 seg = vec2(max(tc - halfChord, 0.0), min(tc + halfChord, dist));
		if (seg.y <= seg.x) continue;

		float core = lightColor.w;
		bool spot = dot(spotDir, spotDir) > 0.0;
		bool shadowed = lightParams.w > 0.0;
		if (!spot && !shadowed)
		{
			float k = sqrt(core * core + h2);
			float scatter = (atan((seg.y - tc) / k) - atan((seg.x - tc) / k)) * core / k;
			float window = 1.0 - sqrt(h2) / radius;
			// Simpson's rule for the sphere glow.
			float tm = 0.5 * (seg.x + seg.y);
			float glow = (SphereFalloff(sqrt(h2 + (seg.x - tc) * (seg.x - tc)), radius) + 4.0 * SphereFalloff(sqrt(h2 + (tm - tc) * (tm - tc)), radius) +
				SphereFalloff(sqrt(h2 + (seg.y - tc) * (seg.y - tc)), radius)) * (seg.y - seg.x) / 6.0 * SphereGlow(radius);
			float tp = clamp(tc, seg.x, seg.y);
			sum += lightColor.rgb * ((scatter * window * window + glow) * Medium(Camera + Dir * tp) * Transmittance(tp));
			continue;
		}

		if (spot)
		{
			seg = ConeSegment(lightPos.xyz, spotDir, lightParams.x, seg);
			if (seg.y <= seg.x) continue;
		}

		// Equiangular sampling: the samples are spread evenly over the angle
		// the segment subtends at the light, so they gather where the light
		// is strongest instead of being wasted at the far ends.
		float h = max(sqrt(h2), 1.0);
		float thetaA = atan((seg.x - tc) / h);
		float thetaB = atan((seg.y - tc) / h);
		float dTheta = (thetaB - thetaA) / float(Samples);
		float acc = 0.0;
		for (int s = 0; s < Samples; s++)
		{
			float theta = thetaA + (float(s) + jitter) * dTheta;
			float t = tc + h * tan(theta);
			float dt = (h * h + (t - tc) * (t - tc)) / h * dTheta;	// 1 / pdf
			vec3 p = Camera + Dir * t;
			vec3 toLight = lightPos.xyz - p;
			float d2 = dot(toLight, toLight);
			float d = sqrt(d2);
			float window = 1.0 - d / radius;
			if (window <= 0.0) continue;
			float a = (core / (core * core + d2) + SphereGlow(radius)) * window * window;
			if (spot) a *= smoothstep(lightParams.x, lightParams.y, dot(-toLight, spotDir) / max(d, 0.001));
			if (a <= 0.0) continue;
			if (shadowed) a *= Visibility(p, lightPos.xyz, lightParams.z);
			acc += a * Medium(p) * Transmittance(t) * dt;
		}
		sum += lightColor.rgb * acc;
	}

	FragColor = vec4(FogColor.rgb * fogAlpha + sum * (Strength * 0.25), fogAlpha);
}
