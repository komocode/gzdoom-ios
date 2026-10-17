#pragma once

#include "hwrenderer/data/shaderuniforms.h"
#include <memory>
#include <map>
#include "intrect.h"
#include "matrix.h"
#include <functional>

struct PostProcessShader;

typedef FRenderStyle PPBlendMode;
typedef IntRect PPViewport;

class PPTexture;
class PPShader;

enum class ETonemapMode : uint8_t
{
	None,
	Uncharted2,
	HejlDawson,
	Reinhard,
	Linear,
	Palette,
	ACES,
	NumTonemapModes
};



enum class PPFilterMode { Nearest, Linear };
enum class PPWrapMode { Clamp, Repeat };
enum class PPTextureType { CurrentPipelineTexture, NextPipelineTexture, PPTexture, SceneColor, SceneFog, SceneNormal, SceneDepth, SwapChain, ShadowMap };

// Descriptor set 0 bindings of the optional postprocess inputs below. Input
// textures use the bindings from 0, the shadow map buffers 4 to 6.
enum
{
	PP_STORAGE_BINDINGPOINT = 10,		// read-only storage buffer (SetStorageData)
	PP_ACCELERATION_BINDINGPOINT = 11,	// scene acceleration structure (SetRayQuery)
};

class PPTextureInput
{
public:
	PPFilterMode Filter = PPFilterMode::Nearest;
	PPWrapMode Wrap = PPWrapMode::Clamp;
	PPTextureType Type = PPTextureType::CurrentPipelineTexture;
	PPTexture *Texture = nullptr;
};

class PPOutput
{
public:
	PPTextureType Type = PPTextureType::NextPipelineTexture;
	PPTexture *Texture = nullptr;
};

class PPUniforms
{
public:
	PPUniforms()
	{
	}

	PPUniforms(const PPUniforms &src)
	{
		Data = src.Data;
	}

	~PPUniforms()
	{
		Clear();
	}

	PPUniforms &operator=(const PPUniforms &src)
	{
		Data = src.Data;
		return *this;
	}

	void Clear()
	{
		Data.Clear();
	}

	template<typename T>
	void Set(const T &v)
	{
		if (Data.Size() != (int)sizeof(T))
		{
			Data.Resize(sizeof(T));
			memcpy(Data.Data(), &v, Data.Size());
		}
	}

	TArray<uint8_t> Data;
};

class PPRenderState
{
public:
	virtual ~PPRenderState() = default;

	virtual void PushGroup(const FString &name) = 0;
	virtual void PopGroup() = 0;

	virtual void Draw() = 0;

	void Clear()
	{
		Shader = nullptr;
		Textures = TArray<PPTextureInput>();
		Uniforms = PPUniforms();
		Viewport = PPViewport();
		BlendMode = PPBlendMode();
		Output = PPOutput();
		ShadowMapBuffers = false;
		StorageData = nullptr;
		StorageSize = 0;
		RayQuery = false;
	}

	// Backends that can bind a storage buffer from CPU data for one draw.
	virtual bool SupportsStorageData() const { return false; }
	// Backends whose postprocess shaders can trace rays against the scene.
	virtual bool SupportsRayQuery() const { return false; }

	void SetInputTexture(int index, PPTexture *texture, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		if ((int)Textures.Size() < index + 1)
			Textures.Resize(index + 1);
		auto &tex = Textures[index];
		tex.Filter = filter;
		tex.Wrap = wrap;
		tex.Type = PPTextureType::PPTexture;
		tex.Texture = texture;
	}

	void SetInputCurrent(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::CurrentPipelineTexture, filter, wrap);
	}

	void SetInputSceneColor(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneColor, filter, wrap);
	}

	void SetInputSceneFog(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneFog, filter, wrap);
	}

	void SetInputSceneNormal(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneNormal, filter, wrap);
	}

	void SetInputSceneDepth(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneDepth, filter, wrap);
	}

	void SetInputSpecialType(int index, PPTextureType type, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		if ((int)Textures.Size() < index + 1)
			Textures.Resize(index + 1);
		auto &tex = Textures[index];
		tex.Filter = filter;
		tex.Wrap = wrap;
		tex.Type = type;
		tex.Texture = nullptr;
	}

	void SetShadowMapBuffers(bool enable)
	{
		ShadowMapBuffers = enable;
	}

	// The data must stay valid until Draw returns.
	void SetStorageData(const void *data, int size)
	{
		StorageData = data;
		StorageSize = size;
	}

	void SetRayQuery(bool enable)
	{
		RayQuery = enable;
	}

	void SetOutputTexture(PPTexture *texture)
	{
		Output.Type = PPTextureType::PPTexture;
		Output.Texture = texture;
	}

	void SetOutputCurrent()
	{
		Output.Type = PPTextureType::CurrentPipelineTexture;
		Output.Texture = nullptr;
	}

	void SetOutputNext()
	{
		Output.Type = PPTextureType::NextPipelineTexture;
		Output.Texture = nullptr;
	}

	void SetOutputSceneColor()
	{
		Output.Type = PPTextureType::SceneColor;
		Output.Texture = nullptr;
	}

	void SetOutputSwapChain()
	{
		Output.Type = PPTextureType::SwapChain;
		Output.Texture = nullptr;
	}

	void SetOutputShadowMap()
	{
		Output.Type = PPTextureType::ShadowMap;
		Output.Texture = nullptr;
	}

	void SetNoBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_One;
		BlendMode.DestAlpha = STYLEALPHA_Zero;
		BlendMode.Flags = 0;
	}

	void SetAdditiveBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_One;
		BlendMode.DestAlpha = STYLEALPHA_One;
		BlendMode.Flags = 0;
	}

	void SetAlphaBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_Src;
		BlendMode.DestAlpha = STYLEALPHA_InvSrc;
		BlendMode.Flags = 0;
	}

	PPShader *Shader;
	TArray<PPTextureInput> Textures;
	PPUniforms Uniforms;
	PPViewport Viewport;
	PPBlendMode BlendMode;
	PPOutput Output;
	bool ShadowMapBuffers = false;
	const void *StorageData = nullptr;
	int StorageSize = 0;
	bool RayQuery = false;
};

enum class PixelFormat
{
	Rgba8,
	Rgba16f,
	R32f,
	Rg16f,
	Rgba16_snorm,
	R16f
};

class PPResource
{
public:
	PPResource()
	{
		Next = First;
		First = this;
		if (Next) Next->Prev = this;
	}

	PPResource(const PPResource &)
	{
		Next = First;
		First = this;
		if (Next) Next->Prev = this;
	}

	virtual ~PPResource()
	{
		if (Next) Next->Prev = Prev;
		if (Prev) Prev->Next = Next;
		else First = Next;
	}

	PPResource &operator=(const PPResource &other)
	{
		return *this;
	}

	static void ResetAll()
	{
		for (PPResource *cur = First; cur; cur = cur->Next)
			cur->ResetBackend();
	}

	virtual void ResetBackend() = 0;

private:
	static PPResource *First;
	PPResource *Prev = nullptr;
	PPResource *Next = nullptr;
};

class PPTextureBackend
{
public:
	virtual ~PPTextureBackend() = default;
};

class PPTexture : public PPResource
{
public:
	PPTexture() = default;
	PPTexture(int width, int height, PixelFormat format, std::shared_ptr<void> data = {}) : Width(width), Height(height), Format(format), Data(data) { }

	void ResetBackend() override { Backend.reset(); }

	int Width;
	int Height;
	PixelFormat Format;
	std::shared_ptr<void> Data;

	std::unique_ptr<PPTextureBackend> Backend;
};

class PPShaderBackend
{
public:
	virtual ~PPShaderBackend() = default;
};

class PPShader : public PPResource
{
public:
	PPShader() = default;
	PPShader(const FString &fragment, const FString &defines, const std::vector<UniformFieldDesc> &uniforms, int version = 330) : FragmentShader(fragment), Defines(defines), Uniforms(uniforms), Version(version) { }

	void ResetBackend() override { Backend.reset(); }

	FString VertexShader = "shaders/pp/screenquad.vp";
	FString FragmentShader;
	FString Defines;
	std::vector<UniformFieldDesc> Uniforms;
	int Version = 330;
	bool RayQuery = false;	// uses GL_EXT_ray_query; only used where SupportsRayQuery()

	std::unique_ptr<PPShaderBackend> Backend;
};

/////////////////////////////////////////////////////////////////////////////

struct ExtractUniforms
{
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExtractUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExtractUniforms, Offset) }
		};
	}
};

struct BlurUniforms
{
	float SampleWeights[8];

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleWeights0", UniformType::Float, offsetof(BlurUniforms, SampleWeights[0]) },
			{ "SampleWeights1", UniformType::Float, offsetof(BlurUniforms, SampleWeights[1]) },
			{ "SampleWeights2", UniformType::Float, offsetof(BlurUniforms, SampleWeights[2]) },
			{ "SampleWeights3", UniformType::Float, offsetof(BlurUniforms, SampleWeights[3]) },
			{ "SampleWeights4", UniformType::Float, offsetof(BlurUniforms, SampleWeights[4]) },
			{ "SampleWeights5", UniformType::Float, offsetof(BlurUniforms, SampleWeights[5]) },
			{ "SampleWeights6", UniformType::Float, offsetof(BlurUniforms, SampleWeights[6]) },
			{ "SampleWeights7", UniformType::Float, offsetof(BlurUniforms, SampleWeights[7]) },
		};
	}
};

enum { NumBloomLevels = 6, NumClassicBloomLevels = 4 };

struct BloomDownUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	FVector2 InvSize;	// texel size of the input
	float Threshold;
	float Knee;
	float ExposureScale;	// manual exposure, exp2(EV bias)
	float AutoExposure;	// 1 applies the eye adaptation EV
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(BloomDownUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(BloomDownUniforms, Offset) },
			{ "InvSize", UniformType::Vec2, offsetof(BloomDownUniforms, InvSize) },
			{ "Threshold", UniformType::Float, offsetof(BloomDownUniforms, Threshold) },
			{ "Knee", UniformType::Float, offsetof(BloomDownUniforms, Knee) },
			{ "ExposureScale", UniformType::Float, offsetof(BloomDownUniforms, ExposureScale) },
			{ "AutoExposure", UniformType::Float, offsetof(BloomDownUniforms, AutoExposure) },
			{ "Padding0", UniformType::Float, offsetof(BloomDownUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(BloomDownUniforms, Padding1) },
		};
	}
};

struct BloomUpUniforms
{
	FVector2 InvSize;	// texel size of the input
	float Intensity;
	float Padding;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "InvSize", UniformType::Vec2, offsetof(BloomUpUniforms, InvSize) },
			{ "Intensity", UniformType::Float, offsetof(BloomUpUniforms, Intensity) },
			{ "Padding", UniformType::Float, offsetof(BloomUpUniforms, Padding) },
		};
	}
};

class PPBlurLevel
{
public:
	PPViewport Viewport;
	PPTexture VTexture;
	PPTexture HTexture;
};

class PPBloom
{
public:
	void RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm);
	void RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight, float gameinfobluramount);

	// The modern bloom of this frame, composited in linear light by the tone
	// mapper (null when there is none or it was already added to the scene).
	PPTexture *ModernResult() { return HasModernResult ? &levels[0].VTexture : nullptr; }
	void ClearModernResult() { HasModernResult = false; }

private:
	bool HasModernResult = false;
	void BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical);
	void UpdateTextures(int width, int height);

	static float ComputeBlurGaussian(float n, float theta);
	static void ComputeBlurSamples(int sampleCount, float blurAmount, float *sampleWeights);

	PPBlurLevel levels[NumBloomLevels];
	int lastWidth = 0;
	int lastHeight = 0;

	PPShader BloomCombine = { "shaders/pp/bloomcombine.fp", "", {} };
	PPShader BloomExtract = { "shaders/pp/bloomextract.fp", "", ExtractUniforms::Desc() };
	PPShader BlurVertical = { "shaders/pp/blur.fp", "#define BLUR_VERTICAL\n", BlurUniforms::Desc() };
	PPShader BlurHorizontal = { "shaders/pp/blur.fp", "#define BLUR_HORIZONTAL\n", BlurUniforms::Desc() };

	// Progressive (downsample/upsample) bloom
	void RenderModernBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight);
	PPShader BloomPrefilter = { "shaders/pp/bloom_down.fp", "#define PREFILTER\n", BloomDownUniforms::Desc() };
	PPShader BloomDown = { "shaders/pp/bloom_down.fp", "", BloomDownUniforms::Desc() };
	PPShader BloomUp = { "shaders/pp/bloom_up.fp", "", BloomUpUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct LensUniforms
{
	float AspectRatio;
	float Scale;
	float Padding0, Padding1;
	FVector4 LensDistortionCoefficient;
	FVector4 CubicDistortionValue;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Aspect", UniformType::Float, offsetof(LensUniforms, AspectRatio) },
			{ "Scale", UniformType::Float, offsetof(LensUniforms, Scale) },
			{ "Padding0", UniformType::Float, offsetof(LensUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(LensUniforms, Padding1) },
			{ "k", UniformType::Vec4, offsetof(LensUniforms, LensDistortionCoefficient) },
			{ "kcube", UniformType::Vec4, offsetof(LensUniforms, CubicDistortionValue) }
		};
	}
};

class PPLensDistort
{
public:
	void Render(PPRenderState *renderstate);

private:
	PPShader Lens = { "shaders/pp/lensdistortion.fp", "", LensUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct FXAAUniforms
{
	FVector2 ReciprocalResolution;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ReciprocalResolution", UniformType::Vec2, offsetof(FXAAUniforms, ReciprocalResolution) },
			{ "Padding0", UniformType::Float, offsetof(FXAAUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(FXAAUniforms, Padding1) }
		};
	}
};

class PPFXAA
{
public:
	void Render(PPRenderState *renderstate);

private:
	void CreateShaders();
	int GetMaxVersion();
	FString GetDefines();

	PPShader FXAALuma;
	PPShader FXAA;
	int LastQuality = -1;
};

/////////////////////////////////////////////////////////////////////////////

struct ExposureExtractUniforms
{
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExposureExtractUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExposureExtractUniforms, Offset) }
		};
	}
};

struct ExposureCombineUniforms
{
	float ExposureBase;
	float ExposureMin;
	float ExposureScale;
	float ExposureSpeed;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ExposureBase", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureBase) },
			{ "ExposureMin", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureMin) },
			{ "ExposureScale", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureScale) },
			{ "ExposureSpeed", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureSpeed) }
		};
	}
};

class PPExposureLevel
{
public:
	PPViewport Viewport;
	PPTexture Texture;
};

struct ExposureAdaptUniforms
{
	float Key;
	float MinEV;
	float MaxEV;
	float Blend;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Key", UniformType::Float, offsetof(ExposureAdaptUniforms, Key) },
			{ "MinEV", UniformType::Float, offsetof(ExposureAdaptUniforms, MinEV) },
			{ "MaxEV", UniformType::Float, offsetof(ExposureAdaptUniforms, MaxEV) },
			{ "Blend", UniformType::Float, offsetof(ExposureAdaptUniforms, Blend) },
		};
	}
};

class PPCameraExposure
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	// Classic bloom threshold scale
	PPTexture CameraTexture = { 1, 1, PixelFormat::R32f };

	// Eye adaptation: the current exposure change in EV (r_autoexposure).
	PPTexture *AdaptedEV();
	float AutoExposureWeight() const { return AdaptValid ? 1.f : 0.f; }
	// Manual exposure: r_exposure plus the level / script bias, as a linear scale.
	float ExposureScale() const;
	bool ExposureActive() const;
	void SetExposureBias(float ev) { ExposureBias = ev; }

private:
	void UpdateTextures(int width, int height);
	void RenderClassic(PPRenderState *renderstate);
	void RenderAdaptation(PPRenderState *renderstate);

	std::vector<PPExposureLevel> ExposureLevels;
	bool FirstExposureFrame = true;
	bool FirstAdaptFrame = true;
	bool AdaptValid = false;
	uint64_t LastAdaptTime = 0;
	float ExposureBias = 0.f;
	PPTexture AdaptTexture = { 1, 1, PixelFormat::R32f };
	PPTexture ZeroTexture;

	PPShader ExposureExtract = { "shaders/pp/exposureextract.fp", "", ExposureExtractUniforms::Desc() };
	PPShader ExposureExtractLog = { "shaders/pp/exposureextract.fp", "#define LOGLUMINANCE\n", ExposureExtractUniforms::Desc() };
	PPShader ExposureAverage = { "shaders/pp/exposureaverage.fp", "", {}, 400 };
	PPShader ExposureCombine = { "shaders/pp/exposurecombine.fp", "", ExposureCombineUniforms::Desc() };
	PPShader ExposureAdapt = { "shaders/pp/exposureadapt.fp", "", ExposureAdaptUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct ColormapUniforms
{
	FVector4 MapStart;
	FVector4 MapRange;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "uFixedColormapStart", UniformType::Vec4, offsetof(ColormapUniforms, MapStart) },
			{ "uFixedColormapRange", UniformType::Vec4, offsetof(ColormapUniforms, MapRange) },
		};
	}
};

class PPColormap
{
public:
	void Render(PPRenderState *renderstate, int fixedcm, float flash);

private:
	PPShader Colormap = { "shaders/pp/colormap.fp", "", ColormapUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct TonemapUniforms
{
	FVector2 BloomScale;	// scene viewport within the screen, for the bloom texture
	FVector2 BloomOffset;
	float ExposureScale;
	float AutoExposure;
	float BloomIntensity;
	float WhiteScale;
	float Contrast;
	float Saturation;
	float TintAmount;
	float LutAmount;
	FVector4 ShadowTint;
	FVector4 HighTint;
	float LutBlend;
	float LutSize0;
	float LutSize1;
	float HdrPeak;	// EDR headroom of the display (1 = SDR)

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "BloomScale", UniformType::Vec2, offsetof(TonemapUniforms, BloomScale) },
			{ "BloomOffset", UniformType::Vec2, offsetof(TonemapUniforms, BloomOffset) },
			{ "ExposureScale", UniformType::Float, offsetof(TonemapUniforms, ExposureScale) },
			{ "AutoExposure", UniformType::Float, offsetof(TonemapUniforms, AutoExposure) },
			{ "BloomIntensity", UniformType::Float, offsetof(TonemapUniforms, BloomIntensity) },
			{ "WhiteScale", UniformType::Float, offsetof(TonemapUniforms, WhiteScale) },
			{ "Contrast", UniformType::Float, offsetof(TonemapUniforms, Contrast) },
			{ "Saturation", UniformType::Float, offsetof(TonemapUniforms, Saturation) },
			{ "TintAmount", UniformType::Float, offsetof(TonemapUniforms, TintAmount) },
			{ "LutAmount", UniformType::Float, offsetof(TonemapUniforms, LutAmount) },
			{ "ShadowTint", UniformType::Vec4, offsetof(TonemapUniforms, ShadowTint) },
			{ "HighTint", UniformType::Vec4, offsetof(TonemapUniforms, HighTint) },
			{ "LutBlend", UniformType::Float, offsetof(TonemapUniforms, LutBlend) },
			{ "LutSize0", UniformType::Float, offsetof(TonemapUniforms, LutSize0) },
			{ "LutSize1", UniformType::Float, offsetof(TonemapUniforms, LutSize1) },
			{ "HdrPeak", UniformType::Float, offsetof(TonemapUniforms, HdrPeak) },
		};
	}
};

// Colour grading LUTs and the exposure bias of the current area. Sources, in
// order of priority: ZScript (PPShader.SetGradingLUT / SetExposureBias), the
// level's MAPINFO 'gradinglut' / 'exposurebias', then r_grading_lut. Changes
// crossfade between the old and the new LUT.
class PPGrading
{
public:
	void SetLevel(const FString &levelName, const FString &lut, float exposureBias);
	void SetOverrideLUT(const FString &lut, float fadeSeconds);
	void ClearOverrideLUT(float fadeSeconds);
	void SetOverrideExposure(float ev, float fadeSeconds);
	void ClearOverrideExposure(float fadeSeconds);
	// Advances the crossfades once per frame; returns the exposure bias in EV.
	float Update();
	bool Active() const { return Current.IsNotEmpty() || Target.IsNotEmpty(); }
	// The LUT texture for a name; an empty name is the identity LUT.
	PPTexture *GetLUT(const FString &name, float &size);
	const FString &CurrentLUT() const { return Current; }
	const FString &TargetLUT() const { return Target; }
	float Blend() const { return Fade; }

private:
	void StartEVFade(float to, float seconds);

	FString LevelName, LevelLUT;
	float LevelEV = 0.f;
	FString OverrideLUT;
	bool HasOverrideLUT = false;
	bool HasOverrideEV = false;
	float LutFadeSeconds = 0.5f;

	FString Current, Target;	// the displayed LUT and the one faded to
	float Fade = 0.f;

	float EVFrom = 0.f, EVTo = 0.f, EVFade = 1.f, EVFadeSeconds = 1.f;
	uint64_t LastTime = 0;

	std::map<FString, std::unique_ptr<PPTexture>> Textures;
	std::map<FString, int> Sizes;
	std::unique_ptr<PPTexture> Identity;
};

class PPTonemap
{
public:
	void SetTonemapMode(ETonemapMode tm) { level_tonemap = tm; }
	void Render(PPRenderState *renderstate);
	void ClearTonemapPalette() { PaletteTexture = {}; }
	bool IsPalette() const;

private:
	void UpdateTextures();

	PPTexture PaletteTexture;
	PPTexture BlackTexture;

	PPShader LinearShader = { "shaders/pp/tonemap.fp", "#define LINEAR\n", TonemapUniforms::Desc() };
	PPShader ReinhardShader = { "shaders/pp/tonemap.fp", "#define REINHARD\n", TonemapUniforms::Desc() };
	PPShader HejlDawsonShader = { "shaders/pp/tonemap.fp", "#define HEJLDAWSON\n", TonemapUniforms::Desc() };
	PPShader Uncharted2Shader = { "shaders/pp/tonemap.fp", "#define UNCHARTED2\n", TonemapUniforms::Desc() };
	PPShader PaletteShader = { "shaders/pp/tonemap.fp", "#define PALETTE\n", {} };
	PPShader ACESShader = { "shaders/pp/tonemap.fp", "#define ACES\n", TonemapUniforms::Desc() };
	ETonemapMode level_tonemap = ETonemapMode::None;
};

/////////////////////////////////////////////////////////////////////////////

struct LinearDepthUniforms
{
	int SampleIndex;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float InverseDepthRangeA;
	float InverseDepthRangeB;
	float Padding0, Padding1, Padding2;
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleIndex", UniformType::Int, offsetof(LinearDepthUniforms, SampleIndex) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(LinearDepthUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(LinearDepthUniforms, LinearizeDepthB) },
			{ "InverseDepthRangeA", UniformType::Float, offsetof(LinearDepthUniforms, InverseDepthRangeA) },
			{ "InverseDepthRangeB", UniformType::Float, offsetof(LinearDepthUniforms, InverseDepthRangeB) },
			{ "Padding0", UniformType::Float, offsetof(LinearDepthUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(LinearDepthUniforms, Padding1) },
			{ "Padding2", UniformType::Float, offsetof(LinearDepthUniforms, Padding2) },
			{ "Scale", UniformType::Vec2, offsetof(LinearDepthUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(LinearDepthUniforms, Offset) }
		};
	}
};

struct SSAOUniforms
{
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 InvFullResolution;
	float NDotVBias;
	float NegInvR2;
	float RadiusToScreen;
	float AOMultiplier;
	float AOStrength;
	int SampleIndex;
	float Padding0, Padding1;
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "UVToViewA", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewB) },
			{ "InvFullResolution", UniformType::Vec2, offsetof(SSAOUniforms, InvFullResolution) },
			{ "NDotVBias", UniformType::Float, offsetof(SSAOUniforms, NDotVBias) },
			{ "NegInvR2", UniformType::Float, offsetof(SSAOUniforms, NegInvR2) },
			{ "RadiusToScreen", UniformType::Float, offsetof(SSAOUniforms, RadiusToScreen) },
			{ "AOMultiplier", UniformType::Float, offsetof(SSAOUniforms, AOMultiplier) },
			{ "AOStrength", UniformType::Float, offsetof(SSAOUniforms, AOStrength) },
			{ "SampleIndex", UniformType::Int, offsetof(SSAOUniforms, SampleIndex) },
			{ "Padding0", UniformType::Float, offsetof(SSAOUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(SSAOUniforms, Padding1) },
			{ "Scale", UniformType::Vec2, offsetof(SSAOUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(SSAOUniforms, Offset) },
		};
	}
};

struct DepthBlurUniforms
{
	float BlurSharpness;
	float PowExponent;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "BlurSharpness", UniformType::Float, offsetof(DepthBlurUniforms, BlurSharpness) },
			{ "PowExponent", UniformType::Float, offsetof(DepthBlurUniforms, PowExponent) },
			{ "Padding0", UniformType::Float, offsetof(DepthBlurUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(DepthBlurUniforms, Padding1) }
		};
	}
};

struct AmbientCombineUniforms
{
	int SampleCount;
	int DebugMode, Padding1, Padding2;
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleCount", UniformType::Int, offsetof(AmbientCombineUniforms, SampleCount) },
			{ "DebugMode", UniformType::Int, offsetof(AmbientCombineUniforms, DebugMode) },
			{ "Padding1", UniformType::Int, offsetof(AmbientCombineUniforms, Padding1) },
			{ "Padding2", UniformType::Int, offsetof(AmbientCombineUniforms, Padding2) },
			{ "Scale", UniformType::Vec2, offsetof(AmbientCombineUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(AmbientCombineUniforms, Offset) }
		};
	}
};

class PPAmbientOcclusion
{
public:
	PPAmbientOcclusion();
	void Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight);
	static bool GBufferNeeded();

	// Half-resolution linear depth of the scene viewport, written by Render.
	// The scene shaders read it afterwards for soft particles.
	PPTexture *GetLinearDepth() { return &LinearDepthTexture; }
	int GetDepthWidth() const { return AmbientWidth; }
	int GetDepthHeight() const { return AmbientHeight; }

private:
	void CreateShaders();
	void UpdateTextures(int width, int height);

	enum Quality
	{
		Off,
		LowQuality,
		MediumQuality,
		HighQuality,
		NumQualityModes
	};

	int AmbientWidth = 0;
	int AmbientHeight = 0;

	int LastQuality = -1;
	int LastWidth = 0;
	int LastHeight = 0;

	PPShader LinearDepth;
	PPShader LinearDepthMS;
	PPShader AmbientOcclude;
	PPShader AmbientOccludeMS;
	PPShader BlurVertical;
	PPShader BlurHorizontal;
	PPShader Combine;
	PPShader CombineMS;

	PPTexture LinearDepthTexture;
	PPTexture Ambient0;
	PPTexture Ambient1;

	enum { NumAmbientRandomTextures = 3 };
	PPTexture AmbientRandomTexture[NumAmbientRandomTextures];
};

struct ReflectionUniforms
{
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 Scale;
	FVector2 Offset;
	FVector2 InvSize;	// texel size of the linear depth texture
	float Strength;
	float MaxDistance;
	float Thickness;
	int Steps;
	int SampleIndex;
	float Padding;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "UVToViewA", UniformType::Vec2, offsetof(ReflectionUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(ReflectionUniforms, UVToViewB) },
			{ "Scale", UniformType::Vec2, offsetof(ReflectionUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ReflectionUniforms, Offset) },
			{ "InvSize", UniformType::Vec2, offsetof(ReflectionUniforms, InvSize) },
			{ "Strength", UniformType::Float, offsetof(ReflectionUniforms, Strength) },
			{ "MaxDistance", UniformType::Float, offsetof(ReflectionUniforms, MaxDistance) },
			{ "Thickness", UniformType::Float, offsetof(ReflectionUniforms, Thickness) },
			{ "Steps", UniformType::Int, offsetof(ReflectionUniforms, Steps) },
			{ "SampleIndex", UniformType::Int, offsetof(ReflectionUniforms, SampleIndex) },
			{ "Padding", UniformType::Float, offsetof(ReflectionUniforms, Padding) },
		};
	}
};

// Volumetric lights live in a storage buffer, 4 vec4s each (see volumetric.fp),
// so the push constant block stays small enough for every Vulkan driver.
enum { MaxVolumetricLights = 32, VolumetricLightVec4s = 4 };

// Medium for the volumetric pass: exponential height fog (optionally rolling
// with noise) and the camera sector's coloured fog. Densities are extinction
// per map unit.
struct VolumetricFog
{
	FVector3 Color = { 0.0f, 0.0f, 0.0f };	// height fog colour (linear 0-1)
	float Density = 0.0f;					// at and below Base; 0 disables height fog
	float Base = 0.0f;						// map height where the fog thins out
	float Falloff = 64.0f;					// height over which the density falls to 1/e
	float Noise = 0.0f;						// 0-1 amount of animated noise
	float NoiseScale = 1.0f / 160.0f;		// noise frequency per map unit
	FVector3 NoiseOffset = { 0.0f, 0.0f, 0.0f };	// wind scroll, GL world space
	float SectorDensity = 0.0f;				// sector fog around the camera
	float RainScatter = 0.0f;				// extra scattering of rain (MAPINFO rainscatter), relative to thin air
	float Strength = 1.0f;					// multiplies r_volumetric_strength (MAPINFO volumetricstrength)
};

struct VolumetricUniforms
{
	FVector4 ViewX;	// view to world rotation (GL world space); w: camera position
	FVector4 ViewY;
	FVector4 ViewZ;
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 Scale;	// full resolution: the 3D viewport in the scene depth buffer
	FVector2 Offset;
	FVector4 FogColor;	// rgb, noise amount
	FVector4 FogParams;	// base height, 1 / falloff, density, sector fog density
	FVector4 FogNoise;	// noise offset, noise scale
	float LinearizeDepthA;
	float LinearizeDepthB;
	float InverseDepthRangeA;
	float InverseDepthRangeB;
	float Strength;
	float FogScatter;
	float MaxDistance;
	float JitterOffset;
	int LightCount;
	int Samples;
	int SampleIndex;
	float RainScatter;	// replaces the former padding: same size and layout

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ViewX", UniformType::Vec4, offsetof(VolumetricUniforms, ViewX) },
			{ "ViewY", UniformType::Vec4, offsetof(VolumetricUniforms, ViewY) },
			{ "ViewZ", UniformType::Vec4, offsetof(VolumetricUniforms, ViewZ) },
			{ "UVToViewA", UniformType::Vec2, offsetof(VolumetricUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(VolumetricUniforms, UVToViewB) },
			{ "Scale", UniformType::Vec2, offsetof(VolumetricUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(VolumetricUniforms, Offset) },
			{ "FogColor", UniformType::Vec4, offsetof(VolumetricUniforms, FogColor) },
			{ "FogParams", UniformType::Vec4, offsetof(VolumetricUniforms, FogParams) },
			{ "FogNoise", UniformType::Vec4, offsetof(VolumetricUniforms, FogNoise) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(VolumetricUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(VolumetricUniforms, LinearizeDepthB) },
			{ "InverseDepthRangeA", UniformType::Float, offsetof(VolumetricUniforms, InverseDepthRangeA) },
			{ "InverseDepthRangeB", UniformType::Float, offsetof(VolumetricUniforms, InverseDepthRangeB) },
			{ "Strength", UniformType::Float, offsetof(VolumetricUniforms, Strength) },
			{ "FogScatter", UniformType::Float, offsetof(VolumetricUniforms, FogScatter) },
			{ "MaxDistance", UniformType::Float, offsetof(VolumetricUniforms, MaxDistance) },
			{ "JitterOffset", UniformType::Float, offsetof(VolumetricUniforms, JitterOffset) },
			{ "LightCount", UniformType::Int, offsetof(VolumetricUniforms, LightCount) },
			{ "Samples", UniformType::Int, offsetof(VolumetricUniforms, Samples) },
			{ "SampleIndex", UniformType::Int, offsetof(VolumetricUniforms, SampleIndex) },
			{ "RainScatter", UniformType::Float, offsetof(VolumetricUniforms, RainScatter) },
		};
	}
};

static_assert(sizeof(VolumetricUniforms) <= 256, "volumetric uniforms must fit 256 bytes of push constants");

struct VolumetricBlurUniforms
{
	FVector2 InvSize;
	float DepthSharpness;
	float Padding;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "InvSize", UniformType::Vec2, offsetof(VolumetricBlurUniforms, InvSize) },
			{ "DepthSharpness", UniformType::Float, offsetof(VolumetricBlurUniforms, DepthSharpness) },
			{ "Padding", UniformType::Float, offsetof(VolumetricBlurUniforms, Padding) },
		};
	}
};

struct CompositeUniforms
{
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(CompositeUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(CompositeUniforms, Offset) },
		};
	}
};

struct SSRUniforms
{
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 Scale;
	FVector2 Offset;
	FVector2 InvSize;	// texel size of the trace (linear depth) texture
	float Strength;
	float MaxDistance;
	float Thickness;
	int Steps;
	int SampleIndex;
	float Stretch;		// vertical elongation of rough reflections
	float Jitter;		// per-frame noise seed while TAA accumulates; 0 = static
	float FarDepth;		// linear depth of cleared (sky) pixels
	float LevelHeight;	// height of the blur chain's first level in pixels
	float Padding0, Padding1, Padding2;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "UVToViewA", UniformType::Vec2, offsetof(SSRUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(SSRUniforms, UVToViewB) },
			{ "Scale", UniformType::Vec2, offsetof(SSRUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(SSRUniforms, Offset) },
			{ "InvSize", UniformType::Vec2, offsetof(SSRUniforms, InvSize) },
			{ "Strength", UniformType::Float, offsetof(SSRUniforms, Strength) },
			{ "MaxDistance", UniformType::Float, offsetof(SSRUniforms, MaxDistance) },
			{ "Thickness", UniformType::Float, offsetof(SSRUniforms, Thickness) },
			{ "Steps", UniformType::Int, offsetof(SSRUniforms, Steps) },
			{ "SampleIndex", UniformType::Int, offsetof(SSRUniforms, SampleIndex) },
			{ "Stretch", UniformType::Float, offsetof(SSRUniforms, Stretch) },
			{ "Jitter", UniformType::Float, offsetof(SSRUniforms, Jitter) },
			{ "FarDepth", UniformType::Float, offsetof(SSRUniforms, FarDepth) },
			{ "LevelHeight", UniformType::Float, offsetof(SSRUniforms, LevelHeight) },
			{ "Padding0", UniformType::Float, offsetof(SSRUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(SSRUniforms, Padding1) },
			{ "Padding2", UniformType::Float, offsetof(SSRUniforms, Padding2) },
		};
	}
};

struct SSRDownUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	FVector2 InvSize;	// texel size of the input
	int SampleIndex;
	float Padding;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(SSRDownUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(SSRDownUniforms, Offset) },
			{ "InvSize", UniformType::Vec2, offsetof(SSRDownUniforms, InvSize) },
			{ "SampleIndex", UniformType::Int, offsetof(SSRDownUniforms, SampleIndex) },
			{ "Padding", UniformType::Float, offsetof(SSRDownUniforms, Padding) },
		};
	}
};

struct SSRCompositeUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	int SampleIndex;
	float Strength;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(SSRCompositeUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(SSRCompositeUniforms, Offset) },
			{ "SampleIndex", UniformType::Int, offsetof(SSRCompositeUniforms, SampleIndex) },
			{ "Strength", UniformType::Float, offsetof(SSRCompositeUniforms, Strength) },
			{ "Padding0", UniformType::Float, offsetof(SSRCompositeUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(SSRCompositeUniforms, Padding1) },
		};
	}
};

enum { NumReflectionLevels = 5 };

// Screen-space GI and volumetric light scattering run after the opaque
// geometry, reuse the SSAO linear depth and are composited into the scene
// before translucent objects are drawn.
//
// Screen-space reflections run after the translucent pass and the GPU
// particles of the main view, so sprites, glows, particles and portal
// contents appear in them. They trace against a linear depth taken at that
// point (opaque sprites write depth for this), blur by surface roughness
// through a small colour pyramid with a vertical stretch, and fall back to the
// blurred screen (or the sky) where rays miss.
class PPSceneEffects
{
public:
	void Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight, PPTexture *linearDepth, int depthWidth, int depthHeight);
	void RenderReflections(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight);
	bool Active() const;

	// Volumetric input for the next frame, set by the scene renderer: the
	// packed light list (VolumetricLightVec4s each, see volumetric.fp), the
	// view matrix and the participating medium.
	void SetVolumetricLights(const FVector4 *lights, int count);
	void SetVolumetricView(const float *viewMatrix);
	void SetVolumetricFog(const VolumetricFog &fog) { Fog = fog; }
	bool VolumetricActive() const;
	// Whether the last frame could trace volumetric shadow rays.
	bool VolumetricRayQuery() const { return LastRayQuery; }

private:
	void UpdateTextures(int width, int height);
	void UpdateReflectionTextures(int width, int height);
	void RenderVolumetrics(PPRenderState *renderstate, const FVector2 &uvToViewA, const FVector2 &uvToViewB, PPTexture *linearDepth);
	void CreateScatterShaders();

	int Width = 0, Height = 0;
	int VolumetricWidth = 0, VolumetricHeight = 0;
	PPTexture Volumetrics;
	PPTexture VolumetricsBlur;
	PPTexture Indirect;
	VolumetricFog Fog;
	int VolumetricLightCount = 0;
	FVector4 VolumetricLights[MaxVolumetricLights * VolumetricLightVec4s] = {};
	FVector4 ViewToWorld[4] = {};
	int FrameIndex = 0;
	bool LastRayQuery = false;

	int ReflectWidth = 0, ReflectHeight = 0;
	unsigned ReflectFrame = 0;
	PPTexture TraceDepth;
	PPTexture Reflections;
	PPTexture ReflectLevels[NumReflectionLevels];

	PPShader TraceLinearDepth = { "shaders/pp/lineardepth.fp", "", LinearDepthUniforms::Desc() };
	PPShader TraceLinearDepthMS = { "shaders/pp/lineardepth.fp", "#define MULTISAMPLE\n", LinearDepthUniforms::Desc() };
	PPShader ReflectDownScene = { "shaders/pp/ssrdown.fp", "#define FROM_SCENE\n", SSRDownUniforms::Desc() };
	PPShader ReflectDownSceneMS = { "shaders/pp/ssrdown.fp", "#define FROM_SCENE\n#define MULTISAMPLE\n", SSRDownUniforms::Desc() };
	PPShader ReflectDown = { "shaders/pp/ssrdown.fp", "", SSRDownUniforms::Desc() };
	PPShader Reflect = { "shaders/pp/ssr.fp", "", SSRUniforms::Desc() };
	PPShader ReflectMS = { "shaders/pp/ssr.fp", "#define MULTISAMPLE\n", SSRUniforms::Desc() };
	PPShader ReflectComposite = { "shaders/pp/ssrcomposite.fp", "", SSRCompositeUniforms::Desc() };
	PPShader ReflectCompositeMS = { "shaders/pp/ssrcomposite.fp", "#define MULTISAMPLE\n", SSRCompositeUniforms::Desc() };
	PPShader Gather = { "shaders/pp/ssgi.fp", "", ReflectionUniforms::Desc() };
	PPShader GatherMS = { "shaders/pp/ssgi.fp", "#define MULTISAMPLE\n", ReflectionUniforms::Desc() };
	PPShader Composite = { "shaders/pp/composite.fp", "", CompositeUniforms::Desc() };

	// Half resolution, full resolution, full resolution with MSAA; then the
	// same three tracing shadow rays.
	enum { ScatterHalf, ScatterFull, ScatterFullMS, NumScatterVariants };
	PPShader Scatter[NumScatterVariants * 2];
	PPShader ScatterBlur = { "shaders/pp/volumetricblur.fp", "", VolumetricBlurUniforms::Desc() };
	bool ScatterShadersCreated = false;
};

struct TemporalUniforms
{
	float Reprojection[16];	// current view space to previous frame clip space
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 Scale;
	FVector2 Offset;
	FVector2 InvSize;
	FVector2 Jitter;	// this frame's subpixel offset in scene UV units
	FVector2 InputScale;	// the rendered (possibly smaller) part of the scene buffers
	FVector2 InputOffset;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float Feedback;	// history weight, 0 discards the history
	float Upscale;	// 1 when the input is smaller than the output
	float Reactive;	// 1 when the scene wrote a reactive mask (normal alpha)
	float Padding0, Padding1, Padding2;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Reprojection", UniformType::Mat4, offsetof(TemporalUniforms, Reprojection) },
			{ "UVToViewA", UniformType::Vec2, offsetof(TemporalUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(TemporalUniforms, UVToViewB) },
			{ "Scale", UniformType::Vec2, offsetof(TemporalUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(TemporalUniforms, Offset) },
			{ "InvSize", UniformType::Vec2, offsetof(TemporalUniforms, InvSize) },
			{ "Jitter", UniformType::Vec2, offsetof(TemporalUniforms, Jitter) },
			{ "InputScale", UniformType::Vec2, offsetof(TemporalUniforms, InputScale) },
			{ "InputOffset", UniformType::Vec2, offsetof(TemporalUniforms, InputOffset) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(TemporalUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(TemporalUniforms, LinearizeDepthB) },
			{ "Feedback", UniformType::Float, offsetof(TemporalUniforms, Feedback) },
			{ "Upscale", UniformType::Float, offsetof(TemporalUniforms, Upscale) },
			{ "Reactive", UniformType::Float, offsetof(TemporalUniforms, Reactive) },
			{ "Padding0", UniformType::Float, offsetof(TemporalUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(TemporalUniforms, Padding1) },
			{ "Padding2", UniformType::Float, offsetof(TemporalUniforms, Padding2) },
		};
	}
};

// Temporal anti-aliasing. The 3D projection is jittered by a subpixel Halton
// offset each frame; this pass reprojects last frame's result through the
// depth buffer and camera motion, clamps it to the current neighbourhood (which
// also stops ghosting on moving sprites, which have no motion vectors) and
// blends. Runs on the HDR scene before bloom and tone mapping.
//
// With r_upscale below 1 it is also a temporal upscaler: the 3D scene is
// rendered into a smaller part of the scene viewport, and the resolve
// reconstructs the full viewport from the jittered samples of many frames.
// Everything after it (exposure, bloom, tone mapping, the weapon and the HUD)
// runs at full resolution.
class PPTemporalAA
{
public:
	// Called before the 3D scene is rendered: returns the viewport to render it
	// into, which is smaller than the scene viewport when upscaling.
	IntRect BeginUpscale(const IntRect &sceneViewport);
	// Called by the scene renderer for the main view; returns the projection jitter in NDC.
	FVector2 BeginFrame(int sceneWidth, int sceneHeight);
	void SetCamera(const VSMatrix &projection, const VSMatrix &view, const DVector3 &pos);
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);
	// Restores the full scene viewport if the resolve didn't run.
	void EndUpscale();
	bool Enabled() const;
	// The scene pass writes the reactive mask into the normal G-buffer's alpha.
	bool ReactiveMask() const;

private:
	void UpdateTextures(int width, int height);
	bool PlatformUpscale(PPRenderState *renderstate, const TemporalUniforms &u, bool reset);

	// Inputs and output of a platform upscaler (MetalFX)
	PPTexture UpscaleColor, UpscaleDepth, UpscaleMotion, UpscaleReactive, UpscaleOutput;
	int UpscaleInputWidth = 0, UpscaleInputHeight = 0, UpscaleOutputWidth = 0, UpscaleOutputHeight = 0;

	bool Upscaling = false;
	IntRect OutputViewport = {};
	float UpscaleRatio = 1.0f;

	int Width = 0, Height = 0;
	PPTexture History;
	bool HistoryValid = false;
	bool CameraValid = false;
	bool PrevValid = false;
	unsigned FrameIndex = 0;
	FVector2 JitterNDC = { 0.0f, 0.0f };
	VSMatrix CurProj, CurView, PrevProj, PrevView;
	DVector3 CurPos, PrevPos;
	uint64_t LastFrame = 0;

	PPShader Resolve = { "shaders/pp/taa.fp", "", TemporalUniforms::Desc() };
	PPShader UpscaleDepthShader = { "shaders/pp/taainputs.fp", "", TemporalUniforms::Desc() };
	PPShader UpscaleMotionShader = { "shaders/pp/taainputs.fp", "#define MOTION\n", TemporalUniforms::Desc() };
	PPShader UpscaleReactiveShader = { "shaders/pp/taainputs.fp", "#define REACTIVE\n", TemporalUniforms::Desc() };
	PPShader Copy = { "shaders/pp/composite.fp", "", CompositeUniforms::Desc() };
};

struct FinishUniforms
{
	FVector2 InvSize;
	float Sharpen;
	float Vignette;
	float Grain;
	float Time;
	float Aspect;
	float MaxValue;	// 1, or the EDR headroom (gamma encoded) with HDR output

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "InvSize", UniformType::Vec2, offsetof(FinishUniforms, InvSize) },
			{ "Sharpen", UniformType::Float, offsetof(FinishUniforms, Sharpen) },
			{ "Vignette", UniformType::Float, offsetof(FinishUniforms, Vignette) },
			{ "Grain", UniformType::Float, offsetof(FinishUniforms, Grain) },
			{ "Time", UniformType::Float, offsetof(FinishUniforms, Time) },
			{ "Aspect", UniformType::Float, offsetof(FinishUniforms, Aspect) },
			{ "MaxValue", UniformType::Float, offsetof(FinishUniforms, MaxValue) },
		};
	}
};

// Contrast-adaptive sharpening, vignette and film grain in one pass.
class PPFinish
{
public:
	void Render(PPRenderState *renderstate);

private:
	PPShader Finish = { "shaders/pp/finish.fp", "", FinishUniforms::Desc() };
};

struct PresentUniforms
{
	float InvGamma;
	float Contrast;
	float Brightness;
	float Saturation;
	int GrayFormula;
	int WindowPositionParity; // top-of-window might not be top-of-screen
	FVector2 Scale;
	FVector2 Offset;
	float ColorScale;
	int HdrMode;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "InvGamma", UniformType::Float, offsetof(PresentUniforms, InvGamma) },
			{ "Contrast", UniformType::Float, offsetof(PresentUniforms, Contrast) },
			{ "Brightness", UniformType::Float, offsetof(PresentUniforms, Brightness) },
			{ "Saturation", UniformType::Float, offsetof(PresentUniforms, Saturation) },
			{ "GrayFormula", UniformType::Int, offsetof(PresentUniforms, GrayFormula) },
			{ "WindowPositionParity", UniformType::Int, offsetof(PresentUniforms, WindowPositionParity) },
			{ "UVScale", UniformType::Vec2, offsetof(PresentUniforms, Scale) },
			{ "UVOffset", UniformType::Vec2, offsetof(PresentUniforms, Offset) },
			{ "ColorScale", UniformType::Float, offsetof(PresentUniforms, ColorScale) },
			{ "HdrMode", UniformType::Int, offsetof(PresentUniforms, HdrMode) }
		};
	}
};

class PPPresent
{
public:
	PPPresent();

	PPTexture Dither;

	PPShader Present = { "shaders/pp/present.fp", "", PresentUniforms::Desc() };
	PPShader Checker3D = { "shaders/pp/present_checker3d.fp", "", PresentUniforms::Desc() };
	PPShader Column3D = { "shaders/pp/present_column3d.fp", "", PresentUniforms::Desc() };
	PPShader Row3D = { "shaders/pp/present_row3d.fp", "", PresentUniforms::Desc() };
};

struct ShadowMapUniforms
{
	float ShadowmapQuality;
	int NodesCount;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ShadowmapQuality", UniformType::Float, offsetof(ShadowMapUniforms, ShadowmapQuality) },
			{ "NodesCount", UniformType::Int, offsetof(ShadowMapUniforms, NodesCount) },
			{ "Padding0", UniformType::Float, offsetof(ShadowMapUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(ShadowMapUniforms, Padding1) },
		};
	}
};

class PPCustomShaderInstance
{
public:
	PPCustomShaderInstance(PostProcessShader *desc);

	void Run(PPRenderState *renderstate);

	PostProcessShader *Desc = nullptr;

private:
	void AddUniformField(size_t &offset, const FString &name, UniformType type, size_t fieldsize, size_t alignment = 0);
	void SetTextures(PPRenderState *renderstate);
	void SetUniforms(PPRenderState *renderstate);

	PPShader Shader;
	int UniformStructSize = 0;
	std::vector<UniformFieldDesc> Fields;
	std::vector<std::unique_ptr<FString>> FieldNames;
	std::map<FTexture*, std::unique_ptr<PPTexture>> Textures;
	std::map<FString, size_t> FieldOffset;
};

class PPCustomShaders
{
public:
	void Run(PPRenderState *renderstate, FString target);

private:
	void CreateShaders();

	std::vector<std::unique_ptr<PPCustomShaderInstance>> mShaders;
};

class PPShadowMap
{
public:
	void Update(PPRenderState* renderstate);

private:
	PPShader ShadowMap = { "shaders/pp/shadowmap.fp", "", ShadowMapUniforms::Desc() };
};




/////////////////////////////////////////////////////////////////////////////

class Postprocess
{
public:
	PPBloom bloom;
	PPLensDistort lens;
	PPFXAA fxaa;
	PPCameraExposure exposure;
	PPColormap colormap;
	PPTonemap tonemap;
	PPGrading grading;
	PPAmbientOcclusion ssao;
	PPSceneEffects sceneEffects;
	PPTemporalAA taa;
	PPFinish finish;
	PPPresent present;
	PPShadowMap shadowmap;
	PPCustomShaders customShaders;


	void SetTonemapMode(ETonemapMode tm) { tonemap.SetTonemapMode(tm); }
	// drawWeapon draws the 2D weapon sprites into the current pipeline image:
	// right after TAA with r_weapon_prebloom (so they bloom), otherwise after bloom.
	void Pass1(PPRenderState *state, int fixedcm, int sceneWidth, int sceneHeight, const std::function<void()> &drawWeapon);
	void Pass2(PPRenderState* state, int fixedcm, float flash, int sceneWidth, int sceneHeight);
};


extern Postprocess hw_postprocess;

