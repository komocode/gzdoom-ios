
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D ExposureTexture;

// Eye adaptation: the exposure (in EV) that brings the log-average scene
// luminance to the key value. It is blended over the previous value with
// alpha 1 - exp(-dt * speed), so the speed doesn't depend on the frame rate.
void main()
{
	float logAverage = texture(ExposureTexture, vec2(0.5)).x;
	float ev = clamp(log2(Key) - logAverage, MinEV, MaxEV);
	FragColor = vec4(ev, 0.0, 0.0, Blend);
}
