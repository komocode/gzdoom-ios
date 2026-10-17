
#pragma once

#include <memory>

#include "vectors.h"
#include "matrix.h"
#include "name.h"
#include "hw_renderstate.h"
#include "zvulkan/vulkanbuilders.h"
#include <list>
#include <cstring>

#define SHADER_MIN_REQUIRED_TEXTURE_LAYERS 11

class VulkanRenderDevice;
class VulkanDevice;
class VulkanShader;
class VkPPShader;
class PPShader;

struct MatricesUBO
{
	VSMatrix ModelMatrix;
	VSMatrix NormalModelMatrix;
	VSMatrix TextureMatrix;
};

#define MAX_STREAM_DATA ((int)(65536 / sizeof(StreamData)))

struct StreamUBO
{
	StreamData data[MAX_STREAM_DATA];
};

struct PushConstants
{
	int uTextureMode;
	float uAlphaThreshold;
	FVector2 uClipSplit;

	// Lighting + Fog
	float uLightLevel;
	float uFogDensity;
	float uLightFactor;
	float uLightDist;
	int uFogEnabled;

	// dynamic lights
	int uLightIndex;

	// Blinn glossiness and specular level
	FVector2 uSpecularMaterial;

	// bone animation
	int uBoneIndexBase;

	int uDataIndex;
	int padding1, padding2, padding3;
};

// What a scene program's fragment shader is built from, kept so that its sun
// variant (main.fp with SUN_LIGHT, hw_sun.h) can be compiled when a sunlit view
// first draws with it. Shared with the Metal backend.
struct SceneFragmentSource
{
	FString Name, FragLump, MaterialLump, LightLump, Defines;
	bool HasMaterial, HasLight, AlphaTest, GBufferPass;

	SceneFragmentSource(const char *name, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass)
		: Name(name), FragLump(frag_lump), MaterialLump(material_lump ? material_lump : ""), LightLump(light_lump ? light_lump : ""),
		Defines(defines ? defines : ""), HasMaterial(material_lump != nullptr), HasLight(light_lump != nullptr), AlphaTest(alphatest), GBufferPass(gbufferpass) {}

	// Null for programs without sun shading (everything but main.fp).
	static std::shared_ptr<SceneFragmentSource> ForSun(const char *name, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass)
	{
		if (!frag_lump || strcmp(frag_lump, "shaders/glsl/main.fp") != 0) return nullptr;
		return std::make_shared<SceneFragmentSource>(name, frag_lump, material_lump, light_lump, defines, alphatest, gbufferpass);
	}

	FString SunDefines() const { return Defines + "\n#define SUN_LIGHT\n"; }
	const char *Material() const { return HasMaterial ? MaterialLump.GetChars() : nullptr; }
	const char *Light() const { return HasLight ? LightLump.GetChars() : nullptr; }
};

class VkShaderProgram
{
public:
	std::unique_ptr<VulkanShader> vert;
	std::unique_ptr<VulkanShader> frag;
	// The fragment shader with the sun compiled in, made on first use (hw_sun.h).
	std::unique_ptr<VulkanShader> fragSun;
	std::shared_ptr<SceneFragmentSource> source;	// null without sun shading
};

class VkShaderManager
{
public:
	VkShaderManager(VulkanRenderDevice* fb);
	~VkShaderManager();

	void Deinit();

	VkShaderProgram *GetEffect(int effect, EPassType passType);
	VkShaderProgram *Get(unsigned int eff, bool alphateston, EPassType passType);
	bool CompileNextShader();

	// The fragment shader of a program's sun variant, compiled on first use. Programs
	// without sun shading return their own.
	VulkanShader *GetSunFragment(VkShaderProgram *program);
	// Compiles the sun variants of a pass's common scene programs and of these user
	// shaders ahead of their first use.
	void PrecacheSunShaders(EPassType passType, const TArray<int> &userShaders);

	VkPPShader* GetVkShader(PPShader* shader);

	void AddVkPPShader(VkPPShader* shader);
	void RemoveVkPPShader(VkPPShader* shader);

	// GLSL source assembly, shared with the Metal backend (which compiles the same
	// source to SPIR-V and translates it to MSL).
	static FString BuildVertexSource(const char *vert_lump, const char *defines, bool noClipDistance, bool vulkan12);
	static FString BuildFragmentSource(const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, bool noClipDistance, bool raytracing, bool vulkan12);
	static FString LoadPublicShaderLump(const char *lumpname);
	static FString LoadPrivateShaderLump(const char *lumpname);
	static ShaderIncludeResult OnInclude(FString headerName, FString includerName, size_t depth);

private:
	std::unique_ptr<VulkanShader> LoadVertShader(FString shadername, const char *vert_lump, const char *defines);
	std::unique_ptr<VulkanShader> LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass);

	static FString GetTargetGlslVersion(bool vulkan12);

	VulkanRenderDevice* fb = nullptr;

	std::vector<VkShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mEffectShaders[MAX_PASS_TYPES];
	uint8_t compilePass = 0, compileState = 0;
	int compileIndex = 0;

	std::list<VkPPShader*> PPShaders;
	friend class VkPPShader;
};
