
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;
#if defined(PREFILTER)
layout(binding=1) uniform sampler2D ExposureTexture;
#endif

// 13-tap downsample (Jimenez 2014): four overlapping 2x2 boxes plus a center
// box, weighted so the result is stable for small bright features.
vec3 Fetch(vec2 uv)
{
	return texture(InputTexture, Offset + uv * Scale).rgb;
}

void main()
{
	vec2 t = InvSize;
#if defined(PREFILTER)
	t *= 2.0; // the first level is a quarter of the scene size
#endif
	vec2 uv = TexCoord;
	vec3 a = Fetch(uv + t * vec2(-2.0, -2.0));
	vec3 b = Fetch(uv + t * vec2( 0.0, -2.0));
	vec3 c = Fetch(uv + t * vec2( 2.0, -2.0));
	vec3 d = Fetch(uv + t * vec2(-1.0, -1.0));
	vec3 e = Fetch(uv + t * vec2( 1.0, -1.0));
	vec3 f = Fetch(uv + t * vec2(-2.0,  0.0));
	vec3 g = Fetch(uv);
	vec3 h = Fetch(uv + t * vec2( 2.0,  0.0));
	vec3 i = Fetch(uv + t * vec2(-1.0,  1.0));
	vec3 j = Fetch(uv + t * vec2( 1.0,  1.0));
	vec3 k = Fetch(uv + t * vec2(-2.0,  2.0));
	vec3 l = Fetch(uv + t * vec2( 0.0,  2.0));
	vec3 m = Fetch(uv + t * vec2( 2.0,  2.0));
	vec3 color = (d + e + i + j) * 0.125
		+ (a + b + f + g) * 0.03125 + (b + c + g + h) * 0.03125
		+ (f + g + k + l) * 0.03125 + (g + h + l + m) * 0.03125;

#if defined(PREFILTER)
	// Scene colors are gamma 2.0 encoded; bloom works in linear light.
	color = color * color;
	float exposure = texture(ExposureTexture, vec2(0.5)).x;
	color *= exposure;
	float brightness = max(max(color.r, color.g), color.b);
	float soft = clamp(brightness - Threshold + Knee, 0.0, 2.0 * Knee);
	soft = soft * soft / (4.0 * Knee + 1e-4);
	float contribution = max(soft, brightness - Threshold) / max(brightness, 1e-4);
	color *= contribution;
	color = min(color, vec3(64.0));
#endif
	FragColor = vec4(color, 1.0);
}
