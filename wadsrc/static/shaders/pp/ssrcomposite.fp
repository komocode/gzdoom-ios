
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Blends the half-resolution reflections into the scene. The reflectivity is
// read at full resolution, so puddle edges stay sharp, and pixels covered by
// translucent surfaces or sprites (marked in the G-buffer) are left alone.

layout(binding=0) uniform sampler2D ReflectionTexture;
#if defined(MULTISAMPLE)
layout(binding=1) uniform sampler2DMS FogTexture;
#define FETCH(tex, uv) texelFetch(tex, ivec2((Offset + (uv) * Scale) * vec2(textureSize(tex))), SampleIndex)
#else
layout(binding=1) uniform sampler2D FogTexture;
#define FETCH(tex, uv) texelFetch(tex, ivec2((Offset + (uv) * Scale) * vec2(textureSize(tex, 0))), 0)
#endif

void main()
{
	float b = floor(FETCH(FogTexture, TexCoord).a * 255.0 + 0.5);
	float reflectivity = b >= 128.0 ? floor((255.0 - b) / 8.0) / 15.0 : 0.0;
	vec4 r = texture(ReflectionTexture, TexCoord);
	if (reflectivity <= 0.0 || r.a <= 0.001)
	{
		FragColor = vec4(0.0);
		return;
	}
	FragColor = vec4(r.rgb / r.a, clamp(r.a * reflectivity * Strength, 0.0, 1.0));
}
