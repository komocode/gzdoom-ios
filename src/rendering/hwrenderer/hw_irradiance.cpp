/*
** hw_irradiance.cpp
** Baked irradiance probes for indirect lighting. See hw_irradiance.h.
*/

#include "hw_irradiance.h"
#include "hw_modernrender.h"
#include "hw_viewpointuniforms.h"
#include "doom_levelmesh.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "texturemanager.h"
#include "bitmap.h"
#include "v_video.h"
#include "printf.h"
#include "i_time.h"
#include "actor.h"
#include "v_text.h"
#include "c_dispatch.h"
#include "a_lights.h"
#include "portal.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <pthread.h>
#endif

namespace
{

struct Vec3
{
	float x = 0, y = 0, z = 0;
	Vec3() = default;
	Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
	Vec3 operator+(const Vec3 &o) const { return { x + o.x, y + o.y, z + o.z }; }
	Vec3 operator-(const Vec3 &o) const { return { x - o.x, y - o.y, z - o.z }; }
	Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
	Vec3 operator*(const Vec3 &o) const { return { x * o.x, y * o.y, z * o.z }; }
	Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
	float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline float Dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3 &a, const Vec3 &b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Length(const Vec3 &v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3 &v) { float l = Length(v); return l > 0 ? v * (1.f / l) : v; }
inline Vec3 Min(const Vec3 &a, const Vec3 &b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
inline Vec3 Max(const Vec3 &a, const Vec3 &b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }
inline float Luminance(const Vec3 &c) { return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f; }

enum
{
	TRI_TWOSIDED = 1,	// masked mid textures and models: seen from both sides
	TRI_NOSHADOW = 2,	// models carrying lights don't shadow them
};

struct Triangle
{
	Vec3 v0, e1, e2, normal;
	int material;
	int flags;
};

// Directional radiance is kept separate from the cosine-filtered diffuse GI.
// Four roughness levels occupy bordered octahedral tiles in the same GPU array.
constexpr int ReflectionResolution = 16;
constexpr int ReflectionBordered = ReflectionResolution + 2;
constexpr int ReflectionTile = ReflectionBordered * 2;
constexpr int ReflectionLevels = 4;
constexpr int MaxReflectionProbes = 128;
constexpr size_t MaxReflectionTextureBytes = 16 * 1024 * 1024;

Vec3 LinearColor(const Vec3 &c)
{
	return { std::pow(std::max(c.x, 0.f), 2.2f), std::pow(std::max(c.y, 0.f), 2.2f), std::pow(std::max(c.z, 0.f), 2.2f) };
}

// Linear radiance (as LinearColor, power 2.2) that a sky lobe of the given
// peak and exponent adds over an encoded sky brightness b, integrated over
// the angle from its axis: the energy of MAPINFO gimoon in reflection probes.
float LobeLinearEnergy(float b, float peak, float exponent)
{
	const int steps = 256;
	// cos^n is below 1e-7 beyond about 6 / sqrt(n) radians.
	double range = std::min(M_PI / 2, 6.0 / std::sqrt(exponent)), base = std::pow(b, 2.2), sum = 0.0;
	for (int i = 0; i < steps; i++)
	{
		double theta = (i + 0.5) * range / steps;
		sum += (std::pow(b + peak * std::pow(std::cos(theta), exponent), 2.2) - base) * std::sin(theta);
	}
	return float(sum * range / steps);
}

Vec3 OctaDirection(float u, float v)
{
	Vec3 n(u, v, 1.f - std::abs(u) - std::abs(v));
	float fold = std::max(-n.z, 0.f);
	n.x += n.x >= 0.f ? -fold : fold;
	n.y += n.y >= 0.f ? -fold : fold;
	return Normalize(n);
}

struct ReflectionTexel
{
	Vec3 albedo, emissive;
	float lit = 1.f;
};

struct ReflectionTexture
{
	int width = 0, height = 0;
	std::vector<ReflectionTexel> pixels;
};

struct ReflectionProbe
{
	bool valid = false;
	std::vector<Vec3> pixels;
};

struct Material
{
	int reflectionTexture = -1;
	Vec3 textureU, textureV;
	float offsetU = 0.f, offsetV = 0.f;
	Vec3 albedo;
	Vec3 light;	// sector light level * light color
	Vec3 emissive;	// fullbright and brightmapped texels
	float lit;	// fraction of the texels that take the light (not brightmapped)
	bool sky;
};

struct StaticLight
{
	Vec3 pos;
	Vec3 color;	// GLDEFS intensity and alpha applied
	float radius;
	bool additive;	// added after the texture, like the scene shader does
	bool spot;
	Vec3 spotDir;	// from the surface towards the light, at the cone centre
	float spotInner, spotOuter;	// cosines
};

struct Hit
{
	float t;
	int tri;
};

// Bounding volume hierarchy over the level triangles. Built once per bake;
// nodes are stored depth-first so the left child always follows its parent.
class BVH
{
public:
	void Build(std::vector<Triangle> &tris)
	{
		triangles = &tris;
		indices.resize(tris.size());
		for (size_t i = 0; i < tris.size(); i++) indices[i] = (int)i;
		centroids.resize(tris.size());
		for (size_t i = 0; i < tris.size(); i++)
		{
			const auto &t = tris[i];
			centroids[i] = t.v0 + (t.e1 + t.e2) * (1.f / 3.f);
		}
		nodes.clear();
		nodes.reserve(tris.size() * 2);
		BuildNode(0, (int)tris.size());
		centroids.clear();
		centroids.shrink_to_fit();
	}

	bool Intersect(const Vec3 &origin, const Vec3 &dir, float maxT, Hit &hit, bool anyHit) const
	{
		if (nodes.empty()) return false;
		Vec3 inv = { 1.f / (std::abs(dir.x) > 1e-8f ? dir.x : 1e-8f), 1.f / (std::abs(dir.y) > 1e-8f ? dir.y : 1e-8f), 1.f / (std::abs(dir.z) > 1e-8f ? dir.z : 1e-8f) };
		int stack[64];
		int sp = 0;
		stack[sp++] = 0;
		hit.t = maxT;
		hit.tri = -1;
		while (sp > 0)
		{
			const Node &node = nodes[stack[--sp]];
			if (!SlabTest(node, origin, inv, hit.t)) continue;
			if (node.count > 0)
			{
				for (int i = 0; i < node.count; i++)
				{
					int ti = indices[node.start + i];
					float t;
					const Triangle &tri = (*triangles)[ti];
					if (anyHit && (tri.flags & TRI_NOSHADOW)) continue;
					if (IntersectTriangle(tri, origin, dir, t) && t < hit.t)
					{
						hit.t = t;
						hit.tri = ti;
						if (anyHit) return true;
					}
				}
			}
			else if (sp < 62)
			{
				int left = int(&node - nodes.data()) + 1, right = node.start;
				// Visit the nearer child first.
				if (dir[node.axis] > 0) { stack[sp++] = right; stack[sp++] = left; }
				else { stack[sp++] = left; stack[sp++] = right; }
			}
		}
		return hit.tri >= 0;
	}

private:
	struct Node
	{
		Vec3 bmin, bmax;
		int start;	// first triangle index for leaves, right child for inner nodes
		int count;	// 0 for inner nodes
		int axis;
	};

	std::vector<Node> nodes;
	std::vector<int> indices;
	std::vector<Vec3> centroids;
	std::vector<Triangle> *triangles = nullptr;

	int BuildNode(int begin, int end)
	{
		int index = (int)nodes.size();
		nodes.push_back({});
		Vec3 bmin = { 1e30f, 1e30f, 1e30f }, bmax = { -1e30f, -1e30f, -1e30f };
		Vec3 cmin = bmin, cmax = bmax;
		for (int i = begin; i < end; i++)
		{
			const auto &t = (*triangles)[indices[i]];
			Vec3 a = t.v0, b = t.v0 + t.e1, c = t.v0 + t.e2;
			bmin = Min(bmin, Min(a, Min(b, c)));
			bmax = Max(bmax, Max(a, Max(b, c)));
			cmin = Min(cmin, centroids[indices[i]]);
			cmax = Max(cmax, centroids[indices[i]]);
		}
		nodes[index].bmin = bmin - Vec3(0.01f, 0.01f, 0.01f);
		nodes[index].bmax = bmax + Vec3(0.01f, 0.01f, 0.01f);
		int count = end - begin;
		Vec3 extent = cmax - cmin;
		int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
		if (count <= 4 || extent[axis] < 1e-3f)
		{
			nodes[index].start = begin;
			nodes[index].count = count;
			nodes[index].axis = 0;
			return index;
		}
		int mid = (begin + end) / 2;
		std::nth_element(indices.begin() + begin, indices.begin() + mid, indices.begin() + end,
			[&](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });
		BuildNode(begin, mid);
		int right = BuildNode(mid, end);
		nodes[index].start = right;
		nodes[index].count = 0;
		nodes[index].axis = axis;
		return index;
	}

	static bool SlabTest(const Node &n, const Vec3 &o, const Vec3 &inv, float maxT)
	{
		float t0 = 0, t1 = maxT;
		for (int a = 0; a < 3; a++)
		{
			float near = (n.bmin[a] - o[a]) * inv[a];
			float far = (n.bmax[a] - o[a]) * inv[a];
			if (near > far) std::swap(near, far);
			t0 = std::max(t0, near);
			t1 = std::min(t1, far);
			if (t0 > t1) return false;
		}
		return true;
	}

	static bool IntersectTriangle(const Triangle &tri, const Vec3 &o, const Vec3 &d, float &t)
	{
		Vec3 p = Cross(d, tri.e2);
		float det = Dot(tri.e1, p);
		if (std::abs(det) < 1e-9f) return false;
		float invDet = 1.f / det;
		Vec3 s = o - tri.v0;
		float u = Dot(s, p) * invDet;
		if (u < 0.f || u > 1.f) return false;
		Vec3 q = Cross(s, tri.e1);
		float v = Dot(d, q) * invDet;
		if (v < 0.f || u + v > 1.f) return false;
		t = Dot(tri.e2, q) * invDet;
		return t > 0.05f;
	}
};

struct Probe
{
	Vec3 face[6];
	bool valid;
};

struct BakeJob
{
	// Captured scene
	std::vector<Triangle> triangles;
	std::vector<Material> materials;
	std::vector<StaticLight> lights;
	std::vector<ReflectionTexture> reflectionTextures;
	std::unordered_map<int, int> reflectionTextureIDs;
	size_t reflectionTextureBytes = 0;
	BVH bvh;

	// Light lookup grid in the XY plane
	float lightCell = 256.f;
	int lightGridW = 0, lightGridH = 0;
	Vec3 lightGridOrigin;
	std::vector<std::vector<int>> lightGrid;

	// Probe grid
	Vec3 origin;
	float spacing = 64.f;
	int sizeX = 0, sizeY = 0, sizeZ = 0;
	std::vector<float> columnFloor, columnCeiling;
	std::vector<Probe> probes;
	int raysPerProbe = 96;
	float albedoNormalize = 1.f;

	// MAPINFO gimoon: a directional lobe on rays that hit the sky, in the same
	// encoded space as the sky radiance. Each bake widens lobes sharper than
	// its ray density can resolve, keeping their energy (scale: encoded for the
	// diffuse probes, linear for the reflection texels), so probes don't flicker
	// between catching and missing the moon.
	float moonIntensity = 0.f;
	Vec3 moonDir, moonColor;
	float moonExponent = 1.f, moonScale = 1.f;	// diffuse probes
	float moonReflectionExponent = 1.f, moonReflectionScale = 1.f;

	Vec3 reflectionOrigin;
	float reflectionSpacing = 256.f;
	int reflectionX = 0, reflectionY = 0, reflectionZ = 0;
	std::vector<float> reflectionFloor, reflectionCeiling;
	std::vector<ReflectionProbe> reflectionProbes;
	std::atomic<int> nextReflection{ 0 };

	// Called on the main thread. Workers only access this bounded snapshot;
	// texture decoders and TexMan must never be touched by bake threads.
	int CaptureReflectionTexture(FTextureID id)
	{
		if (!id.isValid()) return -1;
		auto found = reflectionTextureIDs.find(id.GetIndex());
		if (found != reflectionTextureIDs.end()) return found->second;
		int result = -1;
		auto game = TexMan.GetGameTexture(id, false);
		if (game && game->isValid() && game->GetTexture() && !game->isHardwareCanvas())
		{
			FBitmap bitmap = game->GetTexture()->GetBgraBitmap(nullptr);
			int w = bitmap.GetWidth(), h = bitmap.GetHeight();
			if (bitmap.GetPixels() && w > 0 && h > 0)
			{
				ReflectionTexture texture;
				texture.width = std::min(w, 64); texture.height = std::min(h, 64);
				size_t bytes = size_t(texture.width) * texture.height * sizeof(ReflectionTexel);
				if (reflectionTextureBytes + bytes <= MaxReflectionTextureBytes)
				{
					FBitmap brightmap;
					bool fullbright = game->isFullbright();
					if (!fullbright && game->GetBrightmap()) brightmap = game->GetBrightmap()->GetBgraBitmap(nullptr);
					bool hasBrightmap = brightmap.GetPixels() && brightmap.GetWidth() > 0 && brightmap.GetHeight() > 0;
					texture.pixels.resize(size_t(texture.width) * texture.height);
					for (int y = 0; y < texture.height; y++) for (int x = 0; x < texture.width; x++)
					{
						const uint8_t *pixel = bitmap.GetPixels() + (y * h / texture.height) * bitmap.GetPitch() + (x * w / texture.width) * 4;
						float emission = fullbright ? 1.f : 0.f;
						if (hasBrightmap)
						{
							const uint8_t *bp = brightmap.GetPixels() + (y * brightmap.GetHeight() / texture.height) * brightmap.GetPitch() + (x * brightmap.GetWidth() / texture.width) * 4;
							emission = (bp[0] + bp[1] + bp[2]) / (3.f * 255.f);
						}
						ReflectionTexel &texel = texture.pixels[y * texture.width + x];
						texel.albedo = LinearColor(Vec3(pixel[2], pixel[1], pixel[0]) * (1.f / 255.f));
						texel.emissive = texel.albedo * emission;
						texel.lit = 1.f - emission;
					}
					reflectionTextureBytes += bytes;
					result = int(reflectionTextures.size());
					reflectionTextures.push_back(std::move(texture));
				}
			}
		}
		reflectionTextureIDs[id.GetIndex()] = result;
		return result;
	}

	std::atomic<int> nextSlice{ 0 };
	std::atomic<int> finishedSlices{ 0 };
	std::atomic<bool> cancel{ false };
	std::atomic<bool> done{ false };
	std::vector<std::thread> workers;
	std::thread finisher;
	uint64_t startTime = 0;

	~BakeJob()
	{
		cancel = true;
		if (finisher.joinable()) finisher.join();
		for (auto &w : workers) if (w.joinable()) w.join();
	}

	int ProbeIndex(int x, int y, int z) const { return (z * sizeY + y) * sizeX + x; }

	Vec3 ShadeHit(const Vec3 &pos, const Vec3 &rayDir, const Triangle &tri, const Vec3 &normal, bool reflection = false) const
	{
		const Material &m = materials[tri.material];
		if (m.sky)
		{
			if (moonIntensity <= 0.f) return reflection ? LinearColor(m.light) : m.light;
			// The moon is part of the sky radiance, so reflections linearise both together.
			Vec3 sky = m.light;
			float c = Dot(rayDir, moonDir);
			if (c > 0.f) sky += moonColor * (moonIntensity * (reflection ? moonReflectionScale * std::pow(c, moonReflectionExponent) : moonScale * std::pow(c, moonExponent)));
			return reflection ? LinearColor(sky) : sky;
		}
		Vec3 light = m.light, added;
		int gx = int((pos.x - lightGridOrigin.x) / lightCell);
		int gy = int((pos.y - lightGridOrigin.y) / lightCell);
		if (gx >= 0 && gy >= 0 && gx < lightGridW && gy < lightGridH)
		{
			for (int li : lightGrid[gy * lightGridW + gx])
			{
				const StaticLight &l = lights[li];
				Vec3 toLight = l.pos - pos;
				float dist = Length(toLight);
				if (dist >= l.radius || dist < 0.01f) continue;
				Vec3 dir = toLight * (1.f / dist);
				float facing = Dot(dir, normal);
				if (facing <= 0.f) continue;
				float cone = 1.f;
				if (l.spot)
				{
					// The scene shader's smoothstep between the outer and inner cone.
					float c = Dot(dir, l.spotDir);
					if (c <= l.spotOuter) continue;
					float x = std::clamp((c - l.spotOuter) / std::max(l.spotInner - l.spotOuter, 1e-4f), 0.f, 1.f);
					cone = x * x * (3.f - 2.f * x);
				}
				Hit shadow;
				Vec3 start = pos + normal * 0.5f;
				if (bvh.Intersect(start, dir, dist - 1.f, shadow, true)) continue;
				// Doom lights fall off linearly. Lambert is used for the bounce so
				// grazing surfaces reflect less light back into the room.
				float attenuation = (1.f - dist / l.radius) * (0.35f + 0.65f * facing) * cone;
				if (l.additive) added += l.color * attenuation;
				else light += l.color * attenuation;
			}
		}
		if (!reflection) return m.albedo * light * m.lit + m.emissive + added;
		Vec3 albedo = LinearColor(m.albedo), emissive = LinearColor(m.emissive);
		float lit = m.lit;
		if (m.reflectionTexture >= 0)
		{
			const ReflectionTexture &texture = reflectionTextures[m.reflectionTexture];
			float u = Dot(pos, m.textureU) + m.offsetU, v = Dot(pos, m.textureV) + m.offsetV;
			int x = std::min(int((u - std::floor(u)) * texture.width), texture.width - 1);
			int y = std::min(int((v - std::floor(v)) * texture.height), texture.height - 1);
			const ReflectionTexel &texel = texture.pixels[y * texture.width + x];
			albedo = texel.albedo;
			emissive = texel.emissive;
			lit = texel.lit;
		}
		return albedo * LinearColor(light) * lit + emissive + LinearColor(added);
	}

	void BakeProbe(int x, int y, int z, uint32_t seed)
	{
		Probe &probe = probes[ProbeIndex(x, y, z)];
		probe.valid = false;
		int column = y * sizeX + x;
		float floorZ = columnFloor[column], ceilZ = columnCeiling[column];
		float pz = origin.z + z * spacing;
		if (ceilZ - floorZ < 8.f || pz < floorZ - spacing || pz > ceilZ + spacing)
			return;
		Vec3 pos = { origin.x + x * spacing, origin.y + y * spacing, std::clamp(pz, floorZ + 4.f, ceilZ - 4.f) };

		Vec3 sum[6];
		float weight[6] = {};
		int backfaces = 0;
		const float golden = 2.39996323f;
		float rotation = (seed % 1024) * (6.2831853f / 1024.f);
		for (int i = 0; i < raysPerProbe; i++)
		{
			// Fibonacci sphere with a per-probe rotation to avoid banding.
			float cz = 1.f - (2.f * i + 1.f) / raysPerProbe;
			float r = std::sqrt(std::max(0.f, 1.f - cz * cz));
			float phi = i * golden + rotation;
			Vec3 dir = { std::cos(phi) * r, std::sin(phi) * r, cz };
			Hit hit;
			Vec3 radiance;
			if (bvh.Intersect(pos, dir, 8192.f, hit, false))
			{
				const Triangle &tri = triangles[hit.tri];
				Vec3 normal = tri.normal;
				if (Dot(normal, dir) > 0.f && !materials[tri.material].sky)
				{
					if (!(tri.flags & TRI_TWOSIDED))
					{
						backfaces++;
						continue;
					}
					normal = normal * -1.f;
				}
				radiance = ShadeHit(pos + dir * hit.t, dir, tri, normal);
			}
			float w;
			if ((w = dir.x) > 0) { sum[0] += radiance * w; weight[0] += w; } else { sum[1] += radiance * -w; weight[1] -= w; }
			if ((w = dir.y) > 0) { sum[2] += radiance * w; weight[2] += w; } else { sum[3] += radiance * -w; weight[3] -= w; }
			if ((w = dir.z) > 0) { sum[4] += radiance * w; weight[4] += w; } else { sum[5] += radiance * -w; weight[5] -= w; }
		}
		// Probes that see mostly backfaces are inside walls or outside the map.
		if (backfaces * 4 > raysPerProbe) return;
		for (int f = 0; f < 6; f++)
			probe.face[f] = weight[f] > 0 ? sum[f] * (albedoNormalize / weight[f]) : Vec3();
		probe.valid = true;
	}

	void BakeReflection(int index)
	{
		int x = index % reflectionX, y = (index / reflectionX) % reflectionY, z = index / (reflectionX * reflectionY);
		int column = y * reflectionX + x;
		float floorZ = reflectionFloor[column], ceilZ = reflectionCeiling[column];
		float pz = reflectionOrigin.z + z * reflectionSpacing;
		if (ceilZ - floorZ < 8.f || pz < floorZ - reflectionSpacing || pz > ceilZ + reflectionSpacing) return;
		Vec3 pos(reflectionOrigin.x + x * reflectionSpacing, reflectionOrigin.y + y * reflectionSpacing,
			std::clamp(pz, floorZ + 4.f, ceilZ - 4.f));
		ReflectionProbe &probe = reflectionProbes[index];
		const int samples = ReflectionResolution * ReflectionResolution;
		Vec3 radiance[samples], directions[samples];
		float solidAngle[samples];
		int backfaces = 0, hits = 0;
		for (int py = 0; py < ReflectionResolution && !cancel; py++) for (int px = 0; px < ReflectionResolution; px++)
		{
			int i = py * ReflectionResolution + px;
			Vec3 dir = OctaDirection((px + 0.5f) * (2.f / ReflectionResolution) - 1.f, (py + 0.5f) * (2.f / ReflectionResolution) - 1.f);
			directions[i] = dir;
			// Octahedral texels subtend different solid angles. Weight the
			// prefilter by the Jacobian so corners do not bias rough reflections.
			float l1 = std::abs(dir.x) + std::abs(dir.y) + std::abs(dir.z);
			solidAngle[i] = l1 * l1 * l1;
			Hit hit;
			if (bvh.Intersect(pos, dir, 8192.f, hit, false))
			{
				hits++;
				const Triangle &tri = triangles[hit.tri];
				Vec3 normal = tri.normal;
				if (Dot(normal, dir) > 0.f && !materials[tri.material].sky)
				{
					if (!(tri.flags & TRI_TWOSIDED)) { backfaces++; continue; }
					normal = normal * -1.f;
				}
				radiance[i] = ShadeHit(pos + dir * hit.t, dir, tri, normal, true);
			}
		}
		if (cancel || hits == 0 || backfaces * 4 > hits) return;
		probe.pixels.resize(ReflectionLevels * samples);
		for (int level = 0; level < ReflectionLevels && !cancel; level++)
		{
			float roughness = float(level) / (ReflectionLevels - 1);
			float exponent = 2.f / std::max(roughness * roughness, 0.001f) - 2.f;
			for (int i = 0; i < samples && !cancel; i++)
			{
				if (!level) { probe.pixels[i] = radiance[i]; continue; }
				Vec3 sum;
				float weight = 0.f;
				for (int j = 0; j < samples; j++)
				{
					float cosine = Dot(directions[i], directions[j]);
					if (cosine <= 0.f) continue;
					float w = std::pow(cosine, exponent) * solidAngle[j];
					sum += radiance[j] * w;
					weight += w;
				}
				probe.pixels[level * samples + i] = weight > 0.f ? sum * (1.f / weight) : Vec3();
			}
		}
		probe.valid = !cancel;
	}

	void Worker()
	{
#ifdef __APPLE__
		pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
		int slices = sizeY * sizeZ;
		while (!cancel)
		{
			int slice = nextSlice++;
			if (slice >= slices) break;
			int y = slice % sizeY, z = slice / sizeY;
			for (int x = 0; x < sizeX && !cancel; x++)
				BakeProbe(x, y, z, uint32_t(x * 73856093u ^ y * 19349663u ^ z * 83492791u));
			finishedSlices++;
		}
		while (!cancel)
		{
			int index = nextReflection++;
			if (index >= int(reflectionProbes.size())) break;
			BakeReflection(index);
		}
	}

	void Dilate()
	{
		// Fill invalid probes from their valid neighbours so interpolation near
		// walls does not pull in black probes from inside the geometry.
		std::vector<Probe> next = probes;
		for (int pass = 0; pass < 3; pass++)
		{
			bool changed = false;
			for (int z = 0; z < sizeZ; z++) for (int y = 0; y < sizeY; y++) for (int x = 0; x < sizeX; x++)
			{
				const Probe &p = probes[ProbeIndex(x, y, z)];
				if (p.valid) continue;
				Vec3 acc[6];
				int n = 0;
				const int offsets[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
				for (auto &o : offsets)
				{
					int nx = x + o[0], ny = y + o[1], nz = z + o[2];
					if (nx < 0 || ny < 0 || nz < 0 || nx >= sizeX || ny >= sizeY || nz >= sizeZ) continue;
					const Probe &q = probes[ProbeIndex(nx, ny, nz)];
					if (!q.valid) continue;
					for (int f = 0; f < 6; f++) acc[f] += q.face[f];
					n++;
				}
				if (n == 0) continue;
				Probe &out = next[ProbeIndex(x, y, z)];
				for (int f = 0; f < 6; f++) out.face[f] = acc[f] * (1.f / n);
				out.valid = true;
				changed = true;
			}
			probes = next;
			if (!changed) break;
		}
	}
};

std::unique_ptr<BakeJob> job;
bool volumeUploaded;
bool bakeRequested;
FVector4 volumeOrigin = { 0, 0, 0, 1 }, volumeSize = { 0, 0, 0, 0 };
FVector4 reflectionOrigin = { 0, 0, 0, 1 }, reflectionSize = { 0, 0, 0, 0 }, reflectionAtlas = { 0, 0, 0, 0 };

struct TextureInfo
{
	Vec3 albedo = { 0.5f, 0.5f, 0.5f };
	Vec3 emissive;
	float lit = 1.f;
};
std::unordered_map<int, TextureInfo> textureCache;

// Average colour of the opaque texels, and of the part that is emissive: all of
// it for fullbright textures, the brightmap-weighted texels otherwise.
const TextureInfo &GetTextureInfo(FTextureID id)
{
	static const TextureInfo invalid;
	if (!id.isValid()) return invalid;
	auto found = textureCache.find(id.GetIndex());
	if (found != textureCache.end()) return found->second;
	TextureInfo &result = textureCache[id.GetIndex()];
	// The base texture, not the current animation frame, so the bake does not
	// depend on when the scene is captured.
	auto game = TexMan.GetGameTexture(id, false);
	if (!game || !game->isValid() || !game->GetTexture()) return result;
	FBitmap bitmap = game->GetTexture()->GetBgraBitmap(nullptr);
	const uint8_t *pixels = bitmap.GetPixels();
	int w = bitmap.GetWidth(), h = bitmap.GetHeight();
	if (!pixels || w <= 0 || h <= 0) return result;

	FBitmap brightBitmap;
	const uint8_t *bright = nullptr;
	int bw = 0, bh = 0;
	bool fullbright = game->isFullbright();
	if (!fullbright)
	{
		if (FTexture *brightmap = game->GetBrightmap())
		{
			brightBitmap = brightmap->GetBgraBitmap(nullptr);
			bright = brightBitmap.GetPixels();
			bw = brightBitmap.GetWidth();
			bh = brightBitmap.GetHeight();
			if (bw <= 0 || bh <= 0) bright = nullptr;
		}
	}

	double r = 0, g = 0, b = 0, n = 0, er = 0, eg = 0, eb = 0, mask = 0;
	for (int y = 0; y < h; y++)
	{
		const uint8_t *row = pixels + y * bitmap.GetPitch();
		const uint8_t *brow = bright ? bright + (y * bh / h) * brightBitmap.GetPitch() : nullptr;
		for (int x = 0; x < w; x++, row += 4)
		{
			if (row[3] < 128) continue;
			b += row[0]; g += row[1]; r += row[2]; n++;
			double e = fullbright ? 1.0 : 0.0;
			if (brow)
			{
				const uint8_t *bp = brow + (x * bw / w) * 4;
				e = (bp[0] + bp[1] + bp[2]) / (3.0 * 255.0);
			}
			eb += row[0] * e; eg += row[1] * e; er += row[2] * e; mask += e;
		}
	}
	if (n > 0)
	{
		result.albedo = { float(r / (n * 255)), float(g / (n * 255)), float(b / (n * 255)) };
		result.emissive = { float(er / (n * 255)), float(eg / (n * 255)), float(eb / (n * 255)) };
		result.lit = float(1.0 - mask / n);
	}
	return result;
}

Vec3 TextureAlbedo(FTextureID id)
{
	return GetTextureInfo(id).albedo;
}

// Average colour of the whole sky texture.
Vec3 SkyColor(FTextureID id)
{
	auto game = TexMan.GetGameTexture(id, false);
	if (!game || !game->isValid() || !game->GetTexture()) return { 0.3f, 0.32f, 0.38f };
	FBitmap bitmap = game->GetTexture()->GetBgraBitmap(nullptr);
	const uint8_t *pixels = bitmap.GetPixels();
	int w = bitmap.GetWidth(), h = bitmap.GetHeight();
	if (!pixels || w <= 0 || h <= 0) return { 0.3f, 0.32f, 0.38f };
	double r = 0, g = 0, b = 0;
	for (int y = 0; y < h; y++)
	{
		const uint8_t *row = pixels + y * bitmap.GetPitch();
		for (int x = 0; x < w; x++, row += 4)
		{
			b += row[0]; g += row[1]; r += row[2];
		}
	}
	double n = double(w) * h * 255;
	return { float(r / n), float(g / n), float(b / n) };
}

// Sectors of the playable map: connected through two-sided lines and linked
// portals to a player start, monster, item or teleport destination. Skybox and
// vista areas and isolated control sectors are left out, so they don't stretch
// the probe grid.
std::vector<bool> PlayableSectors(FLevelLocals *level)
{
	unsigned count = level->sectors.Size();
	std::vector<int> parent(count);
	for (unsigned i = 0; i < count; i++) parent[i] = (int)i;
	auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
	auto join = [&](sector_t *a, sector_t *b) { if (a && b) parent[find(a->Index())] = find(b->Index()); };

	for (auto &line : level->lines)
	{
		join(line.frontsector, line.backsector);
		if (line.isLinePortal())
		{
			line_t *dest = line.getPortalDestination();
			if (dest) join(line.frontsector, dest->frontsector);
		}
	}
	for (auto &portal : level->sectorPortals)
	{
		if (portal.mType == PORTS_STACKEDSECTORTHING || portal.mType == PORTS_PORTAL || portal.mType == PORTS_LINKEDPORTAL)
			join(portal.mOrigin, portal.mDestination);
	}

	std::vector<char> seeded(count), excluded(count);
	auto seed = [&](sector_t *sector) { if (sector) seeded[find(sector->Index())] = true; };
	for (auto &start : level->playerstarts)
		if (start.type != 0) seed(level->PointInSector(start.pos.XY()));
	for (auto &start : level->AllPlayerStarts)
		seed(level->PointInSector(start.pos.XY()));
	auto it = level->GetThinkerIterator<AActor>();
	while (AActor *actor = it.Next())
	{
		if (actor->player || (actor->flags & (MF_COUNTKILL | MF_SPECIAL)) || actor->IsKindOf(NAME_TeleportDest))
			seed(actor->Sector);
	}
	for (auto &portal : level->sectorPortals)
	{
		AActor *viewpoint = portal.mSkybox;
		if (portal.mType == PORTS_SKYVIEWPOINT && viewpoint && viewpoint->Sector)
			excluded[find(viewpoint->Sector->Index())] = true;
	}

	std::vector<bool> playable(count);
	bool any = false;
	for (unsigned i = 0; i < count; i++)
	{
		int c = find((int)i);
		playable[i] = seeded[c] && !excluded[c];
		any |= playable[i];
	}
	if (!any) playable.assign(count, true);
	return playable;
}

// Flickering and glowing sectors bake at a steady level, so the result does not
// depend on the tic the scene was captured on.
std::unordered_map<const sector_t*, int> steadyLight;

void CaptureSteadyLights(FLevelLocals *level)
{
	steadyLight.clear();
	auto iterator = level->GetThinkerIterator<DLighting>(NAME_None, STAT_LIGHT);
	while (DLighting *effect = iterator.Next())
	{
		int steady = effect->SteadyLightLevel();
		if (steady >= 0 && effect->GetSector())
			steadyLight[effect->GetSector()] = steady;
	}
}

int SteadyOffset(const sector_t *sector)
{
	auto found = steadyLight.find(sector);
	return found != steadyLight.end() ? found->second - sector->lightlevel : 0;
}

Vec3 LightColor(sector_t *sector, int level)
{
	level += SteadyOffset(sector);
	PalEntry c = sector->Colormap.LightColor;
	float l = std::clamp(level, 0, 255) / 255.f;
	return { c.r / 255.f * l, c.g / 255.f * l, c.b / 255.f * l };
}

void CaptureAndStart(FLevelLocals *level)
{
	// Bake from the current geometry: after a savegame load, hub re-entry or
	// r_gi_rebake, sectors and polyobjects differ from the map data.
	DoomLevelMesh::Update(level, true, true);
	DoomLevelMesh *mesh = level->levelMesh;
	if (!mesh || mesh->MeshElements.Size() == 0) return;

	auto bake = std::make_unique<BakeJob>();
	bake->startTime = I_msTime();
	textureCache.clear();
	CaptureSteadyLights(level);

	// Sky radiance from the sky's real average colour, so night skies stay dark.
	// A day sky of average brightness lands near a well lit room.
	Vec3 sky = { 0.6f, 0.65f, 0.75f };
	if (level->skytexture1.isValid())
	{
		sky = SkyColor(level->skytexture1) * 2.f;
		float lum = Luminance(sky);
		if (lum > 1.f) sky = sky * (1.f / lum);
		else if (lum < 0.03f) sky = lum > 0.f ? sky * (0.03f / lum) : Vec3(0.03f, 0.03f, 0.03f);
	}
	// MAPINFO gisky replaces the derived radiance; sky fog still blends it below.
	const FMapAtmosphereInfo &atmosphere = level->atmosphere;
	if (atmosphere.GISky)
	{
		PalEntry c = atmosphere.GISkyColor;
		sky = Vec3(c.r / 255.f, c.g / 255.f, c.b / 255.f) * atmosphere.GISkyIntensity;
	}
	float skyFog = std::clamp(level->skyfog, 0, 255) / 255.f;

	// One material per surface.
	std::vector<int> surfaceMaterial(mesh->Surfaces.Size(), -1);
	for (unsigned i = 0; i < mesh->Surfaces.Size(); i++)
	{
		const Surface &s = mesh->Surfaces[i];
		Material m = {};
		FTextureID textureID = s.texture;
		m.sky = s.bSky;
		const TextureInfo &texinfo = GetTextureInfo(textureID);
		m.albedo = texinfo.albedo;
		m.emissive = texinfo.emissive;
		m.lit = texinfo.lit;
		sector_t *sector = s.sector;
		if (s.type == ST_FLOOR || s.type == ST_CEILING)
			m.light = LightColor(sector, s.type == ST_FLOOR ? sector->GetFloorLight() : sector->GetCeilingLight());
		else
			m.light = LightColor(sector, sector->lightlevel);
		if (m.sky)
		{
			m.light = sky;
			// Keep the newer sky-fog contribution for both diffuse and reflection probes.
			PalEntry fade = sector->Colormap.FadeColor;
			if (skyFog > 0.f && (fade & 0xffffff) != 0)
			{
				Vec3 fog = { fade.r / 255.f, fade.g / 255.f, fade.b / 255.f };
				m.light = sky * (1.f - skyFog) + fog * skyFog;
			}
		}
		else if (r_sprite_pbr && r_sprite_reflections && s.type != ST_MODEL)
		{
			// The mesh carries the correct texture for 3D floors and masked walls.
			// Models have no captured UVs, so retain their material-average radiance.
			auto tex = TexMan.GetGameTexture(textureID, false);
			if (tex && tex->isValid())
			{
				if (s.type == ST_FLOOR || s.type == ST_CEILING)
				{
					sector_t *mapping = s.controlSector ? s.controlSector : sector;
					bool floor = s.type == ST_FLOOR;
					int plane = floor ? (s.controlSector ? sector_t::ceiling : sector_t::floor) : (s.controlSector ? sector_t::floor : sector_t::ceiling);
					float angle = float(-mapping->GetAngle(plane).Radians());
					float c = std::cos(angle), sn = std::sin(angle);
					float us = float(mapping->GetXScale(plane)) / std::max(tex->GetDisplayWidth(), 1.f);
					float vs = float(mapping->GetYScale(plane)) / std::max(tex->GetDisplayHeight(), 1.f);
					m.textureU = { c * us, sn * us, 0.f };
					m.textureV = { sn * vs, -c * vs, 0.f };
					m.offsetU = float(mapping->GetXOffset(plane)) * us;
					m.offsetV = float(mapping->GetYOffset(plane)) * vs;
				}
				else
				{
					side_t *side = &level->sides[s.typeIndex];
					int part = s.type == ST_UPPERWALL ? side_t::top : s.type == ST_LOWERWALL ? side_t::bottom : side_t::mid;
					FTexCoordInfo tci;
					tci.GetFromTexture(tex, float(side->GetTextureXScale(part)), float(side->GetTextureYScale(part)), !!(level->flags3 & LEVEL3_FORCEWORLDPANNING));
					DVector2 start = side->V1()->fPos(), delta = side->V2()->fPos() - start;
					float length = float(delta.Length());
					float us = length > 0.f ? side->TexelLength / (length * std::max(std::abs(tci.mRenderWidth), 1)) : 0.f;
					Vec3 along = length > 0.f ? Vec3(float(delta.X), float(delta.Y), 0.f) * (1.f / length) : Vec3();
					m.textureU = along * us;
					m.offsetU = tci.FloatToTexU(tci.TextureOffset(float(side->GetTextureXOffset(part)))) - Dot(Vec3(float(start.X), float(start.Y), 0.f), m.textureU);
					// Static affine wall mapping: sloped/skewed midtextures and
					// 3D-floor side offsets remain an approximation.
					sector_t *front = side->sector;
					sector_t *back = side->linedef->frontsector == front ? side->linedef->backsector : side->linedef->frontsector;
					float top = float(front->ceilingplane.ZatPoint(start));
					if (part == side_t::top && back && !(side->linedef->flags & ML_DONTPEGTOP))
						top = float(back->ceilingplane.ZatPoint(start)) + tci.mRenderHeight;
					else if (part == side_t::bottom && back && !(side->linedef->flags & ML_DONTPEGBOTTOM))
						top = float(back->floorplane.ZatPoint(start));
					else if (part == side_t::mid && (side->linedef->flags & ML_DONTPEGBOTTOM))
						top = float(front->floorplane.ZatPoint(start)) + tci.mRenderHeight;
					m.textureV = { 0.f, 0.f, -1.f / std::max(std::abs(tci.mRenderHeight), 1) };
					m.offsetV = tci.FloatToTexV(top + tci.RowOffset(float(side->GetTextureYOffset(part))));
				}
				m.reflectionTexture = bake->CaptureReflectionTexture(textureID);
			}
		}
		surfaceMaterial[i] = (int)bake->materials.size();
		bake->materials.push_back(m);
	}

	// Triangles with normals that face into the sector they belong to. The probe
	// grid only covers the playable map; the light grid covers everything.
	std::vector<bool> playable = PlayableSectors(level);
	double albedoSum = 0, areaSum = 0;
	Vec3 bmin = { 1e30f, 1e30f, 1e30f }, bmax = { -1e30f, -1e30f, -1e30f };
	Vec3 pmin = bmin, pmax = bmax;
	for (unsigned i = 0; i + 2 < mesh->MeshElements.Size(); i += 3)
	{
		int surfIndex = mesh->MeshSurfaces[i / 3];
		const Surface &s = mesh->Surfaces[surfIndex];
		const FVector3 &a = mesh->MeshVertices[mesh->MeshElements[i]];
		const FVector3 &b = mesh->MeshVertices[mesh->MeshElements[i + 1]];
		const FVector3 &c = mesh->MeshVertices[mesh->MeshElements[i + 2]];
		Triangle t;
		t.v0 = { a.X, a.Y, a.Z };
		t.e1 = Vec3(b.X, b.Y, b.Z) - t.v0;
		t.e2 = Vec3(c.X, c.Y, c.Z) - t.v0;
		t.flags = (s.bTwoSided ? TRI_TWOSIDED : 0) | (s.bLightOwner ? TRI_NOSHADOW : 0);
		Vec3 n = Normalize(Cross(t.e1, t.e2));
		if (s.type == ST_FLOOR) { if (n.z < 0) n = n * -1.f; }
		else if (s.type == ST_CEILING) { if (n.z > 0) n = n * -1.f; }
		else if (s.type != ST_MODEL)
		{
			side_t *side = &level->sides[s.typeIndex];
			DVector2 d = side->V2()->fPos() - side->V1()->fPos();
			Vec3 inward = Normalize(Vec3((float)d.Y, (float)-d.X, 0.f));
			if (Dot(n, inward) < 0) n = n * -1.f;
		}
		t.normal = n;
		t.material = surfaceMaterial[surfIndex];
		bake->triangles.push_back(t);
		Vec3 tmin = Min(t.v0, Min(t.v0 + t.e1, t.v0 + t.e2)), tmax = Max(t.v0, Max(t.v0 + t.e1, t.v0 + t.e2));
		bmin = Min(bmin, tmin);
		bmax = Max(bmax, tmax);
		if (s.sector && playable[s.sector->Index()])
		{
			pmin = Min(pmin, tmin);
			pmax = Max(pmax, tmax);
		}
		const Material &m = bake->materials[t.material];
		if (!m.sky)
		{
			double area = Length(Cross(t.e1, t.e2)) * 0.5;
			albedoSum += Luminance(m.albedo) * area;
			areaSum += area;
		}
	}
	if (bake->triangles.empty() || areaSum <= 0) return;
	if (pmin.x > pmax.x) { pmin = bmin; pmax = bmax; }
	// Normalize so a uniformly lit room bounces back roughly its own light level.
	float meanAlbedo = float(albedoSum / areaSum);
	bake->albedoNormalize = 1.f / std::max(meanAlbedo, 0.05f);

	// Static dynamic lights: anything attached to a non-moving, non-monster actor.
	int spots = 0;
	for (FDynamicLight *light = level->lights; light; light = light->next)
	{
		if (!light->IsActive() || light->IsSubtractive()) continue;
		AActor *owner = light->target;
		if (owner && (owner->flags & (MF_MISSILE | MF_SHOOTABLE | MF_COUNTKILL))) continue;
		// Flickering and pulsing lights bake at their average size, so the result
		// does not depend on the tic the scene was captured on.
		float radius = light->GetRadius();
		int intensity = light->GetIntensity(), secondary = light->GetSecondaryIntensity();
		switch (light->lighttype)
		{
		case PulseLight:
		case FlickerLight:
		case RandomFlickerLight:
		case ColorPulseLight:
		case ColorFlickerLight:
		case RandomColorFlickerLight:
			if (secondary > 0) radius = float(intensity + secondary);
			break;
		default:
			break;
		}
		if (radius <= 0) continue;
		// The same scale as the scene shader: GLDEFS intensity, alpha for
		// LIGHTMULTALPHA owners, and additive lights at a fifth.
		float scale = (float)light->GetLightDefIntensity();
		if (owner && (owner->renderflags2 & RF2_LIGHTMULTALPHA)) scale *= (float)owner->Alpha;
		StaticLight l = {};
		l.additive = light->IsAdditive();
		if (l.additive) scale *= 0.2f;
		if (scale <= 0.f) continue;
		l.pos = { (float)light->X(), (float)light->Y(), (float)light->Z() };
		l.color = Vec3(light->GetRed() / 255.f, light->GetGreen() / 255.f, light->GetBlue() / 255.f) * scale;
		l.radius = radius;
		if (light->IsSpot() && owner)
		{
			DAngle negPitch = -*light->pPitch;
			DAngle angle = owner->Angles.Yaw;
			double xzLen = negPitch.Cos();
			l.spot = true;
			l.spotDir = Normalize(Vec3(float(-angle.Cos() * xzLen), float(-angle.Sin() * xzLen), float(-negPitch.Sin())));
			l.spotInner = (float)light->pSpotInnerAngle->Cos();
			l.spotOuter = (float)light->pSpotOuterAngle->Cos();
			spots++;
		}
		bake->lights.push_back(l);
	}

	// Light grid for quick lookup at ray hits.
	bake->lightGridOrigin = bmin;
	bake->lightGridW = std::max(1, int((bmax.x - bmin.x) / bake->lightCell) + 1);
	bake->lightGridH = std::max(1, int((bmax.y - bmin.y) / bake->lightCell) + 1);
	bake->lightGrid.resize(bake->lightGridW * bake->lightGridH);
	for (int li = 0; li < (int)bake->lights.size(); li++)
	{
		const auto &l = bake->lights[li];
		int x0 = std::max(0, int((l.pos.x - l.radius - bmin.x) / bake->lightCell));
		int x1 = std::min(bake->lightGridW - 1, int((l.pos.x + l.radius - bmin.x) / bake->lightCell));
		int y0 = std::max(0, int((l.pos.y - l.radius - bmin.y) / bake->lightCell));
		int y1 = std::min(bake->lightGridH - 1, int((l.pos.y + l.radius - bmin.y) / bake->lightCell));
		for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++)
			bake->lightGrid[y * bake->lightGridW + x].push_back(li);
	}

	// Probe grid. Quality controls spacing and ray count; very large maps get
	// coarser spacing so bake time and memory stay bounded.
	int quality = std::clamp(*r_gi_quality, 0, 2);
	const float baseSpacing[3] = { 64.f, 48.f, 32.f };
	const int rays[3] = { 48, 96, 160 };
	const int maxXY[3] = { 96, 128, 192 };
	const int maxZ[3] = { 16, 24, 32 };
	Vec3 extent = pmax - pmin;
	float spacing = baseSpacing[quality];
	spacing = std::max(spacing, extent.x / maxXY[quality]);
	spacing = std::max(spacing, extent.y / maxXY[quality]);
	spacing = std::max(spacing, extent.z / maxZ[quality]);
	bake->spacing = spacing;
	bake->raysPerProbe = rays[quality];
	if (atmosphere.GIMoonIntensity > 0.f)
	{
		PalEntry c = atmosphere.GIMoonColor;
		float yaw = atmosphere.GIMoonYaw * float(M_PI / 180.0), pitch = atmosphere.GIMoonPitch * float(M_PI / 180.0);
		bake->moonIntensity = atmosphere.GIMoonIntensity;
		bake->moonColor = Vec3(c.r / 255.f, c.g / 255.f, c.b / 255.f);
		bake->moonDir = Vec3(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
		// A lobe of exponent n covers 1/(2(n+1)) of the sphere (2pi/(n+1) sr).
		// Capping n+1 at 1/8 of the samples (diffuse rays or reflection texels)
		// leaves about four samples in it, enough for the rotated Fibonacci rays
		// to catch its energy within about 2 % on every probe. Widened lobes keep
		// the energy: encoded for the diffuse probes, linear for reflections,
		// which raise each texel to the power 2.2 together with the sky under
		// the lobe, so their scale is solved for this sky's brightness.
		float exponent = std::max(atmosphere.GIMoonExponent, 1.f);
		bake->moonExponent = std::min(exponent, std::max(1.f, bake->raysPerProbe / 8.f - 1.f));
		bake->moonScale = (bake->moonExponent + 1.f) / (exponent + 1.f);
		bake->moonReflectionExponent = std::min(exponent, ReflectionResolution * ReflectionResolution / 8.f - 1.f);
		float skyLuma = Luminance(sky), peak = bake->moonIntensity * Luminance(bake->moonColor);
		if (bake->moonReflectionExponent < exponent && peak > 0.f)
		{
			// The energy grows with the scale, and the widened lobe at full scale has more.
			float target = LobeLinearEnergy(skyLuma, peak, exponent), lo = 0.f, hi = 1.f;
			for (int i = 0; i < 24; i++)
			{
				float mid = (lo + hi) * 0.5f;
				if (LobeLinearEnergy(skyLuma, peak * mid, bake->moonReflectionExponent) < target) lo = mid;
				else hi = mid;
			}
			bake->moonReflectionScale = (lo + hi) * 0.5f;
		}
	}
	bake->origin = pmin;
	bake->sizeX = std::max(2, int(std::ceil(extent.x / spacing)) + 1);
	bake->sizeY = std::max(2, int(std::ceil(extent.y / spacing)) + 1);
	bake->sizeZ = std::max(2, int(std::ceil(extent.z / spacing)) + 1);
	bake->probes.resize(size_t(bake->sizeX) * bake->sizeY * bake->sizeZ);

	// Floor and ceiling heights per probe column, used to skip solid space.
	bake->columnFloor.resize(size_t(bake->sizeX) * bake->sizeY);
	bake->columnCeiling.resize(size_t(bake->sizeX) * bake->sizeY);
	for (int y = 0; y < bake->sizeY; y++) for (int x = 0; x < bake->sizeX; x++)
	{
		DVector2 p(bake->origin.x + x * spacing, bake->origin.y + y * spacing);
		sector_t *sector = level->PointInSector(p);
		bake->columnFloor[y * bake->sizeX + x] = (float)sector->floorplane.ZatPoint(p);
		bake->columnCeiling[y * bake->sizeX + x] = (float)sector->ceilingplane.ZatPoint(p);
	}

	if (r_sprite_pbr && r_sprite_reflections)
	{
		// A sparse reflection grid has an independent budget; never multiply
		// the full diffuse GI grid by the angular resolution.
		float step = std::max(192.f, std::max(extent.x / 7.f, std::max(extent.y / 7.f, extent.z / 3.f)));
		auto count = [&] {
			bake->reflectionX = std::max(1, int(std::ceil(extent.x / step)));
			bake->reflectionY = std::max(1, int(std::ceil(extent.y / step)));
			bake->reflectionZ = std::max(1, int(std::ceil(extent.z / step)));
			return bake->reflectionX * bake->reflectionY * bake->reflectionZ;
		};
		while (count() > MaxReflectionProbes) step *= 1.1f;
		bake->reflectionOrigin = pmin + Vec3(
			(extent.x - (bake->reflectionX - 1) * step) * 0.5f,
			(extent.y - (bake->reflectionY - 1) * step) * 0.5f,
			(extent.z - (bake->reflectionZ - 1) * step) * 0.5f);
		bake->reflectionSpacing = step;
		bake->reflectionProbes.resize(count());
		bake->reflectionFloor.resize(bake->reflectionX * bake->reflectionY);
		bake->reflectionCeiling.resize(bake->reflectionX * bake->reflectionY);
		for (int y = 0; y < bake->reflectionY; y++) for (int x = 0; x < bake->reflectionX; x++)
		{
			DVector2 p(bake->reflectionOrigin.x + x * step, bake->reflectionOrigin.y + y * step);
			sector_t *sector = level->PointInSector(p);
			bake->reflectionFloor[y * bake->reflectionX + x] = float(sector->floorplane.ZatPoint(p));
			bake->reflectionCeiling[y * bake->reflectionX + x] = float(sector->ceilingplane.ZatPoint(p));
		}
	}

	bake->bvh.Build(bake->triangles);

	unsigned threads = std::max(1u, std::thread::hardware_concurrency());
	threads = threads > 2 ? threads - 1 : threads;
	BakeJob *raw = bake.get();
	for (unsigned i = 0; i < threads; i++)
		bake->workers.emplace_back([raw] { raw->Worker(); });
	// A final thread waits for the workers and dilates the result.
	bake->finisher = std::thread([raw] {
		for (auto &w : raw->workers) w.join();
		if (raw->cancel) return;
		raw->Dilate();
		raw->done = true;
	});

	FString mapSky;
	if (atmosphere.GISky) mapSky += " from gisky";
	if (bake->moonIntensity > 0.f)
		mapSky.AppendFormat(", gimoon %.2f %.2f %.2f x %.2f at yaw %.0f pitch %.0f, exponent %.0f (%.0f diffuse, %.0f reflections)",
			bake->moonColor.x, bake->moonColor.y, bake->moonColor.z, bake->moonIntensity, atmosphere.GIMoonYaw, atmosphere.GIMoonPitch,
			atmosphere.GIMoonExponent, bake->moonExponent, bake->moonReflectionExponent);
	DPrintf(DMSG_NOTIFY, "GI: baking %dx%dx%d probes (%.0f unit spacing, %d rays, %u triangles, %u static lights, %d spots, sky %.2f %.2f %.2f%s) on %u threads\n",
		bake->sizeX, bake->sizeY, bake->sizeZ, spacing, bake->raysPerProbe, (unsigned)bake->triangles.size(), (unsigned)bake->lights.size(), spots, sky.x, sky.y, sky.z, mapSky.GetChars(), threads);
	job = std::move(bake);
}

uint16_t ToHalf(float f)
{
	// Values are non-negative and modest; a simple conversion is sufficient.
	if (!(f > 0.f)) return 0;
	if (f > 65000.f) f = 65000.f;
	uint32_t bits;
	memcpy(&bits, &f, 4);
	int exponent = int((bits >> 23) & 0xff) - 127 + 15;
	uint32_t mantissa = bits & 0x7fffff;
	if (exponent <= 0) return 0;
	if (exponent >= 31) return 0x7bff;
	return uint16_t((exponent << 10) | (mantissa >> 13));
}

void Upload()
{
	BakeJob &b = *job;
	bool hasReflections = !b.reflectionProbes.empty();
	int width = hasReflections ? std::max(b.sizeX, ReflectionTile) : b.sizeX;
	int height = hasReflections ? std::max(b.sizeY, ReflectionTile) : b.sizeY;
	int tilesX = width / ReflectionTile, tilesY = height / ReflectionTile;
	int reflectionLayers = hasReflections ? (int(b.reflectionProbes.size()) + tilesX * tilesY - 1) / (tilesX * tilesY) : 0;
	int baseLayer = b.sizeZ * 6;
	// Keep the combined array within the portable 256-layer budget. Expand
	// only when necessary so thin/tiny maps do not waste diffuse volume RAM.
	while (baseLayer + reflectionLayers > 256)
	{
		if (width <= height) width = (tilesX + 1) * ReflectionTile;
		else height = (tilesY + 1) * ReflectionTile;
		tilesX = width / ReflectionTile; tilesY = height / ReflectionTile;
		reflectionLayers = (int(b.reflectionProbes.size()) + tilesX * tilesY - 1) / (tilesX * tilesY);
	}
	int layers = baseLayer + reflectionLayers;
	TArray<uint16_t> data;
	data.Resize(unsigned(size_t(width) * height * layers * 4));
	memset(data.Data(), 0, data.Size() * sizeof(uint16_t));
	auto write = [&](int x, int y, int layer, const Vec3 &c, bool valid) {
		uint16_t *out = data.Data() + ((size_t(layer) * height + y) * width + x) * 4;
		out[0] = ToHalf(c.x); out[1] = ToHalf(c.y); out[2] = ToHalf(c.z); out[3] = ToHalf(valid ? 1.f : 0.f);
	};
	// Diffuse ambient cubes retain their original face * sizeZ + z layout.
	// The shader divides XY coordinates by textureSize, not the probe count,
	// because tiny diffuse grids are padded to fit the reflection atlas.
	for (int f = 0; f < 6; f++) for (int z = 0; z < b.sizeZ; z++)
		for (int y = 0; y < b.sizeY; y++) for (int x = 0; x < b.sizeX; x++)
		{
			const Probe &p = b.probes[b.ProbeIndex(x, y, z)];
			write(x, y, f * b.sizeZ + z, p.valid ? p.face[f] : Vec3(), p.valid);
		}
	for (int i = 0; i < int(b.reflectionProbes.size()); i++)
	{
		const ReflectionProbe &probe = b.reflectionProbes[i];
		if (!probe.valid) continue;
		int tile = i % (tilesX * tilesY), layer = baseLayer + i / (tilesX * tilesY);
		int ox = (tile % tilesX) * ReflectionTile, oy = (tile / tilesX) * ReflectionTile;
		for (int level = 0; level < ReflectionLevels; level++)
			for (int y = -1; y <= ReflectionResolution; y++) for (int x = -1; x <= ReflectionResolution; x++)
			{
				// Fold octahedral guard texels across the seam, rather than
				// clamping: bilinear filtering must preserve direction at edges.
				int sx = x, sy = y;
				if (sx < 0 || sx >= ReflectionResolution) { sx = std::clamp(sx, 0, ReflectionResolution - 1); sy = ReflectionResolution - 1 - sy; }
				if (sy < 0 || sy >= ReflectionResolution) { sy = std::clamp(sy, 0, ReflectionResolution - 1); sx = ReflectionResolution - 1 - sx; }
				Vec3 c = probe.pixels[level * ReflectionResolution * ReflectionResolution + sy * ReflectionResolution + sx];
				write(ox + (level % 2) * ReflectionBordered + x + 1, oy + (level / 2) * ReflectionBordered + y + 1, layer, c, true);
			}
	}
	screen->InitIrradianceVolume(width, height, layers, data);
	volumeOrigin = { b.origin.x, b.origin.y, b.origin.z, b.spacing };
	volumeSize = { (float)b.sizeX, (float)b.sizeY, (float)b.sizeZ, 1.f };
	reflectionOrigin = { b.reflectionOrigin.x, b.reflectionOrigin.y, b.reflectionOrigin.z, b.reflectionSpacing };
	reflectionSize = { float(b.reflectionX), float(b.reflectionY), float(b.reflectionZ), float(baseLayer) };
	reflectionAtlas = { float(tilesX), float(tilesY), float(ReflectionResolution), hasReflections ? 1.f : 0.f };
	volumeUploaded = true;
	unsigned validReflections = 0;
	for (const auto &probe : b.reflectionProbes) if (probe.valid) validReflections++;
	DPrintf(DMSG_NOTIFY, "GI: baked in %.2f s, %u/%u reflection probes, %.2f MiB combined atlas\n", (I_msTime() - b.startTime) / 1000.0,
		validReflections, unsigned(b.reflectionProbes.size()), data.Size() * sizeof(uint16_t) / (1024.0 * 1024.0));
}

} // namespace

void GI_LevelLoaded(FLevelLocals *level)
{
	GI_LevelUnloaded();
	bakeRequested = true;
}

void GI_LevelUnloaded()
{
	job.reset();
	bakeRequested = false;
	if (volumeUploaded && screen)
	{
		TArray<uint16_t> empty;
		screen->InitIrradianceVolume(0, 0, 0, empty);
	}
	volumeUploaded = false;
	volumeSize = { 0, 0, 0, 0 };
	reflectionAtlas = { 0, 0, 0, 0 };
}

void GI_Update(FLevelLocals *level)
{
	// Metal uses the Vulkan resource model. Other backends currently have no
	// irradiance-array upload implementation, so retain their ambient fallback.
	if (!screen || !screen->IsVulkan()) return;
	if ((!r_gi && !(r_sprite_pbr && r_sprite_reflections)) || !level) return;
	// Enabling reflections after a diffuse-only bake needs a new snapshot.
	if (volumeUploaded && r_sprite_pbr && r_sprite_reflections && reflectionAtlas.W == 0.f)
		GI_LevelLoaded(level);
	// Lights attach to their actors on the first tics, so wait briefly before capturing.
	if (bakeRequested && !job && level->maptime >= 2)
	{
		bakeRequested = false;
		CaptureAndStart(level);
	}
	if (job && job->done && !volumeUploaded)
	{
		Upload();
		job.reset();
	}
}

void GI_SetUniforms(HWViewpointUniforms &uniforms)
{
	const bool available = volumeUploaded && screen && screen->IsVulkan();
	uniforms.mReflectionOrigin = reflectionOrigin;
	uniforms.mReflectionSize = reflectionSize;
	uniforms.mReflectionAtlas = available && r_sprite_pbr && r_sprite_reflections ? reflectionAtlas : FVector4(0, 0, 0, 0);
	uniforms.mGIOrigin = volumeOrigin;
	uniforms.mGISize = available && r_gi ? volumeSize : FVector4(0, 0, 0, 0);
	uniforms.mGIStrength = r_gi_strength;
	uniforms.mGISaturation = r_gi_saturation;
}

CCMD(r_gi_rebake)
{
	// Rebuild the level mesh too, so the bake sees doors, lifts and
	// polyobjects where they are now.
	if (primaryLevel) DoomLevelMesh::Invalidate(primaryLevel);
	GI_LevelLoaded(nullptr);
}

FVector3 GI_TextureAlbedo(FTextureID id)
{
	Vec3 albedo = TextureAlbedo(id);
	return { albedo.x, albedo.y, albedo.z };
}
