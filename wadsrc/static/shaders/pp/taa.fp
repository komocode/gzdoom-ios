
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Temporal anti-aliasing resolve. The scene was rendered with a subpixel
// jitter; last frame's result is reprojected through the depth buffer and the
// camera motion, clipped to this frame's 3x3 neighbourhood colour range (which
// rejects stale history from moving sprites and disocclusions) and blended.
//
// When upscaling, the input covers a smaller part of the buffers (InputScale,
// InputOffset) than the output (Scale, Offset). Each output pixel is then
// reconstructed from the nearby input samples, weighted by their distance to
// the pixel after removing this frame's jitter, and the history is trusted more
// where no sample landed close to the pixel.

layout(binding=0) uniform sampler2D InputTexture;
layout(binding=1) uniform sampler2D HistoryTexture;
layout(binding=2) uniform sampler2D DepthTexture;
layout(binding=3) uniform sampler2D ReactiveTexture; // scene normals; alpha is the reactive mask

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
	vec2 s = (uv - Offset) / Scale;

	// Input texel position of this output pixel in the jittered input image.
	vec2 inUV = Upscale > 0.5 ? InputOffset + (s + Jitter) * InputScale : uv;
	vec2 texelPos = inUV / InvSize - 0.5;
	vec2 nearest = floor(texelPos + 0.5);

	// Neighbourhood statistics, the reconstructed current colour, and the
	// nearest depth so edges reproject with the foreground.
	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 sum = vec3(0.0);
	float weightSum = 0.0;
	float confidence = 0.0;
	float depth = 1.0;
	float reactive = 0.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec2 tuv = Upscale > 0.5 ? (nearest + vec2(x, y) + 0.5) * InvSize : uv + vec2(x, y) * InvSize;
			vec3 rgb = textureLod(InputTexture, tuv, 0.0).rgb;
			vec3 c = ToYCoCg(rgb);
			m1 += c;
			m2 += c * c;
			depth = min(depth, textureLod(DepthTexture, tuv, 0.0).x);
			if (Reactive > 0.5) reactive = max(reactive, textureLod(ReactiveTexture, tuv, 0.0).a);
			vec2 d = nearest + vec2(x, y) - texelPos;
			float w = exp(-2.3 * dot(d, d));
			sum += rgb * w;
			weightSum += w;
			confidence = max(confidence, w);
		}
	}
	vec3 current = Upscale > 0.5 ? sum / weightSum : textureLod(InputTexture, uv, 0.0).rgb;
	if (Upscale < 0.5) confidence = 1.0;

	if (Feedback <= 0.0 || any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0))))
	{
		FragColor = vec4(current, 1.0);
		return;
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
	// Reactive pixels (moving sprites, particles, liquids, translucency) have no
	// motion vectors of their own: clip their history tighter and keep less of it.
	float gamma = mix(1.25, 0.75, reactive);
	vec3 lo = mean - gamma * sigma;
	vec3 hi = mean + gamma * sigma;
	vec3 h = ToYCoCg(history);
	vec3 center = (lo + hi) * 0.5;
	vec3 extent = max((hi - lo) * 0.5, vec3(1e-4));
	vec3 d = h - center;
	vec3 ratio = abs(d / extent);
	float maxRatio = max(ratio.x, max(ratio.y, ratio.z));
	if (maxRatio > 1.0)
		h = center + d / maxRatio;
	history = FromYCoCg(h);

	// Fast motion keeps less history so it stays sharp. When upscaling, a
	// sample far from this pixel counts for less than one that hit it.
	float pixels = length((s - prev) * vec2(1.0 / InvSize.x, 1.0 / InvSize.y) * Scale);
	float feedback = Feedback * mix(1.0, 0.8, clamp(pixels / 16.0, 0.0, 1.0));
	feedback = 1.0 - (1.0 - feedback) * mix(0.25, 1.0, confidence);
	feedback *= 1.0 - 0.75 * reactive;

	// Luminance-weighted blend so bright HDR pixels don't dominate (no fireflies).
	float wc = (1.0 - feedback) / (1.0 + dot(current, vec3(0.299, 0.587, 0.114)));
	float wh = feedback / (1.0 + dot(history, vec3(0.299, 0.587, 0.114)));
	FragColor = vec4((current * wc + history * wh) / max(wc + wh, 1e-5), 1.0);
}
