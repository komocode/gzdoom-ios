
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Screen-space reflections. Runs at the linear depth resolution and outputs
// the reflected color with its blend weight in alpha.

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

void main()
{
	vec2 uv = TexCoord;
	float marker = FETCH(FogTexture, uv).a;
	float reflectivity = (1.0 - marker) * 2.0;
	if (marker < 0.5 || reflectivity < 0.01)
	{
		FragColor = vec4(0.0);
		return;
	}

	vec3 n = FETCH(NormalTexture, uv).xyz * 2.0 - 1.0;
	if (dot(n, n) < 0.01)
	{
		FragColor = vec4(0.0);
		return;
	}
	n = normalize(n);
	n.z = -n.z;

	vec3 p = ViewPos(uv);
	vec3 v = normalize(p);
	vec3 r = reflect(v, n);

	float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
	float stepLen = MaxDistance / float(Steps);
	vec3 origin = p + n * 1.5;
	float t = stepLen * (0.5 + jitter);
	float prevT = 0.0;
	bool hit = false;
	vec2 hitUV = uv;

	for (int i = 0; i < Steps; i++)
	{
		vec3 s = origin + r * t;
		if (s.z < 1.0) break;
		vec2 suv = Project(s);
		if (!Inside(suv)) break;
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

	if (!hit)
	{
		FragColor = vec4(0.0);
		return;
	}

	vec3 color = FETCH(ColorTexture, hitUV).rgb;
	float edge = smoothstep(0.0, 0.1, min(min(hitUV.x, 1.0 - hitUV.x), min(hitUV.y, 1.0 - hitUV.y)));
	float distanceFade = 1.0 - smoothstep(0.6, 1.0, t / (MaxDistance * 1.8));
	float fresnel = 0.35 + 0.65 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 4.0);
	float weight = clamp(reflectivity * Strength * edge * distanceFade * fresnel, 0.0, 1.0);
	FragColor = vec4(color, weight);
}
