
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Screen-space reflections. Runs at half the 3D viewport's resolution after
// the translucent pass, so the colour it reflects includes sprites, glows and
// particles. Rough surfaces sample a blur chain of the scene by the width of
// the reflection cone, stretched along the surface normal's direction on
// screen (vertically on floors). Rays that leave the screen or find nothing
// fall back to the blurred screen near where they left it (the sky above
// outdoor floors). Outputs the colour premultiplied by its weight; the
// composite applies the surface's reflectivity.

layout(binding=0) uniform sampler2D DepthTexture;
#if defined(MULTISAMPLE)
layout(binding=1) uniform sampler2DMS NormalTexture;
layout(binding=2) uniform sampler2DMS FogTexture;
layout(binding=3) uniform sampler2DMS ColorTexture;
#define FETCH(tex, uv) texelFetch(tex, ivec2((Offset + (uv) * Scale) * vec2(textureSize(tex))), SampleIndex)
#else
layout(binding=1) uniform sampler2D NormalTexture;
layout(binding=2) uniform sampler2D FogTexture;
layout(binding=3) uniform sampler2D ColorTexture;
#define FETCH(tex, uv) texelFetch(tex, ivec2((Offset + (uv) * Scale) * vec2(textureSize(tex, 0))), 0)
#endif
layout(binding=4) uniform sampler2D Level0;
layout(binding=5) uniform sampler2D Level1;
layout(binding=6) uniform sampler2D Level2;
layout(binding=7) uniform sampler2D Level3;
layout(binding=8) uniform sampler2D Level4;

vec3 ViewPos(vec2 uv)
{
	float z = textureLod(DepthTexture, uv, 0.0).x;
	return vec3((UVToViewA * uv + UVToViewB) * z, z);
}

vec2 Project(vec3 p)
{
	return (p.xy / p.z - UVToViewB) / UVToViewA;
}

bool Inside(vec2 uv)
{
	return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Level 0 is the full-resolution scene, levels 1-5 the blur chain.
vec3 SampleLevel(int level, vec2 uv)
{
	if (level <= 0)
	{
#if defined(MULTISAMPLE)
		return FETCH(ColorTexture, uv).rgb;
#else
		return textureLod(ColorTexture, Offset + uv * Scale, 0.0).rgb;
#endif
	}
	if (level == 1) return textureLod(Level0, uv, 0.0).rgb;
	if (level == 2) return textureLod(Level1, uv, 0.0).rgb;
	if (level == 3) return textureLod(Level2, uv, 0.0).rgb;
	if (level == 4) return textureLod(Level3, uv, 0.0).rgb;
	return textureLod(Level4, uv, 0.0).rgb;
}

vec3 SampleBlur(vec2 uv, float lod)
{
	uv = clamp(uv, vec2(0.0), vec2(1.0));
	float l = clamp(lod, 0.0, 5.0);
	int l0 = int(floor(l));
	float f = l - float(l0);
	vec3 a = SampleLevel(l0, uv);
	if (f < 0.01) return a;
	return mix(a, SampleLevel(min(l0 + 1, 5), uv), f);
}

// Five taps along the streak direction, Gaussian weighted.
vec3 SampleStreak(vec2 uv, float lod, vec2 dir, float len)
{
	if (len < InvSize.y) return SampleBlur(uv, lod);
	vec3 sum = vec3(0.0);
	float wsum = 0.0;
	for (int k = -2; k <= 2; k++)
	{
		float t = float(k) * 0.5;
		float w = exp(-2.0 * t * t);
		sum += SampleBlur(uv + dir * (len * t), lod) * w;
		wsum += w;
	}
	return sum / wsum;
}

void main()
{
	vec2 uv = TexCoord;
	float b = floor(FETCH(FogTexture, uv).a * 255.0 + 0.5);
	float reflectivity = b >= 128.0 ? floor((255.0 - b) / 8.0) / 15.0 : 0.0;
	if (reflectivity <= 0.0)
	{
		FragColor = vec4(0.0);
		return;
	}
	float roughness = mod(255.0 - b, 8.0) / 7.0;

	vec3 n = FETCH(NormalTexture, uv).xyz * 2.0 - 1.0;
	if (dot(n, n) < 0.01)
	{
		FragColor = vec4(0.0);
		return;
	}
	n = normalize(n);
	n.z = -n.z;

	vec3 p = ViewPos(uv);
	if (p.z >= FarDepth * 0.9)
	{
		FragColor = vec4(0.0);
		return;
	}
	vec3 v = normalize(p);
	vec3 geomN = n;

	// While TAA accumulates, spread the rays over the roughness lobe with a
	// different pattern every frame.
	float noise = Hash(gl_FragCoord.xy + vec2(Jitter * 7.13, Jitter * 3.71));
	if (Jitter > 0.0 && roughness > 0.05)
	{
		vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
		vec3 bt = cross(n, t);
		float noise2 = Hash(gl_FragCoord.yx + vec2(Jitter * 5.31, Jitter * 1.93));
		vec3 bent = normalize(n + (t * (noise - 0.5) + bt * (noise2 - 0.5)) * (roughness * roughness * 0.5));
		if (dot(bent, -v) > 0.05) n = bent;
	}
	vec3 r = reflect(v, n);

	float facing = clamp(dot(-v, geomN), 0.0, 1.0);
	float fresnel = 0.35 + 0.65 * pow(1.0 - facing, 4.0);

	// Streak direction: the surface normal as seen on screen (vertical on floors).
	vec2 streakDir = Project(p + geomN * 16.0) - uv;
	streakDir = dot(streakDir, streakDir) > 1e-8 ? normalize(streakDir) : vec2(0.0, 1.0);
	float stretch = 1.0 + Stretch * (1.0 - facing);
	float coneTan = roughness * roughness * 0.4;

	float stepLen = MaxDistance / float(Steps);
	vec3 origin = p + geomN * 1.5;
	float t = stepLen * (0.5 + (Jitter > 0.0 ? noise : Hash(gl_FragCoord.xy)));
	float prevT = 0.0;
	bool hit = false;
	vec2 hitUV = uv;
	vec2 lastUV = uv;

	for (int i = 0; i < Steps; i++)
	{
		vec3 s = origin + r * t;
		if (s.z < 1.0) break;
		vec2 suv = Project(s);
		if (!Inside(suv)) break;
		lastUV = suv;
		float sceneZ = textureLod(DepthTexture, suv, 0.0).x;
		float diff = s.z - sceneZ;
		if (diff > 0.0 && diff < Thickness + t * 0.04)
		{
			// Refine the intersection with a short binary search.
			float lo = prevT, hi = t;
			for (int j = 0; j < 5; j++)
			{
				float mid = (lo + hi) * 0.5;
				vec3 m = origin + r * mid;
				vec2 muv = Project(m);
				if (m.z - textureLod(DepthTexture, muv, 0.0).x > 0.0) hi = mid; else lo = mid;
			}
			t = hi;
			hitUV = Project(origin + r * t);
			hit = Inside(hitUV);
			break;
		}
		prevT = t;
		t += stepLen * (1.0 + float(i) * 0.08);
	}

	// Fallback for rays that miss: the sky where the ray left the view, or
	// the heavily blurred screen there mixed with this spot's surroundings.
	vec2 exitUV = clamp(lastUV, vec2(0.0), vec2(1.0));
	bool sky = textureLod(DepthTexture, exitUV, 0.0).x >= FarDepth * 0.9;
	float skyLod = 1.0 + roughness * 4.0;
	vec3 fallback = sky ? SampleStreak(exitUV, skyLod, streakDir, 0.04 * roughness * stretch)
		: mix(SampleBlur(exitUV, 5.0), SampleBlur(uv, 5.0), 0.5);
	float fallbackWeight = sky ? 1.0 : 0.6;

	vec3 color;
	float weight;
	if (hit)
	{
		float hitZ = max(textureLod(DepthTexture, hitUV, 0.0).x, 1.0);
		// Cone footprint at the hit in UV units, and the matching blur level.
		float radius = t * coneTan / (hitZ * UVToViewA.y);
		float lod = log2(max(radius * LevelHeight * 2.0, 1.0));
		color = SampleStreak(hitUV, lod, streakDir, radius * stretch);
		float edge = smoothstep(0.0, 0.08, min(min(hitUV.x, 1.0 - hitUV.x), min(hitUV.y, 1.0 - hitUV.y)));
		float distanceFade = 1.0 - smoothstep(0.6, 1.0, t / (MaxDistance * 1.8));
		float blend = edge * distanceFade;
		color = mix(fallback, color, blend);
		weight = mix(fallbackWeight, 1.0, blend);
	}
	else
	{
		color = fallback;
		weight = fallbackWeight;
	}

	weight *= fresnel;
	FragColor = vec4(color * weight, weight);
}
