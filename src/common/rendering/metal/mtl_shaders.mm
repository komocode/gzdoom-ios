/*
** mtl_shaders.mm
** Scene and postprocess shaders for the Metal backend.
**
** The GLSL is the same as the Vulkan backend's (assembled by VkShaderManager's
** shared helpers). glslang compiles it to SPIR-V and SPIRV-Cross translates
** that to MSL, remapping the Vulkan descriptor layout onto fixed Metal slots.
*/

#include "mtl_renderdevice.h"
#include "vulkan/shaders/vk_shader.h"
#include "hw_shaderpatcher.h"
#include "filesystem.h"
#include "cmdlib.h"
#include "printf.h"
#include <zvulkan/vulkanbuilders.h>
#include "spirv_msl.hpp"

using namespace SPIRV_CROSS_NAMESPACE;

static void AddBinding(CompilerMSL &msl, spv::ExecutionModel stage, uint32_t set, uint32_t binding, uint32_t buffer, uint32_t texture, uint32_t sampler)
{
	MSLResourceBinding b;
	b.stage = stage;
	b.desc_set = set;
	b.binding = binding;
	b.msl_buffer = buffer;
	b.msl_texture = texture;
	b.msl_sampler = sampler;
	msl.add_msl_resource_binding(b);
}

MtlShaderManager::MtlShaderManager(MetalRenderDevice *fb, bool raytracing) : fb(fb), mRaytracing(raytracing)
{
	static bool glslangReady = false;
	if (!glslangReady)
	{
		ShaderBuilder::Init();
		glslangReady = true;
	}
}

id<MTLFunction> MtlShaderManager::Compile(const char *name, const FString &glsl, bool vertex, MtlShaderLayout layout)
{
	@autoreleasepool
	{
		// Ray queries need SPIR-V 1.4.
		bool vulkan12 = layout == MtlShaderLayout::Scene && mRaytracing;
		std::vector<unsigned int> spirv;
		try
		{
			spirv = ShaderBuilder()
				.Type(vertex ? ShaderType::Vertex : ShaderType::Fragment)
				.AddSource(name, glsl.GetChars())
				.OnIncludeLocal(VkShaderManager::OnInclude)
				.OnIncludeSystem(VkShaderManager::OnInclude)
				.CreateSpirv(name, vulkan12);
		}
		catch (const std::exception &e)
		{
			I_FatalError("Metal: could not compile %s: %s", name, e.what());
		}

		std::string source;
		std::string entry;
		try
		{
			CompilerMSL msl(std::move(spirv));
			auto options = msl.get_msl_options();
#if TARGET_OS_IPHONE
			options.platform = CompilerMSL::Options::iOS;
#else
			options.platform = CompilerMSL::Options::macOS;
#endif
			options.set_msl_version(2, 4);
			msl.set_msl_options(options);

			// Vulkan's clip space has y pointing down.
			auto common = msl.get_common_options();
			common.vertex.flip_vert_y = true;
			msl.set_common_options(common);

			spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
			if (layout == MtlShaderLayout::Scene)
			{
				AddBinding(msl, model, 1, 0, MTLBUF_VIEWPOINT, 0, 0);
				AddBinding(msl, model, 1, 1, MTLBUF_MATRICES, 0, 0);
				AddBinding(msl, model, 1, 2, MTLBUF_STREAM, 0, 0);
				AddBinding(msl, model, 1, 3, MTLBUF_LIGHTS, 0, 0);
				AddBinding(msl, model, 1, 4, MTLBUF_BONES, 0, 0);
				AddBinding(msl, model, kPushConstDescSet, kPushConstBinding, MTLBUF_PUSHCONSTANTS, 0, 0);
				for (uint32_t i = 0; i < MTLTEX_MATERIAL_COUNT; i++)
					AddBinding(msl, model, 2, i, 0, i, i);
				AddBinding(msl, model, 0, 0, 0, MTLTEX_SHADOWMAP, MTLTEX_SHADOWMAP);
				AddBinding(msl, model, 0, 1, 0, MTLTEX_LIGHTMAP, MTLTEX_LIGHTMAP);
				AddBinding(msl, model, 0, 3, 0, MTLTEX_IRRADIANCE, MTLTEX_IRRADIANCE);
				AddBinding(msl, model, 0, 2, MTLBUF_ACCELERATION, 0, 0);
			}
			else
			{
				AddBinding(msl, model, kPushConstDescSet, kPushConstBinding, MTLBUF_PP_UNIFORMS, 0, 0);
				for (uint32_t i = 0; i < 16; i++)
					AddBinding(msl, model, 0, i, MTLBUF_PP_STORAGE + i, i, i);
			}

			source = msl.compile();
			entry = msl.get_cleansed_entry_point_name("main", model);
		}
		catch (const std::exception &e)
		{
			I_FatalError("Metal: could not translate %s: %s", name, e.what());
		}

		MTLCompileOptions *options = [MTLCompileOptions new];
		options.languageVersion = MTLLanguageVersion2_4;
		NSError *error = nil;
		id<MTLLibrary> library = [fb->device newLibraryWithSource:@(source.c_str()) options:options error:&error];
		if (!library)
		{
			Printf(PRINT_LOG, "%s\n", source.c_str());
			I_FatalError("Metal: could not build %s: %s", name, error.localizedDescription.UTF8String);
		}
		id<MTLFunction> function = [library newFunctionWithName:@(entry.c_str())];
		if (!function)
			I_FatalError("Metal: %s has no entry point %s", name, entry.c_str());
		function.label = @(name);
		return function;
	}
}

MtlShaderProgram MtlShaderManager::LoadProgram(const char *name, const char *vert_lump, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass)
{
	MtlShaderProgram program;
	bool raytracing = mRaytracing;
	FString vert = VkShaderManager::BuildVertexSource(vert_lump, defines, false, raytracing);
	FString frag = VkShaderManager::BuildFragmentSource(frag_lump, material_lump, light_lump, defines, alphatest, gbufferpass, false, raytracing, raytracing);
	program.vert = Compile(name, vert, true, MtlShaderLayout::Scene);
	program.frag = Compile(name, frag, false, MtlShaderLayout::Scene);
	return program;
}

bool MtlShaderManager::CompileNextShader()
{
	const char *mainvp = "shaders/glsl/main.vp";
	const char *mainfp = "shaders/glsl/main.fp";
	int i = compileIndex;
	bool gbuffer = compilePass == GBUFFER_PASS;

	if (compileState == 0)
	{
		// regular material shaders
		mMaterialShaders[compilePass].push_back(LoadProgram(defaultshaders[i].ShaderName, mainvp, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, true, gbuffer));

		compileIndex++;
		if (defaultshaders[compileIndex].ShaderName == nullptr)
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 1)
	{
		// material shaders without alpha test
		mMaterialShadersNAT[compilePass].push_back(LoadProgram(defaultshaders[i].ShaderName, mainvp, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, false, gbuffer));

		compileIndex++;
		if (compileIndex == SHADER_NoTexture)
		{
			compileIndex = 0;
			compileState++;
			if (usershaders.Size() == 0) compileState++;
		}
	}
	else if (compileState == 2)
	{
		// user shaders
		const FString &name = ExtractFileBase(usershaders[i].shader.GetChars());
		FString defines = defaultshaders[usershaders[i].shaderType].Defines + usershaders[i].defines;
		mMaterialShaders[compilePass].push_back(LoadProgram(name.GetChars(), mainvp, mainfp, usershaders[i].shader.GetChars(), defaultshaders[usershaders[i].shaderType].lightfunc, defines.GetChars(), true, gbuffer));

		compileIndex++;
		if (compileIndex >= (int)usershaders.Size())
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 3)
	{
		// effect shaders
		mEffectShaders[compilePass].push_back(LoadProgram(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines, true, gbuffer));

		compileIndex++;
		if (compileIndex >= MAX_EFFECTS)
		{
			compileIndex = 0;
			compilePass++;
			if (compilePass == MAX_PASS_TYPES)
			{
				compileIndex = -1; // done
				return true;
			}
			compileState = 0;
		}
	}
	return false;
}

MtlShaderProgram *MtlShaderManager::GetEffect(int effect, EPassType passType)
{
	if (compileIndex == -1 && effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[passType][effect].frag)
		return &mEffectShaders[passType][effect];
	return nullptr;
}

MtlShaderProgram *MtlShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (compileIndex != -1)
		return &mMaterialShaders[0][0];
	// indices 0-2 match the warping modes, 3 no texture, the following are custom
	if (!alphateston && eff < SHADER_NoTexture)
		return &mMaterialShadersNAT[passType][eff];
	if (eff < (unsigned int)mMaterialShaders[passType].size())
		return &mMaterialShaders[passType][eff];
	return nullptr;
}

MtlPPShader *MtlShaderManager::GetPPShader(PPShader *shader)
{
	if (!shader->Backend)
		shader->Backend = std::make_unique<MtlPPShader>(fb, shader);
	return static_cast<MtlPPShader*>(shader->Backend.get());
}

/////////////////////////////////////////////////////////////////////////////

static FString LoadPPShaderCode(const FString &lumpName, const FString &defines)
{
	int lump = fileSystem.CheckNumForFullName(lumpName.GetChars());
	if (lump == -1) I_FatalError("Unable to load '%s'", lumpName.GetChars());
	FString code = GetStringFromLump(lump);

	FString patchedCode;
	patchedCode.AppendFormat("#version %d\n#extension GL_GOOGLE_include_directive : enable\n", 450);
	patchedCode << defines;
	patchedCode << "#line 1\n";
	patchedCode << code;
	return patchedCode;
}

MtlPPShader::MtlPPShader(MetalRenderDevice *fb, PPShader *shader)
{
	FString prolog;
	if (!shader->Uniforms.empty())
		prolog = UniformBlockDecl::Create("Uniforms", shader->Uniforms, -1);
	prolog += shader->Defines;

	auto manager = fb->GetShaderManager();
	VertexFunction = manager->Compile(shader->VertexShader.GetChars(), LoadPPShaderCode(shader->VertexShader, ""), true, MtlShaderLayout::PostProcess);
	FragmentFunction = manager->Compile(shader->FragmentShader.GetChars(), LoadPPShaderCode(shader->FragmentShader, prolog), false, MtlShaderLayout::PostProcess);
}
