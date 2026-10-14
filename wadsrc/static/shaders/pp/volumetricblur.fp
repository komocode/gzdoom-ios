layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// 3x3 depth-aware blur of the volumetric scattering. It removes the per-pixel
// sample jitter without bleeding across depth edges.

layout(binding=0) uniform sampler2D InputTexture;
layout(binding=1) uniform sampler2D DepthTexture;	// linear depth

void main()
{
	float centerDepth = textureLod(DepthTexture, TexCoord, 0.0).x;
	vec4 sum = vec4(0.0);
	float weights = 0.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec2 uv = TexCoord + vec2(float(x), float(y)) * InvSize;
			float depth = textureLod(DepthTexture, uv, 0.0).x;
			float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
			w *= exp2(-DepthSharpness * abs(depth - centerDepth) / max(centerDepth, 1.0));
			sum += textureLod(InputTexture, uv, 0.0) * w;
			weights += w;
		}
	}
	FragColor = sum / weights;
}
