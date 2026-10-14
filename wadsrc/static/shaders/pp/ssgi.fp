
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Screen-space indirect light. A few short cosine-distributed rays per pixel
// gather the lit scene around each surface; the average is tinted by the
// surface's own color and added to the scene. Runs at half resolution and
// complements the baked probes with dynamic bounce from moving lights,
// muzzle flashes and explosions.

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
	// Cleared pixels (sky, void) have no surface.
	if (FETCH(FogTexture, uv).a < 0.5)
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
	vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
	vec3 b = cross(n, t);

	// Interleaved gradient noise rotates the ray set per pixel.
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	const int RayCount = 4;
	float stepLen = MaxDistance / float(Steps);

	vec3 sum = vec3(0.0);
	for (int i = 0; i < RayCount; i++)
	{
		float a = (float(i) + noise) * (6.2831853 / float(RayCount));
		float r = sqrt((float(i) + 0.5) / float(RayCount));
		vec3 dir = normalize(t * (cos(a) * r) + b * (sin(a) * r) + n * sqrt(1.0 - r * r));
		vec3 origin = p + n * 2.0;
		float dist = stepLen * (0.5 + noise);
		for (int s = 0; s < Steps; s++)
		{
			vec3 q = origin + dir * dist;
			if (q.z < 1.0) break;
			vec2 quv = Project(q);
			if (!Inside(quv)) break;
			float diff = q.z - textureLod(DepthTexture, quv, 0.0).x;
			if (diff > 0.0 && diff < Thickness)
			{
				// Closer bounces count more.
				sum += FETCH(ColorTexture, quv).rgb * (1.0 - dist / MaxDistance);
				break;
			}
			dist += stepLen;
		}
	}
	vec3 indirect = sum / float(RayCount);
	vec3 albedo = FETCH(ColorTexture, uv).rgb;
	// The scene color carries both albedo and light; normalizing keeps the hue
	// of the surface without counting its own light twice.
	albedo /= max(max(albedo.r, max(albedo.g, albedo.b)), 0.15);
	FragColor = vec4(indirect * albedo * Strength * 0.35, 1.0);
}
