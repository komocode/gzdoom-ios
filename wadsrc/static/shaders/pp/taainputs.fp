
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// Inputs for a platform temporal upscaler (MetalFX), at render resolution:
// device depth, or (with MOTION) the motion of each pixel from the camera:
// its position in the previous frame minus its position now, in UV units.
// The texture covers just the rendered part of the scene buffers.

layout(binding=0) uniform sampler2D DepthTexture;

void main()
{
	vec2 s = TexCoord;
	float depth = textureLod(DepthTexture, InputOffset + s * InputScale, 0.0).x;
#ifdef MOTION
	float z = 1.0 / (clamp(depth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);
	vec2 su = s - Jitter;
	vec3 viewPos = vec3((UVToViewA * su + UVToViewB) * z, z);
	vec4 clip = Reprojection * vec4(viewPos.xy, -viewPos.z, 1.0);
	vec2 prev = clip.w > 0.0 ? clip.xy / clip.w * 0.5 + 0.5 : su;
	FragColor = vec4(prev - su, 0.0, 1.0);
#else
	FragColor = vec4(depth, 0.0, 0.0, 1.0);
#endif
}
