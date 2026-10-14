
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;

// Contrast-adaptive sharpening (after AMD FidelityFX CAS), vignette and grain.

float Hash(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

void main()
{
	ivec2 size = textureSize(InputTexture, 0);
	ivec2 ip = ivec2(TexCoord * vec2(size));
	vec3 c = texelFetch(InputTexture, ip, 0).rgb;

	if (Sharpen > 0.0)
	{
		vec3 n = texelFetch(InputTexture, clamp(ip + ivec2(0, -1), ivec2(0), size - 1), 0).rgb;
		vec3 s = texelFetch(InputTexture, clamp(ip + ivec2(0, 1), ivec2(0), size - 1), 0).rgb;
		vec3 e = texelFetch(InputTexture, clamp(ip + ivec2(1, 0), ivec2(0), size - 1), 0).rgb;
		vec3 w = texelFetch(InputTexture, clamp(ip + ivec2(-1, 0), ivec2(0), size - 1), 0).rgb;
		vec3 mn = min(c, min(min(n, s), min(e, w)));
		vec3 mx = max(c, max(max(n, s), max(e, w)));
		vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
		vec3 wgt = amp * (-1.0 / mix(8.0, 5.0, Sharpen));
		c = clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, MaxValue);
	}

	if (Vignette > 0.0)
	{
		vec2 q = TexCoord - 0.5;
		q.x *= Aspect / 1.6;
		c *= 1.0 - Vignette * smoothstep(0.25, 0.9, length(q) * 1.4);
	}

	if (Grain > 0.0)
	{
		// Triangular noise also removes banding in dark gradients.
		vec2 seed = gl_FragCoord.xy + fract(Time * 13.0) * 117.0;
		float noise = Hash(seed) + Hash(seed + 71.3) - 1.0;
		float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
		c += noise * Grain * mix(0.02, 0.045, 1.0 - abs(lum * 2.0 - 1.0));
	}

	FragColor = vec4(c, 1.0);
}
