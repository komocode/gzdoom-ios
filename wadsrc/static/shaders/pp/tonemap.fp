
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D InputTexture;
#if !defined(PALETTE)
layout(binding=1) uniform sampler2D BloomTexture;
layout(binding=2) uniform sampler2D ExposureTexture;
layout(binding=3) uniform sampler2D GradeLUT0;
layout(binding=4) uniform sampler2D GradeLUT1;
#endif

vec3 Linear(vec3 c)
{
	//c = max(c, vec3(0.0));
	//return pow(c, 2.2);
	return c * c; // cheaper, but assuming gamma of 2.0 instead of 2.2
}

vec3 sRGB(vec3 c)
{
	c = max(c, vec3(0.0));
	//return pow(c, vec3(1.0 / 2.2));
	return sqrt(c); // cheaper, but assuming gamma of 2.0 instead of 2.2
}

#if defined(LINEAR)

vec3 Tonemap(vec3 color)
{
	return sRGB(color);
}

#elif defined(REINHARD)

vec3 Tonemap(vec3 color)
{
	color = color / (1 + color);
	return sRGB(color);
}

#elif defined(HEJLDAWSON)

vec3 Tonemap(vec3 color)
{
	vec3 x = max(vec3(0), color - 0.004);
	return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06); // no sRGB needed
}

#elif defined(UNCHARTED2)

vec3 Uncharted2Tonemap(vec3 x)
{
	float A = 0.15;
	float B = 0.50;
	float C = 0.10;
	float D = 0.20;
	float E = 0.02;
	float F = 0.30;
	return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 Tonemap(vec3 color)
{
	float W = 11.2;
	vec3 curr = Uncharted2Tonemap(color);
	vec3 whiteScale = vec3(1) / Uncharted2Tonemap(vec3(W));
	return sRGB(curr * whiteScale);
}

#elif defined(ACES)

// ACES filmic curve (Narkowicz fit) with a slight exposure reduction so Doom's
// midtones stay close to the original while highlights roll off smoothly.
// WhiteScale normalizes the curve so that r_tonemap_white reaches display white.
vec3 Tonemap(vec3 color)
{
	vec3 x = color * 0.9;
	vec3 mapped = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
	return sRGB(clamp(mapped * WhiteScale, 0.0, 1.0));
}

#elif defined(PALETTE)

layout(binding=1) uniform sampler2D PaletteLUT;

vec3 Tonemap(vec3 color)
{
	ivec3 c = ivec3(clamp(color.rgb, vec3(0.0), vec3(1.0)) * 63.0 + 0.5);
	int index = (c.r * 64 + c.g) * 64 + c.b;
	int tx = index % 512;
	int ty = index / 512;
	return texelFetch(PaletteLUT, ivec2(tx, ty), 0).rgb;
}

#else
#error Tonemap mode define is missing
#endif

#if !defined(PALETTE)

float Luma(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// A 3D LUT packed as a horizontal strip of size x size slices (size * size by
// size pixels): red across a slice, green down, blue selects the slice.
vec3 SampleLUT(sampler2D lut, vec3 c, float size)
{
	c = clamp(c, 0.0, 1.0) * (size - 1.0);
	float slice = floor(c.b);
	float f = c.b - slice;
	vec2 uv = vec2((c.r + 0.5) / (size * size), (c.g + 0.5) / size);
	vec3 a = textureLod(lut, uv + vec2(slice / size, 0.0), 0.0).rgb;
	vec3 b = textureLod(lut, uv + vec2(min(slice + 1.0, size - 1.0) / size, 0.0), 0.0).rgb;
	return mix(a, b, f);
}

vec3 Grade(vec3 c)
{
	if (TintAmount > 0.0)
	{
		// Split toning: shadows and highlights are tinted by luminance-normalized colours.
		float l = Luma(c);
		vec3 shadow = mix(vec3(1.0), ShadowTint.rgb / max(Luma(ShadowTint.rgb), 0.01), 1.0 - smoothstep(0.0, 0.5, l));
		vec3 high = mix(vec3(1.0), HighTint.rgb / max(Luma(HighTint.rgb), 0.01), smoothstep(0.5, 1.0, l));
		c *= shadow * high;
	}
	if (LutAmount > 0.0)
	{
		c = clamp(c, 0.0, 1.0);
		vec3 graded = SampleLUT(GradeLUT0, c, LutSize0);
		if (LutBlend > 0.0) graded = mix(graded, SampleLUT(GradeLUT1, c, LutSize1), LutBlend);
		c = mix(c, graded, LutAmount);
	}
	return c;
}

#endif

void main()
{
	vec3 color = texture(InputTexture, TexCoord).rgb;
#ifndef PALETTE
	color = Linear(color); // needed because gzdoom's scene texture is not linear at the moment

	// Exposure (manual EV bias and eye adaptation), then the bloom, which is
	// already exposed and linear, so faint halos survive.
	color *= ExposureScale * exp2(AutoExposure * texture(ExposureTexture, vec2(0.5)).x);
	if (BloomIntensity > 0.0)
	{
		vec2 bloomUV = (TexCoord - BloomOffset) / BloomScale;
		if (all(greaterThanEqual(bloomUV, vec2(0.0))) && all(lessThanEqual(bloomUV, vec2(1.0))))
			color += texture(BloomTexture, bloomUV).rgb * BloomIntensity;
	}

	if (Contrast != 1.0)
		color = 0.18 * pow(max(color, vec3(0.0)) / 0.18, vec3(Contrast));
	if (Saturation != 1.0)
	{
		float l = Luma(color);
		color = max(vec3(l) + (color - vec3(l)) * Saturation, vec3(0.0));
	}

	vec3 exposed = color;
	color = Tonemap(color);
	if (TintAmount > 0.0 || LutAmount > 0.0)
		color = Grade(color);
#if !defined(LINEAR)
	if (HdrPeak > 1.0)
	{
		// EDR output: the SDR curve stays as it is up to white, and the energy
		// above white is rolled off into the display's headroom, keeping the hue.
		vec3 sdr = color * color;
		float peak = max(max(sdr.r, sdr.g), sdr.b);
		float excess = max(max(max(exposed.r, exposed.g), exposed.b) - 1.0, 0.0);
		float extra = (HdrPeak - 1.0) * excess / (excess + HdrPeak - 1.0);
		color = sqrt(sdr * ((peak + extra) / max(peak, 1e-4)));
	}
#endif
	FragColor = vec4(color, 1.0);
#else
	FragColor = vec4(Tonemap(color), 1.0);
#endif
}
