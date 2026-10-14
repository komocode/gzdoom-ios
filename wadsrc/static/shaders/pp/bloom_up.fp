
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;

// 3x3 tent filter upsample.
void main()
{
	vec2 t = InvSize;
	vec2 uv = TexCoord;
	vec3 color = texture(InputTexture, uv).rgb * 4.0;
	color += (texture(InputTexture, uv + vec2(-t.x, 0.0)).rgb + texture(InputTexture, uv + vec2(t.x, 0.0)).rgb
		+ texture(InputTexture, uv + vec2(0.0, -t.y)).rgb + texture(InputTexture, uv + vec2(0.0, t.y)).rgb) * 2.0;
	color += texture(InputTexture, uv + vec2(-t.x, -t.y)).rgb + texture(InputTexture, uv + vec2(t.x, -t.y)).rgb
		+ texture(InputTexture, uv + vec2(-t.x, t.y)).rgb + texture(InputTexture, uv + vec2(t.x, t.y)).rgb;
	color *= (1.0 / 16.0) * Intensity;
	FragColor = vec4(color, 1.0);
}
