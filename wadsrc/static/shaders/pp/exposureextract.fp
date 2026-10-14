
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D SceneTexture;

void main()
{
	vec4 color = texture(SceneTexture, Offset + TexCoord * Scale);
#if defined(LOGLUMINANCE)
	// Eye adaptation averages the log of the linear luminance (a geometric mean),
	// so a few bright lamps don't darken the whole view.
	vec3 linearColor = max(color.rgb, vec3(0.0));
	linearColor *= linearColor; // the scene is gamma 2.0 encoded
	float luminance = dot(linearColor, vec3(0.2126, 0.7152, 0.0722));
	FragColor = vec4(log2(max(luminance, 1.0 / 4096.0)), 0.0, 0.0, 1.0);
#else
	FragColor = vec4(max(max(color.r, color.g), color.b), 0.0, 0.0, 1.0);
#endif
}
