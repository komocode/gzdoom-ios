
vec3 lightContribution(int i, vec3 normal)
{
	vec4 lightpos = lights[i];
	vec4 lightcolor = lights[i+1];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	if (lightpos.w < lightdistance)
		return vec3(0.0); // Early out lights touching surface but not this fragment

	vec3 lightdir = normalize(lightpos.xyz - pixelpos.xyz);
	float dotprod = dot(normal, lightdir);
	if (dotprod < -0.0001) return vec3(0.0);	// light hits from the backside. This can happen with full sector light lists and must be rejected for all cases. Note that this can cause precision issues.

	float attenuation = clamp((lightpos.w - lightdistance) / lightpos.w, 0.0, 1.0);

	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);

	if (lightcolor.a < 0.0) // Sign bit is the attenuated light flag
	{
		attenuation *= clamp(dotprod, 0.0, 1.0);
	}

	if (attenuation > 0.0) // Skip shadow map test if possible
	{
		attenuation *= shadowAttenuation(lightpos, lightcolor.a);
		return lightcolor.rgb * attenuation;
	}
	else
	{
		return vec3(0.0);
	}
}

//===========================================================================
//
// Modern light model. Light intensities and falloff match the classic model
// so maps keep their intended brightness; texture relief modulates diffuse
// light and dynamic lights add specular highlights that may exceed 1.0 and
// feed the bloom pass.
//
//===========================================================================

struct ModernLight
{
	vec3 diffuse;
	vec3 specular;
};

void modernLightContribution(int i, vec3 geomNormal, vec3 normal, vec3 viewdir, float shininess, inout ModernLight result, float sign)
{
	vec4 lightpos = lights[i];
	vec4 lightcolor = lights[i+1];
	vec4 lightspot1 = lights[i+2];
	vec4 lightspot2 = lights[i+3];

	float lightdistance = distance(lightpos.xyz, pixelpos.xyz);
	if (lightpos.w < lightdistance)
		return;

	vec3 lightdir = (lightpos.xyz - pixelpos.xyz) / max(lightdistance, 0.001);
	float geomdot = dot(geomNormal, lightdir);
	if (geomdot < -0.0001) return;

	float attenuation = clamp((lightpos.w - lightdistance) / lightpos.w, 0.0, 1.0);
	if (lightspot1.w == 1.0)
		attenuation *= spotLightAttenuation(lightpos, lightspot1.xyz, lightspot2.x, lightspot2.y);

	float bumpdot = dot(normal, lightdir);
	if (lightcolor.a < 0.0)
	{
		attenuation *= clamp(bumpdot, 0.0, 1.0);
	}
	else
	{
		// Classic lights ignore the surface angle. Keep that brightness and
		// only modulate it by the relief relative to the flat surface.
		float relief = (bumpdot * 0.5 + 0.5) / max(geomdot * 0.5 + 0.5, 0.25);
		attenuation *= clamp(relief, 0.4, 1.6);
	}

	if (attenuation <= 0.0) return;
	attenuation *= shadowAttenuation(lightpos, lightcolor.a);

	vec3 radiance = lightcolor.rgb * attenuation;
	result.diffuse += radiance * sign;

	if (shininess > 0.0 && sign > 0.0)
	{
		// Normalized Blinn-Phong.
		vec3 halfdir = normalize(lightdir + viewdir);
		if (gWetness > 0.0)
		{
			// Wet surfaces stretch highlights toward the viewer: widen the lobe
			// along the view direction projected onto the surface.
			vec3 t = viewdir - normal * dot(viewdir, normal);
			float tl = length(t);
			if (tl > 1e-3)
			{
				t /= tl;
				halfdir = normalize(halfdir - t * dot(halfdir, t) * (0.65 * gWetness));
			}
		}
		float spec = pow(max(dot(normal, halfdir), 0.0), shininess) * (shininess + 8.0) * (1.0 / 25.1327);
		result.specular += radiance * spec * clamp(geomdot * 4.0, 0.0, 1.0);
	}
}

vec3 ProcessModernMaterialLight(Material material, vec3 color)
{
	vec4 dynlight = uDynLightColor;
	vec3 specular = vec3(0.0);
	vec3 frag;

	if (uLightIndex >= 0)
	{
		bool hasNormal = dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5;
		vec3 geomNormal = hasNormal ? normalize(vWorldNormal.xyz) : vec3(0.0);
		vec3 normal = hasNormal ? material.Normal : vec3(0.0);
		vec3 viewdir = normalize(uCameraPos.xyz - pixelpos.xyz);

		// Glossiness estimated from the texel: bright, desaturated texels (metal,
		// tile, marble) are shinier than dark or saturated ones (rock, flesh).
		float shininess = 0.0;
		float specLevel = 0.0;
		vec3 specTint = vec3(1.0);
		if (HasFeature(RF_SPECULAR) && hasNormal)
		{
			vec3 base = material.Base.rgb;
			float lum = grayscale(material.Base);
			float sat = max(max(base.r, base.g), base.b) - min(min(base.r, base.g), base.b);
			specLevel = uSpecularStrength * mix(0.08, 0.5, smoothstep(0.15, 0.75, lum)) * (1.0 - 0.6 * sat);
			specLevel = max(specLevel, uMaterialParams.x * uSpecularStrength);
			shininess = mix(12.0, 48.0, smoothstep(0.2, 0.8, lum)) + uMaterialParams.y * 64.0;
			// Estimated metalness: mid-to-bright texels with low or warm-metal
			// saturation (steel, brass, copper) tint their highlights with their
			// own colour, as PBR metals do. Dielectrics keep white highlights.
			float metallic = smoothstep(0.3, 0.6, lum) * (1.0 - smoothstep(0.25, 0.55, sat));
			specTint = mix(vec3(1.0), base / max(max(max(base.r, base.g), base.b), 0.05), metallic);
			specLevel *= 1.0 + 0.5 * metallic;
			// Water film: strong, tight highlights.
			specLevel = mix(specLevel, max(specLevel, 0.8 * uSpecularStrength), gWetness);
			shininess = mix(shininess, max(shininess, 140.0), gWetness);
		}

		ModernLight result;
		result.diffuse = vec3(0.0);
		result.specular = vec3(0.0);

		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.z > lightRange.x)
		{
			for (int i = lightRange.x; i < lightRange.y; i += 4)
				modernLightContribution(i, geomNormal, normal, viewdir, shininess, result, 1.0);
			for (int i = lightRange.y; i < lightRange.z; i += 4)
				modernLightContribution(i, geomNormal, normal, viewdir, 0.0, result, -1.0);
		}
		dynlight.rgb += result.diffuse;

		vec3 lightcolor = color + desaturate(dynlight).rgb;
		if (uLightBlendMode == 1)
			frag = material.Base.rgb * ((lightcolor / max(max(max(lightcolor.r, lightcolor.g), lightcolor.b), 1.4) * 1.4));
		else if (uLightBlendMode == 2)
			frag = material.Base.rgb * lightcolor;
		else
			frag = material.Base.rgb * clamp(lightcolor, 0.0, 1.4);

		if (lightRange.w > lightRange.z)
		{
			ModernLight additive;
			additive.diffuse = vec3(0.0);
			additive.specular = vec3(0.0);
			for (int i = lightRange.z; i < lightRange.w; i += 4)
				modernLightContribution(i, geomNormal, normal, viewdir, shininess, additive, 1.0);
			// With HDR emission, additive lights may push surfaces past white.
			frag += desaturate(vec4(additive.diffuse, 0.0)).rgb;
			frag = HasFeature(RF_HDR_EMISSIVE) ? max(frag, vec3(0.0)) : clamp(frag, 0.0, 1.0);
			result.specular += additive.specular;
		}

		// Highlights are not clamped so they can bloom.
		frag += desaturate(vec4(result.specular, 0.0)).rgb * specTint * specLevel;
	}
	else
	{
		vec3 lightcolor = color + desaturate(dynlight).rgb;
		if (uLightBlendMode == 1)
			frag = material.Base.rgb * ((lightcolor / max(max(max(lightcolor.r, lightcolor.g), lightcolor.b), 1.4) * 1.4));
		else if (uLightBlendMode == 2)
			frag = material.Base.rgb * lightcolor;
		else
			frag = material.Base.rgb * clamp(lightcolor, 0.0, 1.4);
	}
	return frag;
}

vec3 ProcessMaterialLight(Material material, vec3 color)
{
	if (HasFeature(RF_MODERN_LIGHT))
		return ProcessModernMaterialLight(material, color);

	vec4 dynlight = uDynLightColor;
	vec3 normal = material.Normal;

	if (uLightIndex >= 0)
	{
		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.z > lightRange.x)
		{
			// modulated lights
			for(int i=lightRange.x; i<lightRange.y; i+=4)
			{
				dynlight.rgb += lightContribution(i, normal);
			}

			// subtractive lights
			for(int i=lightRange.y; i<lightRange.z; i+=4)
			{
				dynlight.rgb -= lightContribution(i, normal);
			}
		}
	}

	vec3 frag;

	if ( uLightBlendMode == 1 )
	{	// COLOR_CORRECT_CLAMPING
		vec3 lightcolor = color + desaturate(dynlight).rgb;
		frag = material.Base.rgb * ((lightcolor / max(max(max(lightcolor.r, lightcolor.g), lightcolor.b), 1.4) * 1.4));
	}
	else if ( uLightBlendMode == 2 )
	{	// UNCLAMPED
		frag = material.Base.rgb * (color + desaturate(dynlight).rgb);
	}
	else
	{
		frag = material.Base.rgb * clamp(color + desaturate(dynlight).rgb, 0.0, 1.4);
	}

	if (uLightIndex >= 0)
	{
		ivec4 lightRange = ivec4(lights[uLightIndex]) + ivec4(uLightIndex + 1);
		if (lightRange.w > lightRange.z)
		{
			vec4 addlight = vec4(0.0,0.0,0.0,0.0);

			// additive lights
			for(int i=lightRange.z; i<lightRange.w; i+=4)
			{
				addlight.rgb += lightContribution(i, normal);
			}

			frag = clamp(frag + desaturate(addlight).rgb, 0.0, 1.0);
		}
	}

	return frag;
}
