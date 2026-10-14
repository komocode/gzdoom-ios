
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Temporal anti-aliasing resolve. The scene was rendered with a subpixel
// jitter; last frame's result is reprojected through the depth buffer and the
// camera motion, clipped to this frame's 3x3 neighbourhood colour range (which
// rejects stale history from moving sprites and disocclusions) and blended.

layout(binding=0) uniform sampler2D InputTexture;
layout(binding=1) uniform sampler2D HistoryTexture;
layout(binding=2) uniform sampler2D DepthTexture;

vec3 ToYCoCg(vec3 c)
{
	return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 FromYCoCg(vec3 c)
{
	return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main()
{
	vec2 uv = TexCoord;
	vec3 current = textureLod(InputTexture, uv, 0.0).rgb;
	vec2 s = (uv - Offset) / Scale;
	if (Feedback <= 0.0 || any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0))))
	{
		FragColor = vec4(current, 1.0);
		return;
	}

	// Neighbourhood statistics, and the nearest depth so edges reproject with
	// the foreground.
	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	float depth = 1.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec2 tuv = uv + vec2(x, y) * InvSize;
			vec3 c = ToYCoCg(textureLod(InputTexture, tuv, 0.0).rgb);
			m1 += c;
			m2 += c * c;
			depth = min(depth, textureLod(DepthTexture, tuv, 0.0).x);
		}
	}

	float z = 1.0 / (clamp(depth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
	vec2 su = s - Jitter;
	vec3 viewPos = vec3((UVToViewA * su + UVToViewB) * z, z);
	vec4 clip = Reprojection * vec4(viewPos.xy, -viewPos.z, 1.0);
	vec2 prev = clip.xy / clip.w * 0.5 + 0.5;
	if (clip.w <= 0.0 || any(lessThan(prev, vec2(0.0))) || any(greaterThan(prev, vec2(1.0))))
	{
		FragColor = vec4(current, 1.0);
		return;
	}

	vec3 history = textureLod(HistoryTexture, Offset + prev * Scale, 0.0).rgb;

	// Variance clipping in YCoCg.
	vec3 mean = m1 / 9.0;
	vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
	vec3 lo = mean - 1.25 * sigma;
	vec3 hi = mean + 1.25 * sigma;
	vec3 h = ToYCoCg(history);
	vec3 center = (lo + hi) * 0.5;
	vec3 extent = max((hi - lo) * 0.5, vec3(1e-4));
	vec3 d = h - center;
	vec3 ratio = abs(d / extent);
	float maxRatio = max(ratio.x, max(ratio.y, ratio.z));
	if (maxRatio > 1.0)
		h = center + d / maxRatio;
	history = FromYCoCg(h);

	// Fast motion keeps less history so it stays sharp.
	float pixels = length((s - prev) * vec2(1.0 / InvSize.x, 1.0 / InvSize.y) * Scale);
	float feedback = Feedback * mix(1.0, 0.8, clamp(pixels / 16.0, 0.0, 1.0));

	// Luminance-weighted blend so bright HDR pixels don't dominate (no fireflies).
	float wc = (1.0 - feedback) / (1.0 + dot(current, vec3(0.299, 0.587, 0.114)));
	float wh = feedback / (1.0 + dot(history, vec3(0.299, 0.587, 0.114)));
	FragColor = vec4((current * wc + history * wh) / max(wc + wh, 1e-5), 1.0);
}
