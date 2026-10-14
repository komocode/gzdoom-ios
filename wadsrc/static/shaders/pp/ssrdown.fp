
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Builds the blur chain that rough reflections sample: each level is a 4x4
// tent-filtered half-size copy of the previous one.

#if defined(FROM_SCENE) && defined(MULTISAMPLE)
layout(binding=0) uniform sampler2DMS InputTexture;
#else
layout(binding=0) uniform sampler2D InputTexture;
#endif

#if defined(FROM_SCENE) && defined(MULTISAMPLE)
vec3 Fetch(vec2 uv)
{
	ivec2 size = textureSize(InputTexture);
	return texelFetch(InputTexture, clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1), SampleIndex).rgb;
}
#else
vec3 Fetch(vec2 uv)
{
	return texture(InputTexture, uv).rgb;
}
#endif

void main()
{
	vec2 uv = Offset + TexCoord * Scale;
	vec2 t = InvSize;
	vec3 color = (Fetch(uv + t * vec2(-1.0, -1.0)) + Fetch(uv + t * vec2(1.0, -1.0)) +
		Fetch(uv + t * vec2(-1.0, 1.0)) + Fetch(uv + t * vec2(1.0, 1.0))) * 0.25;
	// Keep single very bright pixels from turning into flickering blobs.
	color = min(color, vec3(16.0));
	FragColor = vec4(color, 1.0);
}
