
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Analytic single scattering from point lights in a homogeneous medium.
// For each light the inverse-square in-scattering along the view ray up to the
// visible surface has a closed form, so no ray marching is needed.

layout(binding=0) uniform sampler2D DepthTexture;

void main()
{
	vec2 uv = TexCoord;
	float z = textureLod(DepthTexture, uv, 0.0).x;
	vec3 p = vec3((UVToViewA * uv + UVToViewB) * z, z);
	float dist = length(p);
	vec3 dir = p / max(dist, 0.001);

	vec3 sum = vec3(0.0);
	for (int i = 0; i < LightCount; i++)
	{
		vec4 lp = Lights[i * 2];
		vec4 lc = Lights[i * 2 + 1];
		float tc = dot(lp.xyz, dir);
		float h2 = max(dot(lp.xyz, lp.xyz) - tc * tc, 0.0);
		float radius = lp.w;
		if (h2 >= radius * radius) continue;

		float core = lc.w;
		float k = sqrt(core * core + h2);
		float scatter = (atan((dist - tc) / k) - atan(-tc / k)) * core / k;

		// Fade toward the light's radius, and quickly when the light is behind
		// the visible surface so it does not shine through walls.
		float window = 1.0 - sqrt(h2) / radius;
		window *= window;
		if (tc > dist) window *= exp(-(tc - dist) / (core * 0.5));
		sum += lc.rgb * scatter * window;
	}
	FragColor = vec4(sum * Strength * 0.05, 1.0);
}
