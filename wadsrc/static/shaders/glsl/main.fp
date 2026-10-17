

layout(location = 0) in vec4 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 pixelpos;
layout(location = 3) in vec3 glowdist;
layout(location = 4) in vec3 gradientdist;
layout(location = 5) in vec4 vWorldNormal;
layout(location = 6) in vec4 vEyeNormal;
layout(location = 9) in vec3 vLightmap;
layout(location = 10) flat in vec4 vVolumeRow0;
layout(location = 11) flat in vec4 vVolumeRow1;
layout(location = 12) flat in vec4 vVolumeRow2;
layout(location = 13) flat in vec4 vVolumeParams;

#ifdef NO_CLIPDISTANCE_SUPPORT
layout(location = 7) in vec4 ClipDistanceA;
layout(location = 8) in vec4 ClipDistanceB;
#endif

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
#endif

struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// w > 0 is reserved for GPU particles. A negative value marks an actor
// explicitly opted into sprite materials by the renderer; normals alone must
// not change HUD sprites, effects, models, or classic colormap behavior.
bool IsPBRSprite()
{
	return (uRenderFeatures & 128) != 0 && uMaterialParams.w < 0.0;
}

vec3 SafeNormal(vec3 direction, vec3 fallback)
{
	float lengthSquared = dot(direction, direction);
	return lengthSquared > 1e-12 ? direction * inversesqrt(lengthSquared) : fallback;
}

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;

//===========================================================================
//
// RGB to HSV
//
//===========================================================================

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.3, 0.56, 0.14));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
	if (factor != 0.0)
	{
		float gray = grayscale(texel);
		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
	return dodesaturate(texel, uDesaturationFactor);
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;

 vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
 {
	// Step 1: desaturate according to the material's desaturation factor. 
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;
			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;
			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	// TAA and upscaling resolve subpixel detail, so they sample sharper mips.
	vec4 texel;
	if (uMipBias != 0.0) texel = texture(tex, st, uMipBias);
	else texel = texture(tex, st);

	//
	// Apply texture modes
	//
	switch (uTextureMode & 0xffff)
	{
		case 1:	// TM_STENCIL
			texel.rgb = vec3(1.0,1.0,1.0);
			break;

		case 2:	// TM_OPAQUE
			texel.a = 1.0;
			break;

		case 3:	// TM_INVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);
			break;

		case 4:	// TM_ALPHATEXTURE
		{
			float gray = grayscale(texel);
			texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			break;
		}

		case 5:	// TM_CLAMPY
			if (st.t < 0.0 || st.t > 1.0)
			{
				texel.a = 0.0;
			}
			break;

		case 6: // TM_OPAQUEINVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);
			break;

		case 7: //TM_FOGLAYER 
			return texel;

	}

	if ((uTextureMode & TEXF_ClampY) != 0)
	{
		if (st.t < 0.0 || st.t > 1.0)
		{
			texel.a = 0.0;
		}
	}

	// Apply the texture modification colors.
	int blendflags = int(uTextureAddColor.a);	// this alpha is unused otherwise
	if (blendflags != 0)	
	{
		// only apply the texture manipulation if it contains something.
		texel = ApplyTextureManipulation(texel, blendflags);
	}

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	texel.rgb += uAddColor.rgb;
	if (uObjectColor2.a == 0.0) texel *= uObjectColor;
	else texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);

	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}

//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0f, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

float R_DoomColormap(float light, float z)
{
	if ((uPalLightLevels >> 16) == 16) // gl_lightmode 16
	{
		float lightnum = clamp(light * 15.0, 0.0, 15.0);

		if (!IsPBRSprite() && dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
		{
			vec3 normal = normalize(vWorldNormal.xyz);
			return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
		}
		else // vWorldNormal is not set on sprites
		{
			return R_PlaneColormap(lightnum, z);
		}
	}
	else
	{
		return R_ZDoomColormap(light, z);
	}
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;
	if (((uPalLightLevels >> 8)  & 0xff) == 2)
	{
		z = distance(pixelpos.xyz, uCameraPos.xyz);
	}
	else 
	{
		z = pixelpos.w;
	}

	if ((uPalLightLevels >> 16) == 5) // gl_lightmode 5: Build software lighting emulation.
	{
		// This is a lot more primitive than Doom's lighting...
		float numShades = float(uPalLightLevels & 255);
		float curshade = (1.0 - light) * (numShades - 1.0);
		float visibility = max(uGlobVis * uLightFactor * z, 0.0);
		float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
		return clamp(shade * uLightDist, 0.0, 1.0);
	}

	float colormap = R_DoomColormap(light, z);

	if ((uPalLightLevels & 0xff) != 0)
		colormap = floor(colormap) + 0.5;

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

//===========================================================================
//
// Check if light is in shadow
//
//===========================================================================

#ifdef SUPPORTS_RAYTRACING

bool traceHit(vec3 origin, vec3 direction, float dist)
{
	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01f, direction, dist);
	while(rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 softshadow[9 * 3] = vec2[](
	vec2( 0.0, 0.0),
	vec2(-2.0,-2.0),
	vec2( 2.0, 2.0),
	vec2( 2.0,-2.0),
	vec2(-2.0, 2.0),
	vec2(-1.0,-1.0),
	vec2( 1.0, 1.0),
	vec2( 1.0,-1.0),
	vec2(-1.0, 1.0),

	vec2( 0.0, 0.0),
	vec2(-1.5,-1.5),
	vec2( 1.5, 1.5),
	vec2( 1.5,-1.5),
	vec2(-1.5, 1.5),
	vec2(-0.5,-0.5),
	vec2( 0.5, 0.5),
	vec2( 0.5,-0.5),
	vec2(-0.5, 0.5),

	vec2( 0.0, 0.0),
	vec2(-1.25,-1.75),
	vec2( 1.75, 1.25),
	vec2( 1.25,-1.75),
	vec2(-1.75, 1.75),
	vec2(-0.75,-0.25),
	vec2( 0.25, 0.75),
	vec2( 0.75,-0.25),
	vec2(-0.25, 0.75)
);

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	if (shadowIndex >= 1024.0)
		return 1.0; // Don't cast rays for this light

	vec3 origin = pixelpos.xzy;
	vec3 target = lightpos.xzy + 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	vec3 direction = normalize(target - origin);
	float dist = distance(origin, target);

	if (uShadowmapFilter <= 0)
	{
		return traceHit(origin, direction, dist) ? 0.0 : 1.0;
	}
	else
	{
		vec3 v = (abs(direction.x) > abs(direction.y)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 xdir = normalize(cross(direction, v));
		vec3 ydir = cross(direction, xdir);

		float sum = 0.0;
		int step_count = clamp(uShadowmapFilter, 1, 3) * 9;
		for (int i = 0; i < step_count; i++)
		{
			vec3 pos = target + xdir * softshadow[i].x + ydir * softshadow[i].y;
			sum += traceHit(origin, normalize(pos - origin), dist) ? 0.0 : 1.0;
		}
		return sum / step_count;
	}
}

#else
#ifdef SUPPORTS_SHADOWMAPS

float shadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

vec2 shadowUToDir(float u)
{
	u *= 4.0;
	vec2 raydir;
	switch (int(u))
	{
	case 0: raydir = vec2(u * 2.0 - 1.0, 1.0); break;
	case 1: raydir = vec2(1.0, 1.0 - (u - 1.0) * 2.0); break;
	case 2: raydir = vec2(1.0 - (u - 2.0) * 2.0, -1.0); break;
	case 3: raydir = vec2(-1.0, (u - 3.0) * 2.0 - 1.0); break;
	}
	return raydir;
}

float sampleShadowmap(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x) * 0.25;

	// Snap to shadow map texel grid
	if (abs(ray.z) > abs(ray.x))
	{
		ray.y = ray.y / abs(ray.z);
		ray.x = ray.x / abs(ray.z);
		ray.x = (floor((ray.x + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.z = sign(ray.z);
	}
	else
	{
		ray.y = ray.y / abs(ray.x);
		ray.z = ray.z / abs(ray.x);
		ray.z = (floor((ray.z + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.x = sign(ray.x);
	}

	float t = negD / dot(vWorldNormal.xyz, ray) - bias;
	vec2 dir = ray.xz * t;

	float u = shadowDirToU(dir);
	float dist2 = dot(dir, dir);
	return step(dist2, texture(ShadowMap, vec2(u, v)).x);
}

float sampleShadowmapPCF(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	vec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x);
	float texelPos = floor(shadowDirToU(ray.xz) * scale);

	float sum = 0.0;
	float step_count = uShadowmapFilter;

	texelPos -= step_count + 0.5;
	for (float x = -step_count; x <= step_count; x++)
	{
		float u = fract(texelPos / scale);
		vec2 dir = shadowUToDir(u);

		ray.x = dir.x;
		ray.z = dir.y;
		float t = negD / dot(vWorldNormal.xyz, ray) - bias;
		dir = ray.xz * t;

		float dist2 = dot(dir, dir);
		sum += step(dist2, texture(ShadowMap, vec2(u, v)).x);
		texelPos++;
	}
	return sum / (uShadowmapFilter * 2.0 + 1.0);
}

// Contact-hardening soft shadows. A blocker search estimates how far the
// occluder is from the receiver; the filter widens with that gap, so shadows
// are sharp where an object meets the floor and soft further away. Taps are
// tent weighted and jittered per pixel to avoid banding.
vec2 shadowmapTap(vec3 ray, float negD, float texel, float scale, float v)
{
	float u = fract(texel / scale);
	vec2 dir = shadowUToDir(u);
	ray.x = dir.x;
	ray.z = dir.y;
	float t = negD / dot(vWorldNormal.xyz, ray) - 1.0;
	dir = ray.xz * t;
	return vec2(texture(ShadowMap, vec2(u, v)).x, dot(dir, dir));
}

float sampleShadowmapSoft(vec3 planePoint, float v)
{
	float negD = dot(vWorldNormal.xyz, planePoint);
	vec3 ray = planePoint;
	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	float scale = float(textureSize(ShadowMap, 0).x);
	float center = shadowDirToU(ray.xz) * scale;
	float maxRadius = 1.5 + float(uShadowmapFilter) * 2.0;
	float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

	// Blocker search.
	float blockers = 0.0;
	float blockerDist = 0.0;
	float receiverDist = 0.0;
	for (int i = 0; i < 6; i++)
	{
		float offset = (float(i) + noise - 2.5) / 2.5 * maxRadius;
		vec2 tap = shadowmapTap(ray, negD, center + offset, scale, v);
		receiverDist += sqrt(tap.y);
		// Only occluders clearly in front of the receiver count; texels of the
		// receiving wall itself at grazing angles must not.
		if (sqrt(tap.x) < sqrt(tap.y) - 8.0)
		{
			blockers += 1.0;
			blockerDist += sqrt(tap.x);
		}
	}
	if (blockers == 0.0) return 1.0;
	blockerDist /= blockers;
	receiverDist /= 6.0;

	// Penumbra for a light source about 4 map units wide, in shadow map texels.
	const float lightSize = 4.0;
	float penumbra = lightSize * (receiverDist - blockerDist) / max(blockerDist, 1.0);
	float radius = clamp(penumbra / max(receiverDist, 1.0) * scale * 0.16, 0.5, maxRadius);

	float sum = 0.0;
	float weights = 0.0;
	int taps = 4 + uShadowmapFilter * 2;
	for (int i = 0; i < taps; i++)
	{
		float x = (float(i) + noise) / float(taps) * 2.0 - 1.0;
		float w = 1.0 - abs(x);
		vec2 tap = shadowmapTap(ray, negD, center + x * radius, scale, v);
		sum += step(tap.y, tap.x) * w;
		weights += w;
	}
	return sum / max(weights, 0.001);
}

float shadowmapAttenuation(vec4 lightpos, float shadowIndex)
{
	if (shadowIndex >= 1024.0)
		return 1.0; // No shadowmap available for this light

	vec3 planePoint = pixelpos.xyz - lightpos.xyz;
	planePoint += 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0; // Light is too close

	float v = (shadowIndex + 0.5) / 1024.0;
	if (IsPBRSprite())
	{
		// A billboard represents a volume, not an infinite receiver plane.
		// Plane/PCF intersections become singular for side lights on sprites.
		float receiverDistance = max(length(planePoint.xz) - 1.0, 0.0);
		float u = shadowDirToU(planePoint.xz);
		float distanceSquared = receiverDistance * receiverDistance;
		if (uShadowmapFilter <= 0)
			return step(distanceSquared, texture(ShadowMap, vec2(u, v)).x);
		float texel = 1.0 / float(textureSize(ShadowMap, 0).x);
		float visibility = 0.0;
		for (int tap = -1; tap <= 1; ++tap)
			visibility += step(distanceSquared, texture(ShadowMap, vec2(fract(u + float(tap) * texel), v)).x);
		return visibility / 3.0;
	}

	if (uShadowmapFilter <= 0)
	{
		return sampleShadowmap(planePoint, v);
	}
	else if ((uRenderFeatures & 1) != 0) // RF_MODERN_LIGHT
	{
		return sampleShadowmapSoft(planePoint, v);
	}
	else
	{
		return sampleShadowmapPCF(planePoint, v);
	}
}

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	return shadowmapAttenuation(lightpos, shadowIndex);
}

#else

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}

#endif
#endif

float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

//===========================================================================
//
// Adjust normal vector according to the normal map
//
//===========================================================================

mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);
	if (IsPBRSprite())
	{
		// Solve using the UV determinant rather than assuming the billboard's
		// winding. Mirrored Doom rotations reverse U and must reverse the
		// tangent without reversing the normal toward the camera.
		float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
		if (abs(determinant) > 1e-12)
		{
			vec3 spriteT = (dp1 * duv2.y - dp2 * duv1.y) / determinant;
			vec3 spriteB = (dp2 * duv1.x - dp1 * duv2.x) / determinant;
			spriteT -= n * dot(n, spriteT);
			spriteB -= n * dot(n, spriteB);
			if (dot(spriteT, spriteT) > 1e-12 && dot(spriteB, spriteB) > 1e-12)
				return mat3(normalize(spriteT), normalize(spriteB), n);
		}
	}

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float frameSize = max(dot(t,t), dot(b,b));
	if (IsPBRSprite() && frameSize < 1e-12)
	{
		// Degenerate UVs can occur on tiny or clipped sprites. Keep the frame
		// finite, including frames rendered without any dynamic lights.
		vec3 axis = abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		t = SafeNormal(cross(axis, n), vec3(1.0, 0.0, 0.0));
		return mat3(t, cross(n, t), n);
	}
	float invmax = inversesqrt(frameSize);
	return mat3(t * invmax, b * invmax, n);
}

#if defined(NORMALMAP)
vec3 ApplyNormalMap(vec2 texcoord)
{
	#define WITH_NORMALMAP_UNSIGNED
	#define WITH_NORMALMAP_GREEN_UP
	//#define WITH_NORMALMAP_2CHANNEL

	vec3 interpolatedNormal = IsPBRSprite() ? SafeNormal(vWorldNormal.xyz, vec3(0.0, 0.0, 1.0)) : normalize(vWorldNormal.xyz);

	vec3 map = texture(normaltexture, texcoord).xyz;
	#if defined(WITH_NORMALMAP_UNSIGNED)
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	#endif
	#if defined(WITH_NORMALMAP_2CHANNEL)
	map.z = sqrt(1 - dot(map.xy, map.xy));
	#endif
	#if defined(WITH_NORMALMAP_GREEN_UP)
	map.y = -map.y;
	#endif
	if (IsPBRSprite()) map.xy *= max(uMaterialParams.x, 0.0);

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	vec3 bumpedNormal = IsPBRSprite() ? SafeNormal(tbn * map, interpolatedNormal) : normalize(tbn * map);
	return bumpedNormal;
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
	return IsPBRSprite() ? SafeNormal(vWorldNormal.xyz, vec3(0.0)) : normalize(vWorldNormal.xyz);
}
#endif

//===========================================================================
//
// Modern lighting features (see hw_modernrender.h)
//
//===========================================================================

const int RF_MODERN_LIGHT = 1;
const int RF_AUTO_NORMAL = 2;
const int RF_SPECULAR = 4;
const int RF_IRRADIANCE = 8;
const int RF_REFLECTIONS = 16;
const int RF_PARALLAX = 32;
const int RF_HDR_EMISSIVE = 64;
const int RF_SPRITE_PBR = 128;
const int RF_SPRITE_REFLECTIONS = 256;

bool HasFeature(int feature)
{
	return (uRenderFeatures & feature) != 0;
}

bool HasSurfaceNormal()
{
	return dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5;
}

// The directional sun (hw_sun.h) lights this draw. Its shading is only compiled
// into the scene shaders' sun variants (SUN_LIGHT), which the backends use while a
// sunlit viewpoint is bound, so the shaders of maps without a sun are unchanged.
bool SunActive()
{
#ifdef SUN_LIGHT
	return uSunDir.w > 0.0 && uSurfaceParams.z < 0.5;
#else
	return false;
#endif
}

//===========================================================================
//
// Wet surfaces and screen-space reflections. uMaterialParams2 holds the
// wetness, puddle coverage and rain ripple strength of the surface (from
// MAPINFO and UDMF sector properties) and its G-buffer mode.
//
//===========================================================================

float gPuddle = 0.0;		// puddle mask of this pixel (0-1)
float gWetness = 0.0;		// wetness after puddles (0-1)
float gReflectivity = 0.0;	// written to the G-buffer for SSR
float gRoughness = 1.0;
vec3 gReflectNormal = vec3(0.0);	// world space; zero keeps the geometric normal
vec2 gTexCoord = vec2(0.0);	// texture coordinate after parallax

float SurfaceHash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float SurfaceNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(SurfaceHash(i), SurfaceHash(i + vec2(1.0, 0.0)), f.x),
		mix(SurfaceHash(i + vec2(0.0, 1.0)), SurfaceHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float UpFacing()
{
	return HasSurfaceNormal() ? smoothstep(0.5, 0.9, normalize(vWorldNormal.xyz).y) : 0.0;
}

// Procedural puddles: low-frequency noise in map space, thresholded by the
// coverage. Dark texels (cracks and mortar) fill first.
float PuddleMask(float texelLum)
{
	float coverage = uMaterialParams2.y;
	if (coverage <= 0.0) return 0.0;
	float up = UpFacing();
	if (up <= 0.0) return 0.0;
	vec2 p = pixelpos.xz / 192.0;
	float n = SurfaceNoise(p) * 0.6 + SurfaceNoise(p * 2.7 + 13.1) * 0.3 + SurfaceNoise(p * 7.9 + 5.3) * 0.1;
	n += (0.5 - texelLum) * 0.12;
	float threshold = mix(0.78, 0.22, coverage);
	return smoothstep(threshold - 0.03, threshold + 0.03, n) * up;
}

// Expanding rings from rain drops, as a gradient of the water height in map space.
vec2 RainRipples(vec2 p, float time)
{
	vec2 grad = vec2(0.0);
	for (int layer = 0; layer < 3; layer++)
	{
		vec2 q = p / 22.0 + vec2(float(layer) * 0.37, float(layer) * 0.71);
		vec2 cell = floor(q);
		float h = SurfaceHash(cell + float(layer) * 17.0);
		vec2 center = vec2(0.3) + 0.4 * vec2(h, fract(h * 13.7));
		float phase = fract(time * 0.8 + h * 7.0);
		vec2 d = fract(q) - center;
		float r = length(d);
		float x = (r - phase * 0.3) * 36.0;
		float amp = (1.0 - phase) * (1.0 - phase);
		grad += d / max(r, 1e-3) * (x * exp(-x * x * 0.5)) * amp;
	}
	return grad;
}

float EncodeReflection(float reflectivity, float roughness)
{
	// Alpha above 0.5 marks a scene surface. The 7 bits below hold 4 bits of
	// reflectivity and 3 of roughness, dithered so TAA smooths the steps.
	float d = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float r = min(floor(clamp(reflectivity, 0.0, 1.0) * 15.0 + d), 15.0);
	float g = min(floor(clamp(roughness, 0.0, 1.0) * 7.0 + d), 7.0);
	return (255.0 - (r * 8.0 + g)) / 255.0;
}

// Explicit gradients keep the lookups valid inside divergent control flow.
float HeightAt(vec2 st, vec2 gx, vec2 gy)
{
	return grayscale(textureGrad(tex, st, gx, gy));
}

// Steep parallax mapping with the diffuse luminance as the height field.
// Dark texels (mortar, seams, grilles) read as recessed.
vec2 ParallaxTexCoord(vec2 st)
{
	vec2 gx = dFdx(st), gy = dFdy(st);
	if (IsPBRSprite() || uMaterialParams.z == 0.0 || !HasSurfaceNormal() || (uTextureMode & 0xffff) != 0) return st;
	vec3 toCamera = uCameraPos.xyz - pixelpos.xyz;
	float dist = length(toCamera);
	float fade = (1.0 - smoothstep(256.0, 640.0, dist)) * (1.0 - gPuddle);
	if (fade <= 0.0) return st;

	vec3 n = normalize(vWorldNormal.xyz);
	mat3 tbn = cotangent_frame(n, pixelpos.xyz, st);
	vec3 viewTS = normalize(transpose(tbn) * (toCamera / dist));
	// Fade out at grazing angles, where the march would smear the texture.
	fade *= smoothstep(0.08, 0.35, viewTS.z);
	if (fade <= 0.0) return st;

	// About four texels of depth regardless of texture size. The bias in the
	// divisor limits the offset at shallow angles.
	vec2 texSize = vec2(textureSize(tex, 0));
	vec2 maxOffset = viewTS.xy / (viewTS.z + 0.4) * (4.0 / texSize) * fade;
	const int steps = 10;
	float layer = 1.0 / float(steps);
	vec2 delta = maxOffset * layer;
	vec2 cur = st;
	float depth = 0.0;
	float h = 1.0 - HeightAt(cur, gx, gy);
	float prevH = h;
	for (int i = 0; i < steps && depth < h; i++)
	{
		prevH = h;
		cur -= delta;
		depth += layer;
		h = 1.0 - HeightAt(cur, gx, gy);
	}
	// Interpolate between the last two samples.
	float after = h - depth;
	float before = prevH - (depth - layer);
	float w = after / (after - before + 1e-5);
	return mix(cur, cur + delta, clamp(w, 0.0, 1.0));
}

// Relief derived from the diffuse texture for lighting.
vec3 AutoNormal(vec3 geomNormal, vec2 st, vec2 gx, vec2 gy)
{
	vec2 texel = 1.0 / vec2(textureSize(tex, 0));
	float hL = HeightAt(st - vec2(texel.x, 0.0), gx, gy);
	float hR = HeightAt(st + vec2(texel.x, 0.0), gx, gy);
	float hD = HeightAt(st - vec2(0.0, texel.y), gx, gy);
	float hU = HeightAt(st + vec2(0.0, texel.y), gx, gy);
	float k = 2.5 * uNormalStrength;
	vec3 bump = normalize(vec3((hL - hR) * k, (hD - hU) * k, 1.0));
	mat3 tbn = cotangent_frame(geomNormal, pixelpos.xyz, st);
	return normalize(tbn * bump);
}

vec3 SampleIrradianceFace(int face, vec2 uv, float z0, float z1, float t)
{
	float base = float(face) * uGISize.z;
	vec3 a = texture(IrradianceVolume, vec3(uv, base + z0)).rgb;
	vec3 b = texture(IrradianceVolume, vec3(uv, base + z1)).rgb;
	return mix(a, b, t);
}

// Ambient cube lookup. normal is in world space (x, height, y); zero for
// classic sprites, while material sprites sample with their per-pixel normal.
vec3 SampleIrradiance(vec3 normal)
{
	vec3 n = normal.xzy;
	vec3 mapPos = pixelpos.xzy;
	float spacing = max(uGIOrigin.w, 0.001);
	// Normal maps change the sampling direction, not the character's position.
	// Offset by its billboard normal so adjacent texels cannot jump through
	// walls into a different room as their authored surface direction varies.
	vec3 offsetNormal = IsPBRSprite() ? SafeNormal(vWorldNormal.xyz, vec3(0.0)).xzy : n;
	vec3 p = (mapPos + offsetNormal * (spacing * 0.5) - uGIOrigin.xyz) / spacing;
	vec2 uv = (clamp(p.xy, vec2(0.0), uGISize.xy - 1.0) + 0.5) / vec2(textureSize(IrradianceVolume, 0).xy);
	float z = clamp(p.z, 0.0, uGISize.z - 1.0);
	float z0 = floor(z);
	float z1 = min(z0 + 1.0, uGISize.z - 1.0);
	float t = z - z0;
	if (dot(n, n) < 0.5)
	{
		vec3 sum = vec3(0.0);
		for (int f = 0; f < 6; f++) sum += SampleIrradianceFace(f, uv, z0, z1, t);
		return sum * (1.0 / 6.0);
	}
	vec3 n2 = n * n;
	return n2.x * SampleIrradianceFace(n.x >= 0.0 ? 0 : 1, uv, z0, z1, t)
		+ n2.y * SampleIrradianceFace(n.y >= 0.0 ? 2 : 3, uv, z0, z1, t)
		+ n2.z * SampleIrradianceFace(n.z >= 0.0 ? 4 : 5, uv, z0, z1, t);
}

// Raises the effective light level toward the bounced light from brighter
// surroundings and tints the surface with the color of nearby surfaces.
// Doom's own light level stays the minimum.
void ApplyIrradiance(inout float lightlevel, inout vec3 tint, vec3 materialNormal)
{
	vec3 normal = IsPBRSprite() ? materialNormal : (HasSurfaceNormal() ? normalize(vWorldNormal.xyz) : vec3(0.0));
	vec3 gi = SampleIrradiance(normal) * uGIStrength;
	float lum = dot(gi, vec3(0.2126, 0.7152, 0.0722));
	// Only add the part of the bounce that exceeds the sector light, and only
	// partially, so authored light contrast survives.
	lightlevel += max(min(lum, 1.0) - lightlevel, 0.0) * 0.45;
	vec3 chroma = gi / max(lum, 0.001);
	tint = mix(vec3(1.0), chroma, uGISaturation * smoothstep(0.0, 0.1, lum));
	tint /= max(dot(tint, vec3(0.2126, 0.7152, 0.0722)), 0.001);
}

// Sky exposure map (hw_weather.h): one texel per cell of the map seen from above.
// r = height where rain stops (the highest floor or 3D floor under the sky),
// g = sky ceiling height, b = sector light level, a = 1 when the sky is above.
vec4 WeatherCell(vec2 mapPos)
{
	if (uWeatherMap.w <= 0.0) return vec4(0.0);
	ivec2 cell = ivec2(floor((mapPos - uWeatherMap.xy) * uWeatherMap.z));
	if (cell.x < 0 || cell.y < 0 || cell.x >= int(uWeatherMap.w) || cell.y >= int(uWeatherParams.x)) return vec4(0.0);
	return texelFetch(WeatherMap, ivec3(cell, 0), 0);
}

// 1 where a point (x, height, y) is under open sky (rain and lightning reach it), else 0.
float WeatherExposure(vec3 pos)
{
	vec4 cell = WeatherCell(pos.xz);
	return cell.a * step(cell.r - 2.0, pos.y);
}

// Soft particles: fades a translucent fragment out within dist map units of
// the opaque scene behind it, from the half-resolution linear depth.
float SoftDepthFade(float dist)
{
	if (dist <= 0.0) return 1.0;
	ivec2 size = textureSize(SceneLinearDepth, 0);
	ivec2 texel = clamp(ivec2(gl_FragCoord.xy * uSceneDepthMap.xy + uSceneDepthMap.zw), ivec2(0), size - 1);
	float sceneDepth = texelFetch(SceneLinearDepth, texel, 0).r;
	return clamp((sceneDepth - pixelpos.w) / dist, 0.0, 1.0);
}

void ApplySpriteVertexIrradiance(inout vec3 color, vec3 normal)
{
	// Non-software light modes already carry their sector brightness in
	// vColor. They still need directional bounce when distance fog is off.
	float originalLevel = max(max(color.r, color.g), color.b);
	float lightlevel = originalLevel;
	vec3 tint;
	ApplyIrradiance(lightlevel, tint, normal);
	vec3 sectorTint = originalLevel > 0.001 ? color / originalLevel : vec3(1.0);
	color = sectorTint * lightlevel * tint;
}

// The sprite path uses the same maps as authored surface PBR materials, but
// keeps the sector light level as diffuse ambient illumination. Direct lights
// enter exactly once here; uDynLightColor is only the optional residual light.
// These functions intentionally do not change the existing wall/model BRDF.
void SpriteMaterialProperties(Material material, out float metallic, out float roughness, out float ao, out vec3 f0)
{
	vec3 albedo = pow(max(material.Base.rgb, vec3(0.0)), vec3(2.2));
#if defined(PBR)
	metallic = clamp(material.Metallic, 0.0, 1.0);
	roughness = clamp(material.Roughness, 0.12, 1.0);
	ao = clamp(material.AO, 0.0, 1.0);
	f0 = mix(vec3(0.04), albedo, metallic);
#elif defined(SPECULAR)
	// Authored specular maps remain authoritative, including colored metals.
	// Their exponent follows the existing GZDoom specular shader convention.
	metallic = 0.0;
	roughness = clamp(pow(2.0 / (max(material.Glossiness * 4.0, 0.0) + 2.0), 0.25), 0.12, 1.0);
	ao = 1.0;
	f0 = clamp(material.Specular * material.SpecularLevel, vec3(0.0), vec3(0.98));
#else
	metallic = 0.0;
	roughness = 0.85;
	ao = 1.0;
	f0 = vec3(0.04);
#endif
}

vec3 SpriteFresnel(float cosine, vec3 f0)
{
	return f0 + (1.0 - f0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
}

vec3 SpriteLightContribution(int index, vec3 albedo, vec3 normal, vec3 viewdir, vec3 f0, float metallic, float roughness)
{
	vec4 lightpos = lights[index];
	vec4 lightcolor = lights[index + 1];
	vec4 spot1 = lights[index + 2];
	vec4 spot2 = lights[index + 3];
	vec3 toLight = lightpos.xyz - pixelpos.xyz;
	float distanceToLight = length(toLight);
	if (lightpos.w <= 0.0 || distanceToLight >= lightpos.w) return vec3(0.0);
	vec3 lightdir = SafeNormal(toLight, normal);
	float attenuation = 1.0 - distanceToLight / lightpos.w;
	if (spot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, spot1.xyz, spot2.x, spot2.y);
	if (attenuation <= 0.0) return vec3(0.0);
	attenuation *= shadowAttenuation(lightpos, lightcolor.a);
	if (attenuation <= 0.0) return vec3(0.0);

	float ndotl = dot(normal, lightdir);
	float nl = max(ndotl, 0.0);
	float nv = max(dot(normal, viewdir), 0.001);
	vec3 halfdir = SafeNormal(viewdir + lightdir, normal);
	float nh = max(dot(normal, halfdir), 0.0);
	vec3 fresnel = SpriteFresnel(dot(halfdir, viewdir), f0);
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denominator = nh * nh * (alpha2 - 1.0) + 1.0;
	float distribution = alpha2 / max(3.14159265359 * denominator * denominator, 1e-6);
	float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
	float geometry = nv / (nv * (1.0 - k) + k) * nl / max(nl * (1.0 - k) + k, 0.001);
	vec3 specular = distribution * geometry * fresnel / max(4.0 * nv * nl, 0.001);
	// The shallow wrap softens the edge of the authored rounded volume. There is
	// no billboard-plane backface rejection: side lights can catch a shoulder.
	// Specular remains strictly in the visible lit hemisphere. The light's
	// legacy attenuated flag must not apply a second cosine to this BRDF.
	float diffuseCosine = clamp((ndotl + 0.12) / 1.12, 0.0, 1.0);
	vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo * (diffuseCosine / 3.14159265359);
	vec3 radiance = max(desaturate(vec4(lightcolor.rgb, 0.0)).rgb, vec3(0.0)) * attenuation;
	// Authored sprites can boost local light without lifting neutral ambient
	// or changing the material response of walls, models and classic sprites.
	return (diffuse + specular * nl) * radiance * uMaterialParams.z;
}

// Baked static-room radiance, stored in the IrradianceVolume array after its
// diffuse ambient-cube slices. Each probe occupies four bordered octahedral
// maps in a 2x2 tile; levels 0..3 increase roughness. Values are linear RGB.
vec2 ReflectionOctaUV(vec3 direction)
{
	vec3 n = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-6);
	vec2 uv = n.xy;
	if (n.z < 0.0)
		uv = (vec2(1.0) - abs(uv.yx)) * vec2(uv.x >= 0.0 ? 1.0 : -1.0, uv.y >= 0.0 ? 1.0 : -1.0);
	return uv * 0.5 + 0.5;
}

vec4 SampleReflectionProbe(vec3 index, vec2 octaUV, float roughness)
{
	float probe = (index.z * uReflectionSize.y + index.y) * uReflectionSize.x + index.x;
	float perLayer = uReflectionAtlas.x * uReflectionAtlas.y;
	float layer = uReflectionSize.w + floor(probe / perLayer);
	float tile = mod(probe, perLayer);
	float bordered = uReflectionAtlas.z + 2.0;
	vec2 tilePos = vec2(mod(tile, uReflectionAtlas.x), floor(tile / uReflectionAtlas.x)) * (bordered * 2.0);
	float lod = clamp(roughness, 0.0, 1.0) * 3.0;
	float lo = floor(lod), hi = min(lo + 1.0, 3.0);
	vec2 loPos = vec2(mod(lo, 2.0), floor(lo / 2.0)) * bordered;
	vec2 hiPos = vec2(mod(hi, 2.0), floor(hi / 2.0)) * bordered;
	vec2 texel = tilePos + vec2(1.0) + octaUV * uReflectionAtlas.z;
	vec2 extent = vec2(textureSize(IrradianceVolume, 0).xy);
	vec4 a = texture(IrradianceVolume, vec3((texel + loPos) / extent, layer));
	vec4 b = texture(IrradianceVolume, vec3((texel + hiPos) / extent, layer));
	return mix(a, b, fract(lod));
}

vec3 SampleSpriteEnvironment(vec3 worldPosition, vec3 reflectionDirection, float roughness, out float coverage)
{
	coverage = 0.0;
	if (uReflectionAtlas.w < 0.5 || !HasFeature(RF_SPRITE_REFLECTIONS)) return vec3(0.0);
	vec3 p = clamp((worldPosition.xzy - uReflectionOrigin.xyz) / uReflectionOrigin.w,
		vec3(0.0), uReflectionSize.xyz - vec3(1.0));
	vec3 lo = floor(p), hi = min(lo + vec3(1.0), uReflectionSize.xyz - vec3(1.0));
	vec3 blend = fract(p);
	vec2 uv = ReflectionOctaUV(reflectionDirection.xzy);
	vec4 result = vec4(0.0);
	// Trilinear spatial interpolation avoids reflection popping as actors move.
	// Invalid probes are transparent, and remaining weights are renormalized.
	for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++)
	{
		vec3 side = vec3(float(x), float(y), float(z));
		vec3 weight = mix(vec3(1.0) - blend, blend, side);
		float w = weight.x * weight.y * weight.z;
		if (w > 0.0001) result += SampleReflectionProbe(mix(lo, hi, side), uv, roughness) * w;
	}
	coverage = result.a;
	return result.a > 0.0001 ? result.rgb / result.a : vec3(0.0);
}


vec3 ProcessSpriteMaterialLight(Material material, vec3 ambientLight)
{
	float metallic, roughness, ao;
	vec3 f0;
	SpriteMaterialProperties(material, metallic, roughness, ao, f0);
	vec3 normal = SafeNormal(material.Normal, SafeNormal(vWorldNormal.xyz, vec3(0.0, 0.0, 1.0)));
	vec3 viewdir = SafeNormal(uCameraPos.xyz - pixelpos.xyz, normal);
	vec3 albedo = pow(max(material.Base.rgb, vec3(0.0)), vec3(2.2));
	vec3 ambient = pow(max(ambientLight + desaturate(uDynLightColor).rgb, vec3(0.0)), vec3(2.2));
	float nv = max(dot(normal, viewdir), 0.0);
	vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nv, 5.0);
	vec3 color = ambient * albedo * (1.0 - metallic) * (1.0 - fresnel) * ao;

	if (uLightIndex >= 0)
	{
		ivec4 range = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		for (int i = range.x; i < range.y; i += 4)
			color += SpriteLightContribution(i, albedo, normal, viewdir, f0, metallic, roughness);
		for (int i = range.y; i < range.z; i += 4)
			color -= SpriteLightContribution(i, albedo, normal, viewdir, f0, metallic, roughness);
		for (int i = range.z; i < range.w; i += 4)
			color += SpriteLightContribution(i, albedo, normal, viewdir, f0, metallic, roughness);
	}

	// The sector's ambient illumination is also the fallback environment.
	// Without an environment source a fully metallic helmet would be black.
	vec3 environment = ambient;
	if (HasFeature(RF_SPRITE_REFLECTIONS))
	{
		float coverage;
		vec3 probe = SampleSpriteEnvironment(pixelpos.xyz, reflect(-viewdir, normal), roughness, coverage);
		if (coverage > 0.0001) environment = probe;
	}
	// Analytic split-sum environment BRDF approximation (Lazarov 2013).
	vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
	float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
	vec2 brdf = vec2(-1.04, 1.04) * a004 + r.zw;
	color += environment * max(f0 * brdf.x + brdf.y, vec3(0.0)) * ao;
#if !defined(LEGACY_USER_SHADER) && !defined(NO_LAYERS)
	// Emissive eyes, lamps and other authored bright texels retain their
	// brightness even when their underlying material happens to be metallic.
	color = max(color, albedo * pow(clamp(material.Bright.rgb, 0.0, 1.0), vec3(2.2)));
	color = mix(color, max(color, albedo * pow(max(material.Glow.rgb, vec3(0.0)), vec3(2.2))), clamp(material.Glow.a, 0.0, 1.0));
#endif
	return pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
}

vec3 SpriteDebugMaterial(Material material)
{
	float metallic, roughness, ao;
	vec3 f0;
	SpriteMaterialProperties(material, metallic, roughness, ao, f0);
	int mode = int(uMaterialParams.y + 0.5);
	if (mode == 1) return material.Normal * 0.5 + 0.5;
	if (mode == 2) return vec3(roughness);
	if (mode == 3) return vec3(metallic);
	if (mode == 4) return vec3(ao);
	return material.Base.rgb;
}

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{
#ifdef NPOT_EMULATION
	if (uNpotEmulation.y != 0.0)
	{
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
	}
#endif	
	if (HasFeature(RF_PARALLAX)) texCoord = ParallaxTexCoord(texCoord);
	gTexCoord = texCoord.st;
	material.Base = getTexel(texCoord.st); 
	material.Normal = ApplyNormalMap(texCoord.st);
#if !defined(NORMALMAP) && !defined(NO_LAYERS)
	vec2 stdx = dFdx(texCoord.st), stdy = dFdy(texCoord.st);
	if (HasFeature(RF_AUTO_NORMAL) && (uLightIndex >= 0 || SunActive()) && HasSurfaceNormal())
		material.Normal = AutoNormal(normalize(vWorldNormal.xyz), texCoord.st, stdx, stdy);
#endif

// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
#ifndef NO_LAYERS
	if ((uTextureMode & TEXF_Brightmap) != 0)
	{
		if (uMipBias != 0.0) material.Bright = desaturate(texture(brighttexture, texCoord.st, uMipBias));
		else material.Bright = desaturate(texture(brighttexture, texCoord.st));
	}

	if ((uTextureMode & TEXF_Detailmap) != 0)
	{
		vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base.rgb *= Detail.rgb;
	}

	if ((uTextureMode & TEXF_Glowmap) != 0)
		material.Glow = desaturate(texture(glowtexture, texCoord.st));
#endif
}

//===========================================================================
//
// Wet look and reflection parameters. Wetness darkens and saturates the
// albedo and raises gloss; puddles are near-mirrors with a flat surface.
// The reflection normal follows the texture relief, toned down so
// reflections don't get noisy, and rain ripples where it rains.
//
//===========================================================================

void SetupReflection(inout Material material)
{
	if (IsPBRSprite() || !HasFeature(RF_MODERN_LIGHT)) return;

	float up = UpFacing();
	float lum = grayscale(material.Base);
	gPuddle = PuddleMask(lum);
	float wet = max(clamp(uMaterialParams2.x, 0.0, 1.0), gPuddle);
	gWetness = wet;
	if (wet > 0.0)
	{
		vec3 base = material.Base.rgb;
		base = pow(max(base, vec3(0.0)), vec3(1.0 + 0.6 * wet)) * (1.0 - 0.12 * wet);
		material.Base.rgb = mix(base, base * vec3(0.92, 0.96, 1.04), gPuddle * 0.5);
	}

	if (!HasFeature(RF_REFLECTIONS)) return;

	float reflectivity = uMaterialParams.x;
	float roughness = 1.0 - clamp(uMaterialParams.y, 0.0, 1.0);
#ifdef PBR
	roughness = clamp(material.Roughness, 0.0, 1.0);
#endif
	// Without an authored roughness, brighter and smoother texels are glossier.
	roughness = clamp(roughness + (0.4 - lum) * 0.15 * (1.0 - uMaterialParams.y), 0.0, 1.0);
	reflectivity = max(reflectivity, wet * 0.35 * up);
	roughness = min(roughness, mix(1.0, 0.35, wet * up));
	reflectivity = mix(reflectivity, 0.85, gPuddle);
	roughness = mix(roughness, 0.04, gPuddle);
	gReflectivity = reflectivity;
	gRoughness = roughness;

	if (reflectivity <= 0.0 || !HasSurfaceNormal()) return;

	vec3 geom = normalize(vWorldNormal.xyz);
	vec3 n = material.Normal;
#if !defined(NORMALMAP) && !defined(NO_LAYERS)
	if (HasFeature(RF_AUTO_NORMAL) && uLightIndex < 0 && !SunActive())
		n = AutoNormal(geom, gTexCoord, dFdx(gTexCoord), dFdy(gTexCoord));
#endif
	if (dot(n, n) < 0.5) n = geom;
	n = normalize(mix(geom, n, 0.35 * (1.0 - gPuddle)));

	float ripples = uMaterialParams2.z * up * max(wet, 0.25);
	if (ripples > 0.0)
	{
		vec2 g = RainRipples(pixelpos.xz, timer);
		n = normalize(n + vec3(g.x, 0.0, g.y) * (0.35 * ripples));
	}
	gReflectNormal = n;
}

#ifdef SUN_LIGHT
//===========================================================================
//
// Directional sun (hw_sun.h). The shadow map holds, per texel, the depth along
// the sunlight of the surface nearest to the sun. It is read with texelFetch
// and box filtered here over every texel the kernel touches. The receiver is
// moved off its surface along the geometric normal by an amount that grows
// with the filter radius and the slope to the sun, which keeps lit surfaces
// free of acne without detaching contact shadows.
//
//===========================================================================

// Fraction of the sun reaching pos (x, height, y). normal is the geometric
// normal and nl its cosine to the sun; a zero normal (sprites) moves toward the sun.
float SunShadow(vec3 pos, vec3 normal, float nl)
{
	if (uSunParams.x <= 0.0) return 1.0;
	float texel = uSunParams.w;
	float radius = clamp(uSunParams.z, 0.0, 8.0);
	vec3 p;
	if (dot(normal, normal) > 0.5)
	{
		float sinTheta = sqrt(max(1.0 - nl * nl, 0.0));
		p = pos + normal * (texel * ((radius + 0.75) * sinTheta + 0.3));
	}
	else p = pos + uSunDir.xyz * texel;
	vec4 q = vec4(p, 1.0);
	vec2 uv = vec2(dot(uSunShadowU, q), dot(uSunShadowV, q));
	if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= uSunParams.x || uv.y >= uSunParams.y) return 1.0;
	float depth = dot(uSunShadowD, q) - texel * 0.3;
	ivec2 last = ivec2(uSunParams.xy) - 1;
	float r = max(radius, 0.5);

	// Most kernels are entirely lit or entirely shadowed. Any shadow edge that
	// crosses the kernel separates two of its corners, and the centre catches
	// thin bars, so those decide; wide kernels also check the edge midpoints.
	ivec2 c0 = clamp(ivec2(floor(uv - r)), ivec2(0), last), c1 = clamp(ivec2(floor(uv + r)), ivec2(0), last);
	ivec2 cm = clamp(ivec2(floor(uv)), ivec2(0), last);
	float probe = step(depth, texelFetch(SunShadowMap, cm, 0).r) +
		step(depth, texelFetch(SunShadowMap, c0, 0).r) + step(depth, texelFetch(SunShadowMap, c1, 0).r) +
		step(depth, texelFetch(SunShadowMap, ivec2(c0.x, c1.y), 0).r) + step(depth, texelFetch(SunShadowMap, ivec2(c1.x, c0.y), 0).r);
	float probes = 5.0;
	if (r > 2.5)
	{
		probe += step(depth, texelFetch(SunShadowMap, ivec2(cm.x, c0.y), 0).r) + step(depth, texelFetch(SunShadowMap, ivec2(cm.x, c1.y), 0).r) +
			step(depth, texelFetch(SunShadowMap, ivec2(c0.x, cm.y), 0).r) + step(depth, texelFetch(SunShadowMap, ivec2(c1.x, cm.y), 0).r);
		probes = 9.0;
	}
	if (probe < 0.5) return 0.0;
	if (probe > probes - 0.5) return 1.0;

	// Box filter of half-width r around uv: each texel weighs by its overlap.
	// Up to 8x8 texels are read; wider kernels take every other texel or so.
	float stride = max(1.0, ceil((2.0 * r + 1.0) / 8.0));
	vec2 lo = uv - r, hi = uv + r;
	vec2 first = floor(lo / stride) * stride;
	float lit = 0.0, total = 0.0;
	for (int y = 0; y < 8; y++)
	{
		float ty = first.y + float(y) * stride;
		if (ty >= hi.y) break;
		float wy = min(hi.y, ty + stride) - max(lo.y, ty);
		for (int x = 0; x < 8; x++)
		{
			float tx = first.x + float(x) * stride;
			if (tx >= hi.x) break;
			float w = wy * (min(hi.x, tx + stride) - max(lo.x, tx));
			ivec2 t = clamp(ivec2(vec2(tx, ty) + 0.5 * stride), ivec2(0), last);
			lit += w * step(depth, texelFetch(SunShadowMap, t, 0).r);
			total += w;
		}
	}
	return total > 0.0 ? lit / total : 1.0;
}

// The sun's highlight per unit of sun colour, in the units of the diffuse term
// (albedo * N.L, which has no 1/pi): pi * GGX BRDF * N.L. Wet surfaces stretch
// it toward the viewer like the dynamic light highlights.
vec3 SunSpecular(vec3 n, vec3 L, vec3 V, float roughness, vec3 f0, float stretch)
{
	float nl = dot(n, L);
	if (nl <= 0.0) return vec3(0.0);
	vec3 h = normalize(L + V);
	if (stretch > 0.0)
	{
		vec3 t = V - n * dot(V, n);
		float tl = length(t);
		if (tl > 1e-3)
		{
			t /= tl;
			h = normalize(h - t * dot(h, t) * stretch);
		}
	}
	float nh = max(dot(n, h), 0.0);
	float nv = max(dot(n, V), 0.03);
	float a = roughness * roughness;
	float a2 = a * a;
	float den = nh * nh * (a2 - 1.0) + 1.0;
	float D = a2 / max(3.14159265 * den * den, 1e-6);
	float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
	float G = nv / (nv * (1.0 - k) + k) * nl / (nl * (1.0 - k) + k);
	vec3 F = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(h, V), 0.0, 1.0), 5.0);
	return min(F * (D * G * 3.14159265 / (4.0 * nv)), vec3(16.0));
}

// The sun's light on this fragment, linear, before fog. main() evaluates it after
// the scene lighting, from values that are still live there (the material, the
// reflection setup) or cheap to get again (varyings, uniforms), which keeps the
// sun variant's register use close to that of the plain shader.
vec3 SunLight(Material material)
{
	vec3 L = uSunDir.xyz;
	bool hud = uSunParams2.y > 0.5;
	vec3 albedo = max(material.Base.rgb, vec3(0.0));
	albedo *= albedo;
	vec3 light;
	if (IsPBRSprite())
	{
		// Authored sprite materials: their normal maps with the shallow wrap of
		// their dynamic lights, and their own roughness and metalness.
		float metallic, roughness, ao;
		vec3 f0;
		SpriteMaterialProperties(material, metallic, roughness, ao, f0);
		vec3 n = SafeNormal(material.Normal, SafeNormal(vWorldNormal.xyz, vec3(0.0, 1.0, 0.0)));
		float wrap = clamp((dot(n, L) + 0.12) / 1.12, 0.0, 1.0);
		if (wrap <= 0.0) return vec3(0.0);
		float shadow = hud ? 1.0 : SunShadow(pixelpos.xyz, vec3(0.0), 1.0);
		if (shadow <= 0.0) return vec3(0.0);
		vec3 V = hud ? vec3(0.0, 0.0, 1.0) : SafeNormal(uCameraPos.xyz - pixelpos.xyz, n);
		vec3 spec = SunSpecular(n, L, V, max(roughness, 0.1), f0, 0.0);
		light = (albedo * ((1.0 - metallic) * wrap * ao) + spec * uSunColor.w) * shadow;
	}
	else if (HasSurfaceNormal())
	{
		vec3 geom = normalize(vWorldNormal.xyz);
		float gnl = dot(geom, L);
		if (gnl <= 0.0) return vec3(0.0);
		// The material normal carries normal maps and the automatic relief.
		vec3 n = dot(material.Normal, material.Normal) > 0.5 ? material.Normal : geom;
		// Relief may not light what faces away from the sun, nor fully darken a lit face.
		float nl = clamp(dot(n, L), gnl * 0.25, 1.0) * smoothstep(0.0, 0.05, gnl);
		float shadow = hud ? 1.0 : SunShadow(pixelpos.xyz, geom, gnl);
		if (shadow <= 0.0) return vec3(0.0);

		// Roughness from the reflection setup, which has the water film: wet
		// surfaces gloss up and puddles mirror the sun's disc.
		float metallic = 0.0;
		float roughness = HasFeature(RF_REFLECTIONS) ? gRoughness : mix(0.8, 0.35, clamp(uMaterialParams.y, 0.0, 1.0));
		vec3 f0 = vec3(0.04);
#if defined(PBR)
		metallic = clamp(material.Metallic, 0.0, 1.0);
		roughness = min(roughness, clamp(material.Roughness, 0.05, 1.0));
		f0 = mix(vec3(0.04), albedo, metallic);
#elif defined(SPECULAR)
		roughness = min(roughness, clamp(pow(2.0 / (max(material.Glossiness * 4.0, 0.0) + 2.0), 0.25), 0.1, 1.0));
		f0 = clamp(material.Specular * material.SpecularLevel, vec3(0.02), vec3(0.98));
#endif
		roughness = max(roughness, 0.1);
		float wet = clamp(uMaterialParams2.x, 0.0, 1.0) * UpFacing();
		f0 = mix(f0, vec3(0.02), wet * (1.0 - metallic));
		vec3 specNormal = gReflectivity > 0.0 && dot(gReflectNormal, gReflectNormal) > 0.5 ? gReflectNormal : n;
		vec3 spec = vec3(0.0);
		if (!hud && uSunColor.w > 0.0)
		{
			vec3 V = normalize(uCameraPos.xyz - pixelpos.xyz);
			spec = SunSpecular(specNormal, L, V, roughness, f0, 0.65 * wet) * uSunColor.w * smoothstep(0.0, 0.1, gnl);
		}
		light = (albedo * (nl * (1.0 - metallic)) + spec) * shadow;
	}
	else
	{
		// Classic sprites and the HUD weapon: no surface normal. A mild wrap
		// toward the camera keeps back-lit sprites darker than front-lit ones.
		float f = uSunParams2.x;
		if (f <= 0.0)
		{
			vec2 toCamera = uCameraPos.xz - pixelpos.xz;
			float c = dot(L.xz, toCamera / max(length(toCamera), 1e-3));
			f = 0.675 + 0.325 * c;
		}
		float shadow = hud ? 1.0 : SunShadow(pixelpos.xyz, vec3(0.0), 1.0);
		light = albedo * (f * shadow);
	}
	return max(light * uSunColor.rgb, vec3(0.0));
}

// Adds the sun to a scene fragment (gamma 2 encoded) after the light clamps.
vec3 AddSun(vec3 frag, vec3 light, float fogfactor)
{
	// Black fog dims the light toward the distance (light modes without a light level).
	if (uFogEnabled > 0 && uLightLevel < 0.0) light *= fogfactor * fogfactor;
	frag = max(frag, vec3(0.0));
	return sqrt(frag * frag + light);
}
#endif

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
//
//===========================================================================

//===========================================================================
//
// HDR emission of brightmapped texels and of the bright texels of fullbright
// sprites. uDetailParms.w is the material's GLDEFS 'emissive' factor.
//
//===========================================================================

vec3 EmissiveColor(Material material)
{
	vec3 mask = material.Bright.rgb;
	if (uSurfaceParams.y > 0.0)
	{
		// Fullbright sprites without a brightmap: their bright, lamp-like texels emit.
		float peak = max(max(material.Base.r, material.Base.g), material.Base.b);
		mask = max(mask, vec3(smoothstep(0.8, 1.0, peak)));
	}
	return material.Base.rgb * mask * (uEmissiveStrength * sqrt(max(uDetailParms.w, 0.0)));
}

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;

	vec3 giTint = vec3(1.0);
	if (uLightLevel >= 0.0)
	{
		float lightlevel = uLightLevel;
		if (HasFeature(RF_IRRADIANCE)) ApplyIrradiance(lightlevel, giTint, material.Normal);
		float newlightlevel = 1.0 - R_DoomLightingEquation(lightlevel);
		color.rgb *= newlightlevel * giTint;
	}
	else if (uFogEnabled > 0)
	{
		if (HasFeature(RF_IRRADIANCE))
		{
			if (IsPBRSprite()) ApplySpriteVertexIrradiance(color.rgb, material.Normal);
			else
			{
				float unused = 0.0;
				ApplyIrradiance(unused, giTint, material.Normal);
				color.rgb *= giTint;
			}
		}

		// brightening around the player for light mode 2
		if (fogdist < uLightDist)
		{
			color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
		}

		//
		// apply light diminishing through fog equation
		//
		color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
	}
	else if (IsPBRSprite() && HasFeature(RF_IRRADIANCE))
	{
		ApplySpriteVertexIrradiance(color.rgb, material.Normal);
	}

	//
	// handle glowing walls
	//
	if (uGlowTopColor.a > 0.0 && glowdist.x < uGlowTopColor.a)
	{
		color.rgb += desaturate(uGlowTopColor * (1.0 - glowdist.x / uGlowTopColor.a)).rgb;
	}
	if (uGlowBottomColor.a > 0.0 && glowdist.y < uGlowBottomColor.a)
	{
		color.rgb += desaturate(uGlowBottomColor * (1.0 - glowdist.y / uGlowBottomColor.a)).rgb;
	}
	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
	//
	// apply glow 
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);

	//
	// apply brightmaps 
	//
	color.rgb = min(color.rgb + material.Bright.rgb, 1.0);
#endif

	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
	color = ProcessLight(material, color);

	//
	// apply lightmaps
	//
	if (vLightmap.z >= 0.0)
	{
		color.rgb += texture(LightMap, vLightmap).rgb;
	}

	//
	// apply dynamic lights
	//
	vec3 frag = IsPBRSprite() ? ProcessSpriteMaterialLight(material, color.rgb) : ProcessMaterialLight(material, color.rgb);

	//
	// HDR emission: bright texels may exceed white so they drive bloom. With a
	// strength of 1 this is exactly the classic brightmap result.
	//
	if (HasFeature(RF_HDR_EMISSIVE))
		frag = max(frag, EmissiveColor(material));

	return vec4(frag, material.Base.a * vColor.a);
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(uFogColor.rgb, frag.rgb, fogfactor), frag.a);
}

//===========================================================================
//
// The color of the fragment if it is fully occluded by ambient lighting
//
//===========================================================================

vec3 AmbientOcclusionColor()
{
	float fogdist;
	float fogfactor;

	//
	// calculate fog factor
	//
	if (uFogEnabled == -1) 
	{
		fogdist = max(16.0, pixelpos.w);
	}
	else 
	{
		fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	}
	if (uThickFogDistance > 0.0)
	{
		if (fogdist > uThickFogDistance)
		{
			fogdist = fogdist + uThickFogMultiplier * (fogdist - uThickFogDistance);
		}
	}
	fogfactor = exp2 (uFogDensity * fogdist);

	return mix(uFogColor.rgb, vec3(0.0), fogfactor);
}

//===========================================================================
//
// Main shader routine
//
//===========================================================================

// GPU particle shapes (hw_gpuparticles.cpp). vTexCoord = corner (-1..1), seed, shape.
float ParticleHash(vec2 p)
{
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float ParticleNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(ParticleHash(i), ParticleHash(i + vec2(1.0, 0.0)), f.x),
		mix(ParticleHash(i + vec2(0.0, 1.0)), ParticleHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Exact optical depth of rho = extinction * (1 - dot(local, local))^2
// inside unit ellipsoid support. The ray parameter is eye-space distance;
// the caller converts extinction from world units using the view metric.
float VolumeChordIntegral(vec3 p, vec3 b, float nearT, float farT, float extinction)
{
	float aa = dot(b, b);
	if (aa <= 1.0e-20 || extinction <= 0.0 || farT <= nearT) return 0.0;
	float tc = -dot(p, b) / aa;
	vec3 closest = p + tc * b;
	float h = 1.0 - dot(closest, closest);
	if (h <= 0.0) return 0.0;
	float halfChord = sqrt(h / aa);
	float lo = max(-1.0, (nearT - tc) / halfChord);
	float hi = min(1.0, (farT - tc) / halfChord);
	if (hi <= lo) return 0.0;
	// A symmetric polynomial difference stays positive in very short clips.
	float mid = (lo + hi) * 0.5, delta = (hi - lo) * 0.5;
	float m2 = mid * mid, d2 = delta * delta;
	float integral = 2.0 * delta * ((1.0 - m2) * (1.0 - m2) + d2 * (-2.0 + 6.0 * m2) / 3.0 + d2 * d2 / 5.0);
	return max(0.0, extinction * h * h * halfChord * integral);
}

bool ClipVolumeHalfspace(float originDistance, float raySlope, inout float nearT, inout float farT)
{
	if (abs(raySlope) < 0.0000001) return originDistance >= 0.0;
	float crossing = -originDistance / raySlope;
	if (raySlope > 0.0) nearT = max(nearT, crossing);
	else farT = min(farT, crossing);
	return farT > nearT;
}

float VolumeWorldRaySpeed(vec3 ray, vec3 floorRow, float metricDelta)
{
	float verticalSpeed = dot(floorRow, ray);
	return sqrt(max(1.0 + metricDelta * verticalSpeed * verticalSpeed, 1.0e-12));
}

vec4 VolumeParticleColor()
{
	vec3 ray = normalize(gradientdist);
	float nearT = 0.0, farT = 1.0e10;
	if (uParticleParams.y > 0.0)
	{
		ivec2 size = textureSize(SceneLinearDepth, 0);
		ivec2 texel = clamp(ivec2(gl_FragCoord.xy * uSceneDepthMap.xy + uSceneDepthMap.zw), ivec2(0), size - 1);
		float sceneDepth = texelFetch(SceneLinearDepth, texel, 0).r;
		farT = sceneDepth / max(-ray.z, 0.000001);
	}
	if (!ClipVolumeHalfspace(vWorldNormal.w, dot(vWorldNormal.xyz, ray), nearT, farT)
		|| !ClipVolumeHalfspace(vVolumeParams.y, dot(vLightmap, ray), nearT, farT)) return vec4(0.0);
	vec3 p = vec3(vVolumeRow0.w, vVolumeRow1.w, vVolumeRow2.w);
	vec3 b = vec3(dot(vVolumeRow0.xyz, ray), dot(vVolumeRow1.xyz, ray), dot(vVolumeRow2.xyz, ray));
	float worldSpeed = VolumeWorldRaySpeed(ray, vLightmap, vVolumeParams.w);
	float tau = VolumeChordIntegral(p, b, nearT, farT, vVolumeParams.x * worldSpeed);
	return vec4(vColor.rgb, 1.0 - exp(-min(tau, 80.0)));
}

vec4 ParticleColor()
{
	vec2 c = vTexCoord.xy;
	float seed = vTexCoord.z;
	float shape = vTexCoord.w;
	if (shape > 11.5 && shape < 13.5) return VolumeParticleColor();
	float r2 = dot(c, c);
	float a;
	vec3 rgb = vColor.rgb;
	if (shape < 0.5) // glow: hot core, soft halo
	{
		a = exp(-4.0 * r2) * (1.0 - smoothstep(0.7, 1.0, r2));
		rgb *= 1.0 + 1.5 * exp(-16.0 * r2);
	}
	else if (shape < 1.5) // smoke: billowing edge, lit from above
	{
		if (uMaterialParams.z > 0.5) // retain the existing additive muzzle-flame profile
		{
			vec2 p = c * 2.2 + seed * 37.0 + vec2(0.18, -0.3) * vLightmap.y;
			float n = ParticleNoise(p) * 0.6 + ParticleNoise(p * 2.3 + 11.0) * 0.4;
			a = 1.0 - smoothstep(0.25, 1.0, r2 + (n - 0.5) * 0.7);
			rgb *= 0.8 + 0.35 * (c.y * 0.5 + 0.5) + 0.15 * (n - 0.5);
		}
		else
		{
			// Counter-moving density scales give each puff rolling lobes and dark
			// folds. Reuse two noise samples: cloud volume needs no extra textures,
			// lighting passes or per-fragment noise octaves.
			vec2 drift = vec2(0.24, -0.38) * vLightmap.y;
			vec2 p = c * 2.2 + seed * 37.0;
			float broad = ParticleNoise(p + drift);
			float fine = ParticleNoise(p * 2.3 + 11.0 - drift * 1.3);
			float n = broad * 0.65 + fine * 0.35;
			a = (1.0 - smoothstep(0.18, 1.0, r2 + (n - 0.5) * 0.8)) *
				mix(0.68, 1.0, smoothstep(0.2, 0.8, n));
			rgb *= 0.64 + 0.4 * (c.y * 0.5 + 0.5) + 0.28 * broad;
		}
	}
	else if (shape < 2.5) // chunk: irregular hard-edged polygon with a shaded face
	{
		// Distinct triangular splinters, thin slabs and broken five-sided
		// aggregate. Per-chip skew and aspect keep debris from reading as a
		// cloud of identical square confetti. Only cheap fracture planes are
		// evaluated here; no per-fragment angle or noise lookup is needed.
		float variation = fract(seed * 17.37);
		vec2 q = c;
		q.x += q.y * (fract(seed * 29.13) - 0.5) * 0.7;
		q.y *= 0.85 + fract(seed * 53.71) * 0.65;
		float edge;
		if (variation < 0.4)
			edge = max(-q.y, max(dot(q, vec2(0.95, 0.52)), dot(q, vec2(-0.72, 0.68))));
		else if (variation < 0.7)
		{
			q.y *= 1.65;
			edge = max(max(q.x + q.y * 0.16, -q.x + q.y * 0.25), max(q.y * 0.85, -q.y));
			edge = max(edge, dot(q, vec2(0.76, 0.73)));
		}
		else
		{
			edge = max(max(dot(q, vec2(0.85, 0.45)), dot(q, vec2(-0.7, 0.65))),
				max(dot(q, vec2(0.6, -0.8)), dot(q, vec2(-0.8, -0.4))));
			edge = max(edge, q.y * 0.96 + q.x * 0.08);
		}
		float limit = 0.53 + fract(seed * 41.3) * 0.22;
		float aa = max(fwidth(edge), 0.012);
		a = 1.0 - smoothstep(limit - aa, limit + aa, edge);
		float face = dot(q, vec2(-0.45, 0.8));
		float facet = face > 0.18 ? 1.04 : q.x + q.y * 0.35 > seed * 0.3 ? 0.86 : 0.65;
		float bevel = smoothstep(limit - 0.045, limit - 0.01, edge) * 0.3;
		rgb *= mix(facet, 0.98, bevel) * (0.78 + vLightmap.z * 0.22);
	}
	else if (shape < 3.5) // droplet: dense with a specular glint
	{
		a = 1.0 - smoothstep(0.55, 1.0, r2);
		rgb += vec3(0.5) * exp(-30.0 * dot(c - vec2(-0.3, 0.3), c - vec2(-0.3, 0.3))) * vColor.a;
	}
	else if (shape < 4.5 || (shape > 8.5 && shape < 9.5)) // ring: shock waves, foam, rain ripples
	{
		float r = sqrt(r2);
		a = smoothstep(0.55, 0.85, r) * (1.0 - smoothstep(0.88, 1.0, r));
	}
	else if (shape < 5.5 || shape > 10.5) // mote, snow flake: small soft disc
	{
		a = exp(-3.5 * r2) * (1.0 - smoothstep(0.7, 1.0, r2));
	}
	else if (shape < 8.5) // rain streak: c.x across, c.y from tail (-1) to head (1)
	{
		a = (1.0 - c.x * c.x) * (0.3 + 0.7 * smoothstep(-1.0, 0.6, c.y));
	}
	else // splash: droplets thrown up in a crown
	{
		vec2 q = c - vec2(0.0, -1.0);
		float ang = atan(q.x, q.y);
		float crown = smoothstep(0.3, 0.0, abs(length(q) - 0.85)) * step(abs(ang), 1.2);
		a = crown * (0.5 + 0.5 * sin(ang * 9.0 + seed * 20.0));
	}
	// Soft intersection with the surface the particle came from and the floor.
	float soft = glowdist.y;
	a *= clamp((pixelpos.y - glowdist.x) / soft, 0.0, 1.0);
	a *= clamp((dot(vWorldNormal.xyz, pixelpos.xyz) - vWorldNormal.w) / soft, 0.0, 1.0);
	return vec4(rgb, a * vColor.a);
}

void main()
{
#ifdef NO_CLIPDISTANCE_SUPPORT
	if (ClipDistanceA.x < 0 || ClipDistanceA.y < 0 || ClipDistanceA.z < 0 || ClipDistanceA.w < 0 || ClipDistanceB.x < 0) discard;
#endif

	if (uMaterialParams.w > 0.0)
	{
		vec4 frag = ParticleColor();
		bool weather = uMaterialParams.z > 1.5;
		bool additive = uMaterialParams.z > 0.5 && uMaterialParams.z < 2.5;
		// Soft against the opaque scene: per draw for the weather, scaled by size
		// for particles. Solid chunks and droplets stay hard so they can lie on the floor.
		bool solid = vTexCoord.w > 1.5 && vTexCoord.w < 3.5;
		bool volume = vTexCoord.w > 11.5 && vTexCoord.w < 13.5;
		if (!volume) frag.a *= SoftDepthFade(weather ? uParticleParams2.w * vLightmap.x : (uParticleParams.x > 0.0 && !solid ? max(glowdist.y * 2.0, uParticleParams.x) : 0.0));
		if (uFogEnabled != 0)
		{
			float fogfactor = exp2(uFogDensity * max(16.0, pixelpos.w));
			if (additive || uFogEnabled > 0) frag.rgb *= fogfactor; // additive light and black fog fade out
			else frag.rgb = mix(uFogColor.rgb, frag.rgb, fogfactor);
		}
		FragColor = frag;
		if (FragColor.a <= 0.002) discard;
#ifdef GBUFFER_PASS
		// Particles hide reflections behind dense parts; additive ones never do.
		// Screen-space reflections run after this, so the normals of what is
		// behind must survive: alpha-blended particles only replace them where
		// they cover, additive ones add nothing to them and only raise the TAA
		// reactive mask in the alpha (weather by its density, particles fully).
		float cover = (uMaterialParams.z < 0.5 && FragColor.a >= 0.5) ? 1.0 : 0.0;
		FragFog = vec4(AmbientOcclusionColor(), cover);
		if (additive) FragNormal = vec4(0.0, 0.0, 0.0, weather ? FragColor.a : 1.0);
		else FragNormal = vec4(vEyeNormal.xyz * 0.5 + 0.5, cover);
#endif
		return;
	}

	gPuddle = !IsPBRSprite() && HasFeature(RF_MODERN_LIGHT) ? PuddleMask(0.5) : 0.0;

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	SetupReflection(material);
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

	if (uFogEnabled != -3)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;

		//
		// calculate fog factor
		//
		if (uFogEnabled != 0)
		{
			if (uFogEnabled == 1 || uFogEnabled == -1) 
			{
				fogdist = max(16.0, pixelpos.w);
			}
			else 
			{
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			}
			if (uThickFogDistance > 0.0)
			{
				if (fogdist > uThickFogDistance)
				{
					fogdist = fogdist + uThickFogMultiplier * (fogdist - uThickFogDistance);
				}
			}
			fogfactor = exp2 (uFogDensity * fogdist);
		}

		if ((uTextureMode & 0xffff) != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

#ifdef SUN_LIGHT
			// The sun, in linear space after the light clamps like the lightning below.
			if (SunActive()) frag.rgb = AddSun(frag.rgb, SunLight(material), fogfactor);
#endif

			// Lightning: a brief sky light on everything under open sky. It is
			// added after the light clamps so that a strike can reach the bloom.
			if (uWeatherParams.y > 0.0)
			{
				vec3 probe = pixelpos.xyz + (HasSurfaceNormal() ? normalize(vWorldNormal.xyz) * 8.0 : vec3(0.0, 8.0, 0.0));
				frag.rgb += material.Base.rgb * (uWeatherParams.y * WeatherExposure(probe)) * vec3(0.8, 0.85, 1.0);
			}

			//
			// colored fog
			//
			if (uFogEnabled < 0) 
			{
				frag = applyFog(frag, fogfactor);
			}
		}
		else
		{
			frag = vec4(uFogColor.rgb, (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
	}
	else // simple 2D (uses the fog color to add a color overlay)
	{
		if ((uTextureMode & 0xffff) == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
			frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	if (uParticleParams.x > 0.0) frag.a *= SoftDepthFade(uParticleParams.x);
	if (IsPBRSprite() && uMaterialParams.y > 0.5)
		frag.rgb = SpriteDebugMaterial(material);
	FragColor = frag;

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 8) * 8 + int(pixelpos.y) % 8;
	const float DITHER_THRESHOLDS[64] =
	float[64](
		1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
		49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
		13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
		61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
		4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
		52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
		16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
		64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 /65.0
	);

	vec3 fragHSV = rgb2hsv(FragColor.rgb);
	float brightness = clamp(1.5*fragHSV.z, 0.1, 1.0);
	if (DITHER_THRESHOLDS[index] < brightness) discard;
	else FragColor *= 0.5;
#endif

#ifdef GBUFFER_PASS
	// Cleared pixels (sky, void) keep alpha 0 and never reflect. Translucent
	// surfaces only mark what they cover as not reflective (see EGBufferMode).
	if (uMaterialParams2.w < 0.5)
	{
		vec3 eyeNormal = vEyeNormal.xyz;
		if (gReflectivity > 0.0 && dot(gReflectNormal, gReflectNormal) > 0.5)
			eyeNormal = normalize((NormalViewMatrix * vec4(gReflectNormal, 1.0)).xyz);
		// Sprite probe reflections are already shaded in the material pass.
		// Keep the current packed G-buffer format, with zero SSR reflectivity.
		if (IsPBRSprite())
			eyeNormal = SafeNormal((NormalViewMatrix * vec4(material.Normal, 0.0)).xyz, vec3(0.0, 0.0, 1.0));
		FragFog = vec4(AmbientOcclusionColor(), EncodeReflection(IsPBRSprite() ? 0.0 : gReflectivity, gRoughness));
		// The normal's alpha is the TAA reactive mask: 0 keeps history, 1 ignores it.
		FragNormal = vec4(eyeNormal * 0.5 + 0.5, uSurfaceParams.x);
	}
	else
	{
		// The normals behind must survive for the reflections: blended surfaces
		// only replace them where they cover (and those pixels no longer reflect),
		// and additive ones (GBUFFER_KEEP) add nothing to them and only raise the
		// reactive mask. Other blend ops in that mode have a reactive amount of 0.
		if (uMaterialParams2.w < 1.5)
		{
			float cover = FragColor.a >= 0.5 ? 1.0 : 0.0;
			FragFog = vec4(AmbientOcclusionColor(), cover);
			vec3 eyeNormal = IsPBRSprite() ? SafeNormal((NormalViewMatrix * vec4(material.Normal, 0.0)).xyz, vec3(0.0, 0.0, 1.0)) : vEyeNormal.xyz;
			FragNormal = vec4(eyeNormal * 0.5 + 0.5, cover * uSurfaceParams.x);
		}
		else
		{
			FragFog = vec4(0.0);
			FragNormal = vec4(0.0, 0.0, 0.0, uSurfaceParams.x);
		}
	}
#endif
}
