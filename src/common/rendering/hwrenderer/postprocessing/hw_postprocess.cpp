/*
**  Postprocessing framework
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
*/

#include "v_video.h"
#include "hw_postprocess.h"
#include "hw_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocess_cvars.h"
#include "hwrenderer/postprocessing/hw_postprocessshader.h"
#include <random>
#include "texturemanager.h"
#include "hw_modernrender.h"

#include "stats.h"
#include "printf.h"
#include <cmath>

Postprocess hw_postprocess;

PPResource *PPResource::First = nullptr;
TArray<PostProcessShader> PostProcessShaders;

bool gpuStatActive = false;
bool keepGpuStatActive = false;
FString gpuStatOutput;

ADD_STAT(gpu)
{
	keepGpuStatActive = true;
	return gpuStatOutput;
}

/////////////////////////////////////////////////////////////////////////////

void PPBloom::UpdateTextures(int width, int height)
{
	if (width == lastWidth && height == lastHeight)
		return;

	int bloomWidth = (width + 1) / 2;
	int bloomHeight = (height + 1) / 2;

	for (int i = 0; i < NumBloomLevels; i++)
	{
		auto &blevel = levels[i];
		blevel.Viewport.left = 0;
		blevel.Viewport.top = 0;
		blevel.Viewport.width = (bloomWidth + 1) / 2;
		blevel.Viewport.height = (bloomHeight + 1) / 2;
		blevel.VTexture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::Rgba16f };
		blevel.HTexture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::Rgba16f };

		bloomWidth = blevel.Viewport.width;
		bloomHeight = blevel.Viewport.height;
	}

	lastWidth = width;
	lastHeight = height;
}

void PPBloom::RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm)
{
	HasModernResult = false;

	// Only bloom things if enabled (gl_bloom or the map's forcebloom) and no special fixed light mode is active
	if (!R_BloomEnabled() || fixedcm != CM_DEFAULT || gl_ssao_debug || sceneWidth <= 0 || sceneHeight <= 0)
	{
		return;
	}

	UpdateTextures(sceneWidth, sceneHeight);

	if (r_bloom_modern)
	{
		RenderModernBloom(renderstate, sceneWidth, sceneHeight);
		return;
	}

	renderstate->PushGroup("bloom");

	ExtractUniforms extractUniforms;
	extractUniforms.Scale = screen->SceneScale();
	extractUniforms.Offset = screen->SceneOffset();

	auto &level0 = levels[0];

	// Extract blooming pixels from scene texture:
	renderstate->Clear();
	renderstate->Shader = &BloomExtract;
	renderstate->Uniforms.Set(extractUniforms);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetInputTexture(1, &hw_postprocess.exposure.CameraTexture);
	renderstate->SetOutputTexture(&level0.VTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	const float blurAmount = gl_bloom_amount;
	BlurUniforms blurUniforms;
	ComputeBlurSamples(7, blurAmount, blurUniforms.SampleWeights);

	// Blur and downscale:
	for (int i = 0; i < NumClassicBloomLevels - 1; i++)
	{
		auto &blevel = levels[i];
		auto &next = levels[i + 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear downscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Blur and upscale:
	for (int i = NumClassicBloomLevels - 1; i > 0; i--)
	{
		auto &blevel = levels[i];
		auto &next = levels[i - 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear upscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	BlurStep(renderstate, blurUniforms, level0.VTexture, level0.HTexture, level0.Viewport, false);
	BlurStep(renderstate, blurUniforms, level0.HTexture, level0.VTexture, level0.Viewport, true);

	// Add bloom back to scene texture:
	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &level0.VTexture, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	renderstate->SetAdditiveBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

void PPBloom::RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight, float gameinfobluramount)
{
	// No scene, no blur!
	if (sceneWidth <= 0 || sceneHeight <= 0)
		return;

	UpdateTextures(sceneWidth, sceneHeight);

	// first, respect the CVar
	float blurAmount = gl_menu_blur;

	// if CVar is negative, use the gameinfo entry
	if (gl_menu_blur < 0)
		blurAmount = gameinfobluramount;

	// if blurAmount == 0 or somehow still returns negative, exit to prevent a crash, clearly we don't want this
	if (blurAmount <= 0.0)
	{
		return;
	}

	renderstate->PushGroup("blur");

	int numLevels = 3;
	assert(numLevels <= NumBloomLevels);

	auto &level0 = levels[0];

	// Grab the area we want to bloom:
	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.VTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	BlurUniforms blurUniforms;
	ComputeBlurSamples(7, blurAmount, blurUniforms.SampleWeights);

	// Blur and downscale:
	for (int i = 0; i < numLevels - 1; i++)
	{
		auto &blevel = levels[i];
		auto &next = levels[i + 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear downscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Blur and upscale:
	for (int i = numLevels - 1; i > 0; i--)
	{
		auto &blevel = levels[i];
		auto &next = levels[i - 1];

		BlurStep(renderstate, blurUniforms, blevel.VTexture, blevel.HTexture, blevel.Viewport, false);
		BlurStep(renderstate, blurUniforms, blevel.HTexture, blevel.VTexture, blevel.Viewport, true);

		// Linear upscale:
		renderstate->Clear();
		renderstate->Shader = &BloomCombine;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	BlurStep(renderstate, blurUniforms, level0.VTexture, level0.HTexture, level0.Viewport, false);
	BlurStep(renderstate, blurUniforms, level0.HTexture, level0.VTexture, level0.Viewport, true);

	// Copy blur back to scene texture:
	renderstate->Clear();
	renderstate->Shader = &BloomCombine;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputTexture(0, &level0.VTexture, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

void PPBloom::BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical)
{
	renderstate->Clear();
	renderstate->Shader = vertical ? &BlurVertical : &BlurHorizontal;
	renderstate->Uniforms.Set(blurUniforms);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, &input);
	renderstate->SetOutputTexture(&output);
	renderstate->SetNoBlend();
	renderstate->Draw();
}

float PPBloom::ComputeBlurGaussian(float n, float theta) // theta = Blur Amount
{
	return (float)((1.0f / sqrtf(2 * (float)M_PI * theta)) * expf(-(n * n) / (2.0f * theta * theta)));
}

void PPBloom::ComputeBlurSamples(int sampleCount, float blurAmount, float *sampleWeights)
{
	sampleWeights[0] = ComputeBlurGaussian(0, blurAmount);

	float totalWeights = sampleWeights[0];

	for (int i = 0; i < sampleCount / 2; i++)
	{
		float weight = ComputeBlurGaussian(i + 1.0f, blurAmount);

		sampleWeights[i * 2 + 1] = weight;
		sampleWeights[i * 2 + 2] = weight;

		totalWeights += weight * 2;
	}

	for (int i = 0; i < sampleCount; i++)
	{
		sampleWeights[i] /= totalWeights;
	}
}

/////////////////////////////////////////////////////////////////////////////

void PPLensDistort::Render(PPRenderState *renderstate)
{
	if (gl_lens == 0)
	{
		return;
	}

	float k[4] =
	{
		gl_lens_k,
		gl_lens_k * gl_lens_chromatic,
		gl_lens_k * gl_lens_chromatic * gl_lens_chromatic,
		0.0f
	};
	float kcube[4] =
	{
		gl_lens_kcube,
		gl_lens_kcube * gl_lens_chromatic,
		gl_lens_kcube * gl_lens_chromatic * gl_lens_chromatic,
		0.0f
	};

	float aspect = screen->mSceneViewport.width / (float)screen->mSceneViewport.height;

	// Scale factor to keep sampling within the input texture
	float r2 = aspect * aspect * 0.25f + 0.25f;
	float sqrt_r2 = sqrt(r2);
	float f0 = 1.0f + max(r2 * (k[0] + kcube[0] * sqrt_r2), 0.0f);
	float f2 = 1.0f + max(r2 * (k[2] + kcube[2] * sqrt_r2), 0.0f);
	float f = max(f0, f2);
	float scale = 1.0f / f;

	LensUniforms uniforms;
	uniforms.AspectRatio = aspect;
	uniforms.Scale = scale;
	uniforms.LensDistortionCoefficient = k;
	uniforms.CubicDistortionValue = kcube;

	renderstate->PushGroup("lens");

	renderstate->Clear();
	renderstate->Shader = &Lens;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPFXAA::Render(PPRenderState *renderstate)
{
	if (0 == gl_fxaa)
	{
		return;
	}

	CreateShaders();

	FXAAUniforms uniforms;
	uniforms.ReciprocalResolution = { 1.0f / screen->mScreenViewport.width, 1.0f / screen->mScreenViewport.height };

	renderstate->PushGroup("fxaa");

	renderstate->Clear();
	renderstate->Shader = &FXAALuma;
	renderstate->Uniforms.Clear();
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->Shader = &FXAA;
	renderstate->Uniforms.Set(uniforms);
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->Draw();

	renderstate->PopGroup();
}

int PPFXAA::GetMaxVersion()
{
	return screen->glslversion >= 4.f ? 400 : 330;
}

void PPFXAA::CreateShaders()
{
	if (LastQuality == gl_fxaa)
		return;

	FXAALuma = { "shaders/pp/fxaa.fp", "#define FXAA_LUMA_PASS\n", {} };
	FXAA = { "shaders/pp/fxaa.fp", GetDefines(), FXAAUniforms::Desc(), GetMaxVersion() };
	LastQuality = gl_fxaa;
}

FString PPFXAA::GetDefines()
{
	int quality;

	switch (gl_fxaa)
	{
	default:
	case IFXAAShader::Low:     quality = 10; break;
	case IFXAAShader::Medium:  quality = 12; break;
	case IFXAAShader::High:    quality = 29; break;
	case IFXAAShader::Extreme: quality = 39; break;
	}

	const int gatherAlpha = GetMaxVersion() >= 400 ? 1 : 0;

	// TODO: enable FXAA_GATHER4_ALPHA on OpenGL earlier than 4.0
	// when GL_ARB_gpu_shader5/GL_NV_gpu_shader5 extensions are supported

	FString result;
	result.Format(
		"#define FXAA_QUALITY__PRESET %i\n"
		"#define FXAA_GATHER4_ALPHA %i\n",
		quality, gatherAlpha);

	return result;
}

/////////////////////////////////////////////////////////////////////////////

void PPCameraExposure::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	// The classic bloom scales its threshold by the camera exposure; eye
	// adaptation is independent of bloom and is applied by the tone mapper.
	bool classic = R_BloomEnabled() && !r_bloom_modern;
	bool adapt = r_autoexposure && sceneWidth > 0 && sceneHeight > 0;
	if (!adapt)
	{
		AdaptValid = false;
		FirstAdaptFrame = true;
	}
	if (!classic && !adapt)
	{
		return;
	}

	renderstate->PushGroup("exposure");

	UpdateTextures(sceneWidth, sceneHeight);
	if (classic) RenderClassic(renderstate);
	if (adapt) RenderAdaptation(renderstate);

	renderstate->PopGroup();
}

float PPCameraExposure::ExposureScale() const
{
	return exp2f(clamp((float)r_exposure + ExposureBias, -8.f, 8.f));
}

bool PPCameraExposure::ExposureActive() const
{
	return AdaptValid || r_exposure != 0.f || ExposureBias != 0.f;
}

PPTexture *PPCameraExposure::AdaptedEV()
{
	if (AdaptValid)
		return &AdaptTexture;
	if (!ZeroTexture.Data)
	{
		std::shared_ptr<void> data(new float[1] { 0.f }, [](void *p) { delete[](float*)p; });
		ZeroTexture = { 1, 1, PixelFormat::R32f, data };
	}
	return &ZeroTexture;
}

void PPCameraExposure::RenderAdaptation(PPRenderState *renderstate)
{
	ExposureExtractUniforms extractUniforms;
	extractUniforms.Scale = screen->SceneScale();
	extractUniforms.Offset = screen->SceneOffset();

	// Log luminance of the scene, averaged down to one texel.
	auto &level0 = ExposureLevels[0];
	renderstate->Clear();
	renderstate->Shader = &ExposureExtractLog;
	renderstate->Uniforms.Set(extractUniforms);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.Texture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	for (size_t i = 0; i + 1 < ExposureLevels.size(); i++)
	{
		auto &blevel = ExposureLevels[i];
		auto &next = ExposureLevels[i + 1];

		renderstate->Shader = &ExposureAverage;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.Texture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.Texture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Time-based adaptation toward the target EV.
	uint64_t now = screen->FrameTime;
	float dt = FirstAdaptFrame ? 0.f : clamp((now - LastAdaptTime) * 0.001f, 0.f, 0.25f);
	ExposureAdaptUniforms adaptUniforms;
	adaptUniforms.Key = max((float)r_autoexposure_key, 0.001f);
	adaptUniforms.MinEV = min((float)r_autoexposure_min, (float)r_autoexposure_max);
	adaptUniforms.MaxEV = max((float)r_autoexposure_min, (float)r_autoexposure_max);
	adaptUniforms.Blend = FirstAdaptFrame ? 1.f : 1.f - expf(-dt * max((float)r_autoexposure_speed, 0.f));

	renderstate->Clear();
	renderstate->Shader = &ExposureAdapt;
	renderstate->Uniforms.Set(adaptUniforms);
	renderstate->Viewport = { 0, 0, 1, 1 };
	renderstate->SetInputTexture(0, &ExposureLevels.back().Texture, PPFilterMode::Nearest);
	renderstate->SetOutputTexture(&AdaptTexture);
	if (FirstAdaptFrame)
		renderstate->SetNoBlend();
	else
		renderstate->SetAlphaBlend();
	renderstate->Draw();

	FirstAdaptFrame = false;
	AdaptValid = true;
	LastAdaptTime = now;
}

void PPCameraExposure::RenderClassic(PPRenderState *renderstate)
{
	ExposureExtractUniforms extractUniforms;
	extractUniforms.Scale = screen->SceneScale();
	extractUniforms.Offset = screen->SceneOffset();

	ExposureCombineUniforms combineUniforms;
	combineUniforms.ExposureBase = gl_exposure_base;
	combineUniforms.ExposureMin = gl_exposure_min;
	combineUniforms.ExposureScale = gl_exposure_scale;
	combineUniforms.ExposureSpeed = gl_exposure_speed;

	auto &level0 = ExposureLevels[0];

	// Extract light blevel from scene texture:
	renderstate->Clear();
	renderstate->Shader = &ExposureExtract;
	renderstate->Uniforms.Set(extractUniforms);
	renderstate->Viewport = level0.Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&level0.Texture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Find the average value:
	for (size_t i = 0; i + 1 < ExposureLevels.size(); i++)
	{
		auto &blevel = ExposureLevels[i];
		auto &next = ExposureLevels[i + 1];

		renderstate->Shader = &ExposureAverage;
		renderstate->Uniforms.Clear();
		renderstate->Viewport = next.Viewport;
		renderstate->SetInputTexture(0, &blevel.Texture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&next.Texture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Combine average value with current camera exposure:
	renderstate->Shader = &ExposureCombine;
	renderstate->Uniforms.Set(combineUniforms);
	renderstate->Viewport.left = 0;
	renderstate->Viewport.top = 0;
	renderstate->Viewport.width = 1;
	renderstate->Viewport.height = 1;
	renderstate->SetInputTexture(0, &ExposureLevels.back().Texture, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&CameraTexture);
	if (!FirstExposureFrame)
		renderstate->SetAlphaBlend();
	else
		renderstate->SetNoBlend();
	renderstate->Draw();

	FirstExposureFrame = false;
}

void PPCameraExposure::UpdateTextures(int width, int height)
{
	int firstwidth = max(width / 2, 1);
	int firstheight = max(height / 2, 1);

	if (ExposureLevels.size() > 0 && ExposureLevels[0].Viewport.width == firstwidth && ExposureLevels[0].Viewport.height == firstheight)
	{
		return;
	}

	ExposureLevels.clear();

	int i = 0;
	do
	{
		width = max(width / 2, 1);
		height = max(height / 2, 1);

		PPExposureLevel blevel;
		blevel.Viewport.left = 0;
		blevel.Viewport.top = 0;
		blevel.Viewport.width = width;
		blevel.Viewport.height = height;
		blevel.Texture = { blevel.Viewport.width, blevel.Viewport.height, PixelFormat::R32f };
		ExposureLevels.push_back(std::move(blevel));

		i++;

	} while (width > 1 || height > 1);

	FirstExposureFrame = true;
	FirstAdaptFrame = true;
}

/////////////////////////////////////////////////////////////////////////////

void PPColormap::Render(PPRenderState *renderstate, int fixedcm, float flash)
{
	ColormapUniforms uniforms;

	if (fixedcm < CM_FIRSTSPECIALCOLORMAP || fixedcm >= CM_MAXCOLORMAP)
	{
		if (flash == 1.f)
			return;

		uniforms.MapStart = { 0,0,0, flash };
		uniforms.MapRange = { 0,0,0, 1.f };
	}
	else
	{
		FSpecialColormap* scm = &SpecialColormaps[fixedcm - CM_FIRSTSPECIALCOLORMAP];

		uniforms.MapStart = { scm->ColorizeStart[0], scm->ColorizeStart[1], scm->ColorizeStart[2], flash };
		uniforms.MapRange = { scm->ColorizeEnd[0] - scm->ColorizeStart[0],
			scm->ColorizeEnd[1] - scm->ColorizeStart[1], scm->ColorizeEnd[2] - scm->ColorizeStart[2], 0.f };
	}

	renderstate->PushGroup("colormap");

	renderstate->Clear();
	renderstate->Shader = &Colormap;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPTonemap::UpdateTextures()
{
	// level.info->tonemap cannot be ETonemapMode::Palette, so it's fine to only check gl_tonemap here
	if (ETonemapMode((int)gl_tonemap) == ETonemapMode::Palette && !PaletteTexture.Data)
	{
		std::shared_ptr<void> data(new uint32_t[512 * 512], [](void *p) { delete[](uint32_t*)p; });

		uint8_t *lut = (uint8_t *)data.get();
		for (int r = 0; r < 64; r++)
		{
			for (int g = 0; g < 64; g++)
			{
				for (int b = 0; b < 64; b++)
				{
					PalEntry color = GPalette.BaseColors[(uint8_t)PTM_BestColor((uint32_t *)GPalette.BaseColors, (r << 2) | (r >> 4), (g << 2) | (g >> 4), (b << 2) | (b >> 4),
						gl_paltonemap_reverselookup, gl_paltonemap_powtable, 0, 256)];
					int index = ((r * 64 + g) * 64 + b) * 4;
					lut[index] = color.r;
					lut[index + 1] = color.g;
					lut[index + 2] = color.b;
					lut[index + 3] = 255;
				}
			}
		}

		PaletteTexture = { 512, 512, PixelFormat::Rgba8, data };
	}
}

bool PPTonemap::IsPalette() const
{
	return level_tonemap == ETonemapMode::None && ETonemapMode((int)gl_tonemap) == ETonemapMode::Palette;
}

static float ACESCurve(float x)
{
	x *= 0.9f;
	return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
}

void PPTonemap::Render(PPRenderState *renderstate)
{
	ETonemapMode current_tonemap = (level_tonemap != ETonemapMode::None) ? level_tonemap : ETonemapMode((int)gl_tonemap);

	// HDR resolve: the modern bloom, exposure and grading are applied here, in
	// linear light, even when no tone mapping curve is selected.
	PPTexture *bloomTexture = hw_postprocess.bloom.ModernResult();
	hw_postprocess.bloom.ClearModernResult();
	auto &grading = hw_postprocess.grading;
	PalEntry shadowTint = (uint32_t)r_grade_shadowtint, highTint = (uint32_t)r_grade_hightint;
	bool tint = (shadowTint.d & 0xffffff) != 0xffffff || (highTint.d & 0xffffff) != 0xffffff;
	bool grade = r_grade_contrast != 1.f || r_grade_saturation != 1.f || tint || grading.Active();
	bool exposure = hw_postprocess.exposure.ExposureActive();

	if (current_tonemap == ETonemapMode::None)
	{
		if (!bloomTexture && !grade && !exposure)
			return;
		current_tonemap = ETonemapMode::Linear;
	}

	UpdateTextures();

	PPShader *shader = nullptr;
	switch (current_tonemap)
	{
	default:
	case ETonemapMode::Linear:		shader = &LinearShader; break;
	case ETonemapMode::Reinhard:		shader = &ReinhardShader; break;
	case ETonemapMode::HejlDawson:	shader = &HejlDawsonShader; break;
	case ETonemapMode::Uncharted2:	shader = &Uncharted2Shader; break;
	case ETonemapMode::Palette:		shader = &PaletteShader; break;
	case ETonemapMode::ACES:		shader = &ACESShader; break;
	}

	renderstate->PushGroup("tonemap");

	renderstate->Clear();
	renderstate->Shader = shader;
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0);
	if (current_tonemap == ETonemapMode::Palette)
	{
		renderstate->SetInputTexture(1, &PaletteTexture);
	}
	else
	{
		if (!BlackTexture.Data)
		{
			std::shared_ptr<void> data(new uint32_t[1] { 0 }, [](void *p) { delete[](uint32_t*)p; });
			BlackTexture = { 1, 1, PixelFormat::Rgba8, data };
		}

		TonemapUniforms u = {};
		u.BloomScale = screen->SceneScale();
		u.BloomOffset = screen->SceneOffset();
		u.ExposureScale = hw_postprocess.exposure.ExposureScale();
		u.AutoExposure = hw_postprocess.exposure.AutoExposureWeight();
		u.BloomIntensity = bloomTexture ? 0.2f * gl_bloom_amount : 0.f;
		u.WhiteScale = 1.f / ACESCurve(clamp((float)r_tonemap_white, 0.25f, 64.f));
		u.Contrast = clamp((float)r_grade_contrast, 0.25f, 4.f);
		u.Saturation = clamp((float)r_grade_saturation, 0.f, 4.f);
		u.TintAmount = tint ? 1.f : 0.f;
		u.ShadowTint = { shadowTint.r / 255.f, shadowTint.g / 255.f, shadowTint.b / 255.f, 1.f };
		u.HighTint = { highTint.r / 255.f, highTint.g / 255.f, highTint.b / 255.f, 1.f };
		float size0 = 0.f, size1 = 0.f;
		PPTexture *lut0 = nullptr, *lut1 = nullptr;
		if (grading.Active())
		{
			lut0 = grading.GetLUT(grading.CurrentLUT(), size0);
			lut1 = grading.GetLUT(grading.TargetLUT(), size1);
		}
		u.LutAmount = lut0 && lut1 ? 1.f : 0.f;
		u.LutBlend = grading.CurrentLUT() != grading.TargetLUT() ? grading.Blend() : 0.f;
		u.LutSize0 = size0;
		u.LutSize1 = size1;
		u.HdrPeak = screen->GetHdrHeadroom();
		renderstate->Uniforms.Set(u);

		renderstate->SetInputTexture(1, bloomTexture ? bloomTexture : &BlackTexture, PPFilterMode::Linear);
		renderstate->SetInputTexture(2, hw_postprocess.exposure.AdaptedEV());
		renderstate->SetInputTexture(3, lut0 ? lut0 : &BlackTexture, PPFilterMode::Linear);
		renderstate->SetInputTexture(4, lut1 ? lut1 : &BlackTexture, PPFilterMode::Linear);
	}
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPGrading::SetLevel(const FString &levelName, const FString &lut, float exposureBias)
{
	LevelLUT = lut;
	LevelEV = exposureBias;
	if (levelName != LevelName)
	{
		// A new level starts with its own grade, without a fade or script overrides.
		LevelName = levelName;
		HasOverrideLUT = false;
		HasOverrideEV = false;
		Current = Target = LevelLUT.IsNotEmpty() ? LevelLUT : FString(r_grading_lut);
		Fade = 0.f;
		EVFrom = EVTo = LevelEV;
		EVFade = 1.f;
	}
}

void PPGrading::SetOverrideLUT(const FString &lut, float fadeSeconds)
{
	OverrideLUT = lut;
	HasOverrideLUT = true;
	LutFadeSeconds = max(fadeSeconds, 0.f);
}

void PPGrading::ClearOverrideLUT(float fadeSeconds)
{
	HasOverrideLUT = false;
	LutFadeSeconds = max(fadeSeconds, 0.f);
}

void PPGrading::StartEVFade(float to, float seconds)
{
	EVFrom = EVFrom + (EVTo - EVFrom) * EVFade;
	EVTo = to;
	EVFade = seconds > 0.f ? 0.f : 1.f;
	EVFadeSeconds = max(seconds, 0.001f);
}

void PPGrading::SetOverrideExposure(float ev, float fadeSeconds)
{
	HasOverrideEV = true;
	StartEVFade(ev, fadeSeconds);
}

void PPGrading::ClearOverrideExposure(float fadeSeconds)
{
	HasOverrideEV = false;
	StartEVFade(LevelEV, fadeSeconds);
}

float PPGrading::Update()
{
	uint64_t now = screen->FrameTime;
	float dt = LastTime != 0 ? clamp((now - LastTime) * 0.001f, 0.f, 0.25f) : 0.f;
	LastTime = now;

	FString desired = HasOverrideLUT ? OverrideLUT : LevelLUT.IsNotEmpty() ? LevelLUT : FString(r_grading_lut);
	if (desired.Compare(Target) != 0)
	{
		// Retargeting mid-fade continues from whichever LUT dominates.
		if (Fade > 0.5f) Current = Target;
		Target = desired;
		Fade = 0.f;
	}
	if (Current.Compare(Target) != 0)
	{
		Fade += LutFadeSeconds > 0.f ? dt / LutFadeSeconds : 1.f;
		if (Fade >= 1.f)
		{
			Current = Target;
			Fade = 0.f;
			LutFadeSeconds = 0.5f;
		}
	}

	if (!HasOverrideEV && EVTo != LevelEV) StartEVFade(LevelEV, 0.5f);
	EVFade = min(EVFade + dt / EVFadeSeconds, 1.f);
	return EVFrom + (EVTo - EVFrom) * EVFade;
}

PPTexture *PPGrading::GetLUT(const FString &name, float &size)
{
	if (name.IsEmpty())
	{
		// Identity 32x32x32 strip, the neutral end of a fade.
		const int n = 32;
		if (!Identity)
		{
			std::shared_ptr<void> data(new uint32_t[n * n * n], [](void *p) { delete[](uint32_t*)p; });
			uint8_t *pixels = (uint8_t *)data.get();
			for (int y = 0; y < n; y++)
			{
				for (int x = 0; x < n * n; x++)
				{
					uint8_t *p = pixels + (y * n * n + x) * 4;
					p[0] = (uint8_t)((x % n) * 255 / (n - 1));
					p[1] = (uint8_t)(y * 255 / (n - 1));
					p[2] = (uint8_t)((x / n) * 255 / (n - 1));
					p[3] = 255;
				}
			}
			Identity = std::make_unique<PPTexture>(n * n, n, PixelFormat::Rgba8, data);
		}
		size = (float)n;
		return Identity.get();
	}

	auto it = Textures.find(name);
	if (it == Textures.end())
	{
		std::unique_ptr<PPTexture> pptex;
		int lutSize = 0;
		auto gtex = TexMan.GetGameTexture(TexMan.CheckForTexture(name.GetChars(), ETextureType::Any, FTextureManager::TEXMAN_TryAny | FTextureManager::TEXMAN_ShortNameOnly), true);
		if (!gtex || !gtex->isValid())
			gtex = TexMan.GetGameTexture(TexMan.CheckForTexture(name.GetChars(), ETextureType::Any, FTextureManager::TEXMAN_TryAny), true);
		if (gtex && gtex->isValid())
		{
			auto buffer = gtex->GetTexture()->CreateTexBuffer(0);
			if (buffer.mHeight > 1 && buffer.mWidth == buffer.mHeight * buffer.mHeight)
			{
				std::shared_ptr<void> data(new uint32_t[buffer.mWidth * buffer.mHeight], [](void *p) { delete[](uint32_t*)p; });
				int count = buffer.mWidth * buffer.mHeight;
				uint8_t *pixels = (uint8_t *)data.get();
				for (int i = 0; i < count; i++)
				{
					int pos = i << 2;
					pixels[pos] = buffer.mBuffer[pos + 2];
					pixels[pos + 1] = buffer.mBuffer[pos + 1];
					pixels[pos + 2] = buffer.mBuffer[pos];
					pixels[pos + 3] = 255;
				}
				pptex = std::make_unique<PPTexture>(buffer.mWidth, buffer.mHeight, PixelFormat::Rgba8, data);
				lutSize = buffer.mHeight;
			}
			else
			{
				Printf("Grading LUT '%s' must be a strip of N slices of NxN (for example 1024x32)\n", name.GetChars());
			}
		}
		else
		{
			Printf("Grading LUT '%s' not found\n", name.GetChars());
		}
		Sizes[name] = lutSize;
		it = Textures.emplace(name, std::move(pptex)).first;
	}
	if (!it->second)
		return GetLUT(FString(), size);
	size = (float)Sizes[name];
	return it->second.get();
}

/////////////////////////////////////////////////////////////////////////////

PPAmbientOcclusion::PPAmbientOcclusion()
{
	// Must match quality enum in PPAmbientOcclusion::DeclareShaders
	double numDirections[NumAmbientRandomTextures] = { 2.0, 4.0, 8.0 };

	std::mt19937 generator(1337);
	std::uniform_real_distribution<double> distribution(0.0, 1.0);
	for (int quality = 0; quality < NumAmbientRandomTextures; quality++)
	{
		std::shared_ptr<void> data(new int16_t[16 * 4], [](void *p) { delete[](int16_t*)p; });
		int16_t *randomValues = (int16_t *)data.get();

		for (int i = 0; i < 16; i++)
		{
			double angle = 2.0 * M_PI * distribution(generator) / numDirections[quality];
			double x = cos(angle);
			double y = sin(angle);
			double z = distribution(generator);
			double w = distribution(generator);

			randomValues[i * 4 + 0] = (int16_t)clamp(x * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 1] = (int16_t)clamp(y * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 2] = (int16_t)clamp(z * 32767.0, -32768.0, 32767.0);
			randomValues[i * 4 + 3] = (int16_t)clamp(w * 32767.0, -32768.0, 32767.0);
		}

		AmbientRandomTexture[quality] = { 4, 4, PixelFormat::Rgba16_snorm, data };
	}
}

void PPAmbientOcclusion::CreateShaders()
{
	if (gl_ssao == LastQuality)
		return;

	// Must match quality values in PPAmbientOcclusion::UpdateTextures
	int numDirections, numSteps;
	switch (gl_ssao)
	{
	default:
	case LowQuality:    numDirections = 2; numSteps = 4; break;
	case MediumQuality: numDirections = 4; numSteps = 4; break;
	case HighQuality:   numDirections = 8; numSteps = 4; break;
	}

	FString defines;
	defines.Format(R"(
		#define USE_RANDOM_TEXTURE
		#define RANDOM_TEXTURE_WIDTH 4.0
		#define NUM_DIRECTIONS %d.0
		#define NUM_STEPS %d.0
	)", numDirections, numSteps);

	LinearDepth = { "shaders/pp/lineardepth.fp", "", LinearDepthUniforms::Desc() };
	LinearDepthMS = { "shaders/pp/lineardepth.fp", "#define MULTISAMPLE\n", LinearDepthUniforms::Desc() };
	AmbientOcclude = { "shaders/pp/ssao.fp", defines, SSAOUniforms::Desc() };
	AmbientOccludeMS = { "shaders/pp/ssao.fp", defines + "\n#define MULTISAMPLE\n", SSAOUniforms::Desc() };
	BlurVertical = { "shaders/pp/depthblur.fp", "#define BLUR_VERTICAL\n", DepthBlurUniforms::Desc() };
	BlurHorizontal = { "shaders/pp/depthblur.fp", "#define BLUR_HORIZONTAL\n", DepthBlurUniforms::Desc() };
	Combine = { "shaders/pp/ssaocombine.fp", "", AmbientCombineUniforms::Desc() };
	CombineMS = { "shaders/pp/ssaocombine.fp", "#define MULTISAMPLE\n", AmbientCombineUniforms::Desc() };

	LastQuality = gl_ssao;
}

void PPAmbientOcclusion::UpdateTextures(int width, int height)
{
	if ((width <= 0 || height <= 0) || (width == LastWidth && height == LastHeight))
		return;

	AmbientWidth = (width + 1) / 2;
	AmbientHeight = (height + 1) / 2;

	LinearDepthTexture = { AmbientWidth, AmbientHeight, PixelFormat::R32f };
	Ambient0 = { AmbientWidth, AmbientHeight, PixelFormat::Rg16f };
	Ambient1 = { AmbientWidth, AmbientHeight, PixelFormat::Rg16f };

	LastWidth = width;
	LastHeight = height;
}

bool PPAmbientOcclusion::GBufferNeeded()
{
	// TAA reads its reactive mask from the normal G-buffer's alpha.
	return gl_ssao != 0 || hw_postprocess.sceneEffects.Active() || (r_taa_reactive && hw_postprocess.taa.Enabled());
}

void PPAmbientOcclusion::Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight)
{
	if (!GBufferNeeded() || sceneWidth == 0 || sceneHeight == 0)
	{
		return;
	}

	if (gl_ssao == 0 && !hw_postprocess.sceneEffects.Active() && !r_softparticles)
	{
		return; // the G-buffer is only there for the TAA reactive mask
	}
	// Soft particles need the linear depth below even without SSAO.

	// The half-resolution buffers follow the 3D viewport, which is smaller
	// than the scene buffers while upscaling, so their cost scales with it.
	sceneWidth = max(screen->mSceneViewport.width, 1);
	sceneHeight = max(screen->mSceneViewport.height, 1);

	CreateShaders();
	UpdateTextures(sceneWidth, sceneHeight);

	float bias = gl_ssao_bias;
	float aoRadius = gl_ssao_radius;
	const float blurAmount = gl_ssao_blur;
	float aoStrength = gl_ssao_strength;

	//float tanHalfFovy = tan(fovy * (M_PI / 360.0f));
	float tanHalfFovy = 1.0f / m5;
	float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
	float invFocalLenY = tanHalfFovy;
	float nDotVBias = clamp(bias, 0.0f, 1.0f);
	float r2 = aoRadius * aoRadius;

	float blurSharpness = 1.0f / blurAmount;

	auto sceneScale = screen->SceneScale();
	auto sceneOffset = screen->SceneOffset();

	int randomTexture = clamp(gl_ssao - 1, 0, NumAmbientRandomTextures - 1);

	LinearDepthUniforms linearUniforms;
	linearUniforms.SampleIndex = 0;
	linearUniforms.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	linearUniforms.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	linearUniforms.InverseDepthRangeA = 1.0f;
	linearUniforms.InverseDepthRangeB = 0.0f;
	linearUniforms.Scale = sceneScale;
	linearUniforms.Offset = sceneOffset;

	SSAOUniforms ssaoUniforms;
	ssaoUniforms.SampleIndex = 0;
	ssaoUniforms.UVToViewA = { 2.0f * invFocalLenX, 2.0f * invFocalLenY };
	ssaoUniforms.UVToViewB = { -invFocalLenX, -invFocalLenY };
	ssaoUniforms.InvFullResolution = { 1.0f / AmbientWidth, 1.0f / AmbientHeight };
	ssaoUniforms.NDotVBias = nDotVBias;
	ssaoUniforms.NegInvR2 = -1.0f / r2;
	ssaoUniforms.RadiusToScreen = aoRadius * 0.5f / tanHalfFovy * AmbientHeight;
	ssaoUniforms.AOMultiplier = 1.0f / (1.0f - nDotVBias);
	ssaoUniforms.AOStrength = aoStrength;
	ssaoUniforms.Scale = sceneScale;
	ssaoUniforms.Offset = sceneOffset;

	DepthBlurUniforms blurUniforms;
	blurUniforms.BlurSharpness = blurSharpness;
	blurUniforms.PowExponent = gl_ssao_exponent;

	AmbientCombineUniforms combineUniforms;
	combineUniforms.SampleCount = gl_multisample;
	combineUniforms.Scale = screen->SceneScale();
	combineUniforms.Offset = screen->SceneOffset();
	combineUniforms.DebugMode = gl_ssao_debug;

	IntRect ambientViewport;
	ambientViewport.left = 0;
	ambientViewport.top = 0;
	ambientViewport.width = AmbientWidth;
	ambientViewport.height = AmbientHeight;

	renderstate->PushGroup("ssao");

	// Calculate linear depth values
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &LinearDepthMS : &LinearDepth;
	renderstate->Uniforms.Set(linearUniforms);
	renderstate->Viewport = ambientViewport;
	renderstate->SetInputSceneDepth(0);
	renderstate->SetInputSceneColor(1);
	renderstate->SetOutputTexture(&LinearDepthTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Indirect light and light scattering read the opaque scene before AO darkens it.
	hw_postprocess.sceneEffects.Render(renderstate, m5, sceneWidth, sceneHeight, &LinearDepthTexture, AmbientWidth, AmbientHeight);

	if (gl_ssao == 0)
	{
		renderstate->PopGroup();
		return;
	}

	// Apply ambient occlusion
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &AmbientOccludeMS : &AmbientOcclude;
	renderstate->Uniforms.Set(ssaoUniforms);
	renderstate->Viewport = ambientViewport;
	renderstate->SetInputTexture(0, &LinearDepthTexture);
	renderstate->SetInputSceneNormal(1);
	renderstate->SetInputTexture(2, &AmbientRandomTexture[randomTexture], PPFilterMode::Nearest, PPWrapMode::Repeat);
	renderstate->SetOutputTexture(&Ambient0);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Blur SSAO texture
	if (gl_ssao_debug < 2)
	{
		renderstate->Clear();
		renderstate->Shader = &BlurHorizontal;
		renderstate->Uniforms.Set(blurUniforms);
		renderstate->Viewport = ambientViewport;
		renderstate->SetInputTexture(0, &Ambient0);
		renderstate->SetOutputTexture(&Ambient1);
		renderstate->SetNoBlend();
		renderstate->Draw();

		renderstate->Clear();
		renderstate->Shader = &BlurVertical;
		renderstate->Uniforms.Set(blurUniforms);
		renderstate->Viewport = ambientViewport;
		renderstate->SetInputTexture(0, &Ambient1);
		renderstate->SetOutputTexture(&Ambient0);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// Add SSAO back to scene texture:
	renderstate->Clear();
	renderstate->Shader = gl_multisample > 1 ? &CombineMS : &Combine;
	renderstate->Uniforms.Set(combineUniforms);
	renderstate->Viewport = screen->mSceneViewport;
	if (gl_ssao_debug < 4)
		renderstate->SetInputTexture(0, &Ambient0, PPFilterMode::Linear);
	else
		renderstate->SetInputSceneNormal(0, PPFilterMode::Linear);
	renderstate->SetInputSceneFog(1);
	renderstate->SetOutputSceneColor();
	if (gl_ssao_debug != 0)
		renderstate->SetNoBlend();
	else
		renderstate->SetAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

PPPresent::PPPresent()
{
	static const float data[64] =
	{
			.0078125, .2578125, .1328125, .3828125, .0234375, .2734375, .1484375, .3984375,
			.7578125, .5078125, .8828125, .6328125, .7734375, .5234375, .8984375, .6484375,
			.0703125, .3203125, .1953125, .4453125, .0859375, .3359375, .2109375, .4609375,
			.8203125, .5703125, .9453125, .6953125, .8359375, .5859375, .9609375, .7109375,
			.0390625, .2890625, .1640625, .4140625, .0546875, .3046875, .1796875, .4296875,
			.7890625, .5390625, .9140625, .6640625, .8046875, .5546875, .9296875, .6796875,
			.1015625, .3515625, .2265625, .4765625, .1171875, .3671875, .2421875, .4921875,
			.8515625, .6015625, .9765625, .7265625, .8671875, .6171875, .9921875, .7421875,
	};

	std::shared_ptr<void> pixels(new float[64], [](void *p) { delete[](float*)p; });
	memcpy(pixels.get(), data, 64 * sizeof(float));
	Dither = { 8, 8, PixelFormat::R32f, pixels };
}

/////////////////////////////////////////////////////////////////////////////


void PPShadowMap::Update(PPRenderState* renderstate)
{
	ShadowMapUniforms uniforms;
	uniforms.ShadowmapQuality = (float)gl_shadowmap_quality;
	uniforms.NodesCount = screen->mShadowMap.NodesCount();

	renderstate->PushGroup("shadowmap");

	renderstate->Clear();
	renderstate->Shader = &ShadowMap;
	renderstate->Uniforms.Set(uniforms);
	renderstate->Viewport = { 0, 0, gl_shadowmap_quality, 1024 };
	renderstate->SetShadowMapBuffers(true);
	renderstate->SetOutputShadowMap();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

CVAR(Bool, gl_custompost, true, 0)

void PPCustomShaders::Run(PPRenderState *renderstate, FString target)
{
	if (!gl_custompost)
		return;

	CreateShaders();

	for (auto &shader : mShaders)
	{
		if (shader->Desc->Target == target && shader->Desc->Enabled)
		{
			shader->Run(renderstate);
		}
	}
}

void PPCustomShaders::CreateShaders()
{
	if (mShaders.size() == PostProcessShaders.Size())
		return;

	mShaders.clear();

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		mShaders.push_back(std::make_unique<PPCustomShaderInstance>(&PostProcessShaders[i]));
	}
}

/////////////////////////////////////////////////////////////////////////////

PPCustomShaderInstance::PPCustomShaderInstance(PostProcessShader *desc) : Desc(desc)
{
	// Build an uniform block to be used as input
	TMap<FString, PostProcessUniformValue>::Iterator it(Desc->Uniforms);
	TMap<FString, PostProcessUniformValue>::Pair *pair;
	size_t offset = 0;
	while (it.NextPair(pair))
	{
		FString type;
		FString name = pair->Key;

		switch (pair->Value.Type)
		{
		case PostProcessUniformType::Float: AddUniformField(offset, name, UniformType::Float, sizeof(float)); break;
		case PostProcessUniformType::Int: AddUniformField(offset, name, UniformType::Int, sizeof(int)); break;
		case PostProcessUniformType::Vec2: AddUniformField(offset, name, UniformType::Vec2, sizeof(float) * 2); break;
		case PostProcessUniformType::Vec3: AddUniformField(offset, name, UniformType::Vec3, sizeof(float) * 3, sizeof(float) * 4); break;
		case PostProcessUniformType::Vec4: AddUniformField(offset, name, UniformType::Vec4, sizeof(float) * 4); break;
		default: break;
		}
	}
	UniformStructSize = ((int)offset + 15) / 16 * 16;

	// Build the input textures
	FString uniformTextures;
	uniformTextures += "layout(binding=0) uniform sampler2D InputTexture;\n";

	TMap<FString, FString>::Iterator itTextures(Desc->Textures);
	TMap<FString, FString>::Pair *pairTextures;
	int binding = 1;
	while (itTextures.NextPair(pairTextures))
	{
		uniformTextures.AppendFormat("layout(binding=%d) uniform sampler2D %s;\n", binding++, pairTextures->Key.GetChars());
	}

	// Setup pipeline
	FString pipelineInOut;
	if (screen->IsVulkan())
	{
		pipelineInOut += "layout(location=0) in vec2 TexCoord;\n";
		pipelineInOut += "layout(location=0) out vec4 FragColor;\n";
	}
	else 
	{
		pipelineInOut += "in vec2 TexCoord;\n";
		pipelineInOut += "out vec4 FragColor;\n";
	}

	FString prolog;
	prolog += uniformTextures;
	prolog += pipelineInOut;

	Shader = PPShader(Desc->ShaderLumpName, prolog, Fields);
}

void PPCustomShaderInstance::Run(PPRenderState *renderstate)
{
	renderstate->PushGroup(Desc->Name);

	renderstate->Clear();
	renderstate->Shader = &Shader;
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetNoBlend();
	renderstate->SetOutputNext();
	//renderstate->SetDebugName(Desc->ShaderLumpName.GetChars());

	SetTextures(renderstate);
	SetUniforms(renderstate);

	renderstate->Draw();

	renderstate->PopGroup();
}

void PPCustomShaderInstance::SetTextures(PPRenderState *renderstate)
{
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);

	int textureIndex = 1;
	TMap<FString, FString>::Iterator it(Desc->Textures);
	TMap<FString, FString>::Pair *pair;
	while (it.NextPair(pair))
	{
		FString name = pair->Value;
		auto gtex = TexMan.GetGameTexture(TexMan.CheckForTexture(name.GetChars(), ETextureType::Any), true);
		if (gtex && gtex->isValid())
		{
			// Why does this completely circumvent the normal way of handling textures?
			// This absolutely needs fixing because it will also circumvent any potential caching system that may get implemented.
			//
			// To do: fix the above problem by adding PPRenderState::SetInput(FTexture *tex)

			auto tex = gtex->GetTexture();
			auto &pptex = Textures[tex];
			if (!pptex)
			{
				auto buffer = tex->CreateTexBuffer(0);

				std::shared_ptr<void> data(new uint32_t[buffer.mWidth * buffer.mHeight], [](void *p) { delete[](uint32_t*)p; });

				int count = buffer.mWidth * buffer.mHeight;
				uint8_t *pixels = (uint8_t *)data.get();
				for (int i = 0; i < count; i++)
				{
					int pos = i << 2;
					pixels[pos] = buffer.mBuffer[pos + 2];
					pixels[pos + 1] = buffer.mBuffer[pos + 1];
					pixels[pos + 2] = buffer.mBuffer[pos];
					pixels[pos + 3] = buffer.mBuffer[pos + 3];
				}

				pptex = std::make_unique<PPTexture>(buffer.mWidth, buffer.mHeight, PixelFormat::Rgba8, data);
			}

			renderstate->SetInputTexture(textureIndex, pptex.get(), PPFilterMode::Linear, PPWrapMode::Repeat);
			textureIndex++;
		}
	}
}

void PPCustomShaderInstance::SetUniforms(PPRenderState *renderstate)
{
	TArray<uint8_t> uniforms;
	uniforms.Resize(UniformStructSize);

	TMap<FString, PostProcessUniformValue>::Iterator it(Desc->Uniforms);
	TMap<FString, PostProcessUniformValue>::Pair *pair;
	while (it.NextPair(pair))
	{
		auto it2 = FieldOffset.find(pair->Key);
		if (it2 != FieldOffset.end())
		{
			uint8_t *dst = &uniforms[it2->second];
			float fValues[4];
			int iValues[4];
			switch (pair->Value.Type)
			{
			case PostProcessUniformType::Float:
				fValues[0] = (float)pair->Value.Values[0];
				memcpy(dst, fValues, sizeof(float));
				break;
			case PostProcessUniformType::Int:
				iValues[0] = (int)pair->Value.Values[0];
				memcpy(dst, iValues, sizeof(int));
				break;
			case PostProcessUniformType::Vec2:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				memcpy(dst, fValues, sizeof(float) * 2);
				break;
			case PostProcessUniformType::Vec3:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				fValues[2] = (float)pair->Value.Values[2];
				memcpy(dst, fValues, sizeof(float) * 3);
				break;
			case PostProcessUniformType::Vec4:
				fValues[0] = (float)pair->Value.Values[0];
				fValues[1] = (float)pair->Value.Values[1];
				fValues[2] = (float)pair->Value.Values[2];
				fValues[3] = (float)pair->Value.Values[3];
				memcpy(dst, fValues, sizeof(float) * 4);
				break;
			default:
				break;
			}
		}
	}

	renderstate->Uniforms.Data = uniforms;
}

void PPCustomShaderInstance::AddUniformField(size_t &offset, const FString &name, UniformType type, size_t fieldsize, size_t alignment)
{
	if (alignment == 0) alignment = fieldsize;
	offset = (offset + alignment - 1) / alignment * alignment;

	FieldOffset[name] = offset;

	auto name2 = std::make_unique<FString>(name);
	auto chars = name2->GetChars();
	FieldNames.push_back(std::move(name2));
	Fields.push_back({ chars, type, offset });
	offset += fieldsize;

	if (fieldsize != alignment) // Workaround for buggy OpenGL drivers that does not do std140 layout correctly for vec3
	{
		name2 = std::make_unique<FString>(name + "_F39350FF12DE_padding");
		chars = name2->GetChars();
		FieldNames.push_back(std::move(name2));
		Fields.push_back({ chars, UniformType::Float, offset });
		offset += alignment - fieldsize;
	}
}


void Postprocess::Pass1(PPRenderState* state, int fixedcm, int sceneWidth, int sceneHeight, const std::function<void()> &drawWeapon)
{
	exposure.SetExposureBias(grading.Update());
	taa.Render(state, sceneWidth, sceneHeight);
	// The weapon is drawn after TAA (so it doesn't smear) but before exposure
	// and bloom, so its emissive parts bloom and it is graded like the scene.
	bool weaponFirst = r_weapon_prebloom;
	if (weaponFirst && drawWeapon) drawWeapon();
	exposure.Render(state, sceneWidth, sceneHeight);
	customShaders.Run(state, "beforebloom");
	bloom.RenderBloom(state, sceneWidth, sceneHeight, fixedcm);
	if (!weaponFirst && drawWeapon) drawWeapon();
}

void Postprocess::Pass2(PPRenderState* state, int fixedcm, float flash, int sceneWidth, int sceneHeight)
{
	tonemap.Render(state);
	colormap.Render(state, fixedcm, flash);
	lens.Render(state);
	fxaa.Render(state);
	finish.Render(state);
	customShaders.Run(state, "scene");
}

/////////////////////////////////////////////////////////////////////////////
//
// Progressive bloom: a 13-tap downsample chain with a soft-knee threshold on
// the first level, then a tent-filtered upsample that accumulates each level
// into the next larger one. Fewer passes and less bandwidth than the separable
// Gaussian chain, and without its flicker on small bright pixels.
//
/////////////////////////////////////////////////////////////////////////////

void PPBloom::RenderModernBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	renderstate->PushGroup("bloom");

	BloomDownUniforms down;
	down.Scale = screen->SceneScale();
	down.Offset = screen->SceneOffset();
	down.InvSize = { 1.0f / sceneWidth, 1.0f / sceneHeight };
	down.Threshold = max((float)r_bloom_threshold, 0.f);
	down.Knee = max((float)r_bloom_knee, 0.01f);
	down.ExposureScale = hw_postprocess.exposure.ExposureScale();
	down.AutoExposure = hw_postprocess.exposure.AutoExposureWeight();
	down.Padding0 = down.Padding1 = 0.0f;

	renderstate->Clear();
	renderstate->Shader = &BloomPrefilter;
	renderstate->Uniforms.Set(down);
	renderstate->Viewport = levels[0].Viewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Linear);
	renderstate->SetInputTexture(1, hw_postprocess.exposure.AdaptedEV());
	renderstate->SetOutputTexture(&levels[0].VTexture);
	renderstate->SetNoBlend();
	renderstate->Draw();

	for (int i = 1; i < NumBloomLevels; i++)
	{
		auto &prev = levels[i - 1];
		down.Scale = { 1.0f, 1.0f };
		down.Offset = { 0.0f, 0.0f };
		down.InvSize = { 1.0f / prev.Viewport.width, 1.0f / prev.Viewport.height };
		renderstate->Clear();
		renderstate->Shader = &BloomDown;
		renderstate->Uniforms.Set(down);
		renderstate->Viewport = levels[i].Viewport;
		renderstate->SetInputTexture(0, &prev.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&levels[i].VTexture);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	BloomUpUniforms up;
	up.Padding = 0.0f;
	for (int i = NumBloomLevels - 1; i > 0; i--)
	{
		auto &src = levels[i];
		up.InvSize = { 1.0f / src.Viewport.width, 1.0f / src.Viewport.height };
		up.Intensity = 1.0f;
		renderstate->Clear();
		renderstate->Shader = &BloomUp;
		renderstate->Uniforms.Set(up);
		renderstate->Viewport = levels[i - 1].Viewport;
		renderstate->SetInputTexture(0, &src.VTexture, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&levels[i - 1].VTexture);
		renderstate->SetAdditiveBlend();
		renderstate->Draw();
	}

	// The tone mapper adds the bloom in linear light. The palette tone mapper
	// can't, so there it is added to the gamma-encoded scene as before.
	if (!hw_postprocess.tonemap.IsPalette())
	{
		HasModernResult = true;
		renderstate->PopGroup();
		return;
	}

	up.InvSize = { 1.0f / levels[0].Viewport.width, 1.0f / levels[0].Viewport.height };
	up.Intensity = 0.12f * gl_bloom_amount;
	renderstate->Clear();
	renderstate->Shader = &BloomUp;
	renderstate->Uniforms.Set(up);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &levels[0].VTexture, PPFilterMode::Linear);
	renderstate->SetOutputCurrent();
	renderstate->SetAdditiveBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

bool PPSceneEffects::Active() const
{
	return r_ssr > 0 || r_ssgi > 0 || VolumetricActive();
}

bool PPSceneEffects::VolumetricActive() const
{
	return r_volumetric > 0 && (VolumetricLightCount > 0 || Fog.Density > 0.0f);
}

void PPSceneEffects::SetVolumetricLights(const FVector4 *lights, int count)
{
	count = clamp(count, 0, (int)MaxVolumetricLights);
	VolumetricLightCount = count;
	for (int i = 0; i < count * VolumetricLightVec4s; i++)
		VolumetricLights[i] = lights[i];
}

void PPSceneEffects::SetVolumetricView(const float *viewMatrix)
{
	VSMatrix view, inverse;
	view.loadMatrix(viewMatrix);
	if (!view.inverseMatrix(inverse))
		return;
	const FLOATTYPE *m = inverse.get();
	// The pass works with view space z pointing forward, hence the negated z axis.
	ViewToWorld[0] = { (float)m[0], (float)m[1], (float)m[2], (float)m[12] };
	ViewToWorld[1] = { (float)m[4], (float)m[5], (float)m[6], (float)m[13] };
	ViewToWorld[2] = { -(float)m[8], -(float)m[9], -(float)m[10], (float)m[14] };
}

void PPSceneEffects::UpdateTextures(int width, int height)
{
	if (width == Width && height == Height)
		return;
	Width = width;
	Height = height;
	Indirect = { width, height, PixelFormat::Rgba16f };
}

void PPSceneEffects::CreateScatterShaders()
{
	if (ScatterShadersCreated)
		return;
	static const char *defines[NumScatterVariants] = { "", "#define FULL_RESOLUTION\n", "#define FULL_RESOLUTION\n#define MULTISAMPLE\n" };
	for (int i = 0; i < NumScatterVariants; i++)
	{
		Scatter[i] = { "shaders/pp/volumetric.fp", defines[i], VolumetricUniforms::Desc() };
		Scatter[NumScatterVariants + i] = { "shaders/pp/volumetric.fp", FString(defines[i]) + "#define USE_RAYTRACE\n", VolumetricUniforms::Desc() };
		Scatter[NumScatterVariants + i].RayQuery = true;
	}
	ScatterShadersCreated = true;
}

void PPSceneEffects::RenderVolumetrics(PPRenderState *renderstate, const FVector2 &uvToViewA, const FVector2 &uvToViewB, PPTexture *linearDepth)
{
	// The light list is a storage buffer, which the OpenGL backends do not bind.
	if (!renderstate->SupportsStorageData())
		return;

	CreateScatterShaders();

	// r_volumetric 2 marches every pixel of the 3D viewport, 1 every fourth.
	// Either way the buffer follows the 3D viewport, which is smaller than
	// the scene buffers while upscaling.
	bool full = r_volumetric > 1;
	const IntRect &sceneViewport = screen->mSceneViewport;
	int width = full ? sceneViewport.width : (sceneViewport.width + 1) / 2;
	int height = full ? sceneViewport.height : (sceneViewport.height + 1) / 2;
	if (width <= 0 || height <= 0)
		return;
	if (width != VolumetricWidth || height != VolumetricHeight)
	{
		VolumetricWidth = width;
		VolumetricHeight = height;
		Volumetrics = { width, height, PixelFormat::Rgba16f };
		VolumetricsBlur = { width, height, PixelFormat::Rgba16f };
	}

	LastRayQuery = renderstate->SupportsRayQuery();
	bool rayQuery = false;
	for (int i = 0; i < VolumetricLightCount && LastRayQuery; i++)
		rayQuery |= VolumetricLights[i * VolumetricLightVec4s + 3].W > 0.0f;

	VolumetricUniforms u = {};
	u.ViewX = ViewToWorld[0];
	u.ViewY = ViewToWorld[1];
	u.ViewZ = ViewToWorld[2];
	u.UVToViewA = uvToViewA;
	u.UVToViewB = uvToViewB;
	u.Scale = screen->SceneScale();
	u.Offset = screen->SceneOffset();
	u.FogColor = { Fog.Color.X, Fog.Color.Y, Fog.Color.Z, clamp(Fog.Noise, 0.0f, 1.0f) };
	u.FogParams = { Fog.Base, 1.0f / max(Fog.Falloff, 1.0f), max(Fog.Density, 0.0f), max(Fog.SectorDensity, 0.0f) };
	u.FogNoise = { Fog.NoiseOffset.X, Fog.NoiseOffset.Y, Fog.NoiseOffset.Z, Fog.NoiseScale };
	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	u.InverseDepthRangeA = 1.0f;
	u.InverseDepthRangeB = 0.0f;
	u.Strength = r_volumetric_strength * max(Fog.Strength, 0.0f);
	u.FogScatter = max((float)r_volumetric_fogscatter, 0.0f);
	u.RainScatter = max(Fog.RainScatter, 0.0f);
	u.MaxDistance = 16384.0f;
	// A new sample offset every frame for TAA to integrate; fixed without it.
	u.JitterOffset = r_taa ? (FrameIndex++ & 63) * 0.618034f : 0.0f;
	u.LightCount = VolumetricLightCount;
	u.Samples = clamp((int)r_volumetric_samples, 2, 64);
	if (full) u.Samples = max(u.Samples * 2 / 3, 2);
	u.SampleIndex = 0;

	int variant = !full ? ScatterHalf : gl_multisample > 1 ? ScatterFullMS : ScatterFull;
	if (rayQuery) variant += NumScatterVariants;

	IntRect viewport;
	viewport.left = 0;
	viewport.top = 0;
	viewport.width = width;
	viewport.height = height;

	renderstate->Clear();
	renderstate->Shader = &Scatter[variant];
	renderstate->Uniforms.Set(u);
	renderstate->Viewport = viewport;
	if (full)
	{
		renderstate->SetInputSceneDepth(0);
		renderstate->SetInputSceneColor(2);
	}
	else
	{
		renderstate->SetInputTexture(0, linearDepth, PPFilterMode::Nearest);
	}
	renderstate->SetInputSpecialType(1, PPTextureType::ShadowMap, PPFilterMode::Nearest);
	// The buffer is never empty: the shader declares at least one light.
	renderstate->SetStorageData(VolumetricLights, max(VolumetricLightCount, 1) * VolumetricLightVec4s * (int)sizeof(FVector4));
	renderstate->SetRayQuery(rayQuery);
	renderstate->SetOutputTexture(&Volumetrics);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Depth-aware blur hides the per-pixel sample jitter (and, at half
	// resolution, softens the upsample).
	VolumetricBlurUniforms blur;
	blur.InvSize = { 1.0f / width, 1.0f / height };
	blur.DepthSharpness = 8.0f;
	blur.Padding = 0.0f;
	renderstate->Clear();
	renderstate->Shader = &ScatterBlur;
	renderstate->Uniforms.Set(blur);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, &Volumetrics, PPFilterMode::Nearest);
	renderstate->SetInputTexture(1, linearDepth, PPFilterMode::Nearest);
	renderstate->SetOutputTexture(&VolumetricsBlur);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Premultiplied: in-scattered light plus height fog over the scene.
	CompositeUniforms composite;
	composite.Scale = { 1.0f, 1.0f };
	composite.Offset = { 0.0f, 0.0f };
	renderstate->Clear();
	renderstate->Shader = &Composite;
	renderstate->Uniforms.Set(composite);
	renderstate->Viewport = sceneViewport;
	renderstate->SetInputTexture(0, &VolumetricsBlur, PPFilterMode::Linear);
	renderstate->SetOutputSceneColor();
	renderstate->BlendMode.BlendOp = STYLEOP_Add;
	renderstate->BlendMode.SrcAlpha = STYLEALPHA_One;
	renderstate->BlendMode.DestAlpha = STYLEALPHA_InvSrc;
	renderstate->BlendMode.Flags = 0;
	renderstate->Draw();
}

void PPSceneEffects::Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight, PPTexture *linearDepth, int depthWidth, int depthHeight)
{
	bool volumetrics = VolumetricActive();
	bool indirect = r_ssgi > 0;
	if (!volumetrics && !indirect)
		return;

	// Effects render at the SSAO resolution (half size) and are upscaled when composited.
	UpdateTextures(depthWidth, depthHeight);

	float tanHalfFovy = 1.0f / m5;
	float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
	float invFocalLenY = tanHalfFovy;
	FVector2 uvToViewA = { 2.0f * invFocalLenX, 2.0f * invFocalLenY };
	FVector2 uvToViewB = { -invFocalLenX, -invFocalLenY };

	IntRect viewport;
	viewport.left = 0;
	viewport.top = 0;
	viewport.width = depthWidth;
	viewport.height = depthHeight;

	CompositeUniforms composite;
	composite.Scale = { 1.0f, 1.0f };
	composite.Offset = { 0.0f, 0.0f };

	renderstate->PushGroup("sceneeffects");

	if (indirect)
	{
		ReflectionUniforms u;
		u.UVToViewA = uvToViewA;
		u.UVToViewB = uvToViewB;
		u.Scale = screen->SceneScale();
		u.Offset = screen->SceneOffset();
		u.InvSize = { 1.0f / depthWidth, 1.0f / depthHeight };
		u.Strength = r_ssgi_strength;
		u.MaxDistance = 192.0f;
		u.Thickness = 32.0f;
		u.Steps = 8;
		u.SampleIndex = 0;
		u.Padding = 0.0f;

		renderstate->Clear();
		renderstate->Shader = gl_multisample > 1 ? &GatherMS : &Gather;
		renderstate->Uniforms.Set(u);
		renderstate->Viewport = viewport;
		renderstate->SetInputTexture(0, linearDepth, PPFilterMode::Nearest);
		renderstate->SetInputSceneNormal(1);
		renderstate->SetInputSceneFog(2);
		renderstate->SetInputSceneColor(3);
		renderstate->SetOutputTexture(&Indirect);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	if (indirect)
	{
		renderstate->Clear();
		renderstate->Shader = &Composite;
		renderstate->Uniforms.Set(composite);
		renderstate->Viewport = screen->mSceneViewport;
		renderstate->SetInputTexture(0, &Indirect, PPFilterMode::Linear);
		renderstate->SetOutputSceneColor();
		renderstate->SetAdditiveBlend();
		renderstate->Draw();
	}

	if (volumetrics)
		RenderVolumetrics(renderstate, uvToViewA, uvToViewB, linearDepth);

	renderstate->PopGroup();
}

void PPSceneEffects::UpdateReflectionTextures(int width, int height)
{
	if (width == ReflectWidth && height == ReflectHeight)
		return;
	ReflectWidth = width;
	ReflectHeight = height;
	TraceDepth = { width, height, PixelFormat::R32f };
	Reflections = { width, height, PixelFormat::Rgba16f };
	for (int i = 0; i < NumReflectionLevels; i++)
		ReflectLevels[i] = { max(width >> i, 1), max(height >> i, 1), PixelFormat::Rgba16f };
}

void PPSceneEffects::RenderReflections(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight)
{
	if (r_ssr <= 0 || sceneWidth <= 0 || sceneHeight <= 0)
		return;

	// Half the size of the 3D viewport (smaller than the scene buffers while upscaling).
	int viewWidth = max(screen->mSceneViewport.width, 1);
	int viewHeight = max(screen->mSceneViewport.height, 1);
	int width = (viewWidth + 1) / 2;
	int height = (viewHeight + 1) / 2;
	UpdateReflectionTextures(width, height);

	float tanHalfFovy = 1.0f / m5;
	float invFocalLenX = tanHalfFovy * (viewWidth / (float)viewHeight);
	float invFocalLenY = tanHalfFovy;
	bool multisample = gl_multisample > 1;
	auto sceneScale = screen->SceneScale();
	auto sceneOffset = screen->SceneOffset();

	IntRect viewport = { 0, 0, width, height };

	renderstate->PushGroup("reflections");

	// Linear depth of the finished scene, including the sprites that write depth.
	LinearDepthUniforms linearUniforms = {};
	linearUniforms.SampleIndex = 0;
	linearUniforms.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	linearUniforms.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	linearUniforms.InverseDepthRangeA = 1.0f;
	linearUniforms.InverseDepthRangeB = 0.0f;
	linearUniforms.Scale = sceneScale;
	linearUniforms.Offset = sceneOffset;

	renderstate->Clear();
	renderstate->Shader = multisample ? &TraceLinearDepthMS : &TraceLinearDepth;
	renderstate->Uniforms.Set(linearUniforms);
	renderstate->Viewport = viewport;
	renderstate->SetInputSceneDepth(0);
	renderstate->SetInputSceneColor(1);
	renderstate->SetOutputTexture(&TraceDepth);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Blur chain of the scene colour; rough surfaces sample coarser levels.
	SSRDownUniforms down = {};
	down.Scale = sceneScale;
	down.Offset = sceneOffset;
	down.InvSize = { 1.0f / screen->mScreenViewport.width, 1.0f / screen->mScreenViewport.height };
	down.SampleIndex = 0;
	renderstate->Clear();
	renderstate->Shader = multisample ? &ReflectDownSceneMS : &ReflectDownScene;
	renderstate->Uniforms.Set(down);
	renderstate->Viewport = viewport;
	renderstate->SetInputSceneColor(0, PPFilterMode::Linear);
	renderstate->SetOutputTexture(&ReflectLevels[0]);
	renderstate->SetNoBlend();
	renderstate->Draw();

	for (int i = 1; i < NumReflectionLevels; i++)
	{
		auto &src = ReflectLevels[i - 1];
		auto &dst = ReflectLevels[i];
		down.Scale = { 1.0f, 1.0f };
		down.Offset = { 0.0f, 0.0f };
		down.InvSize = { 1.0f / src.Width, 1.0f / src.Height };
		renderstate->Clear();
		renderstate->Shader = &ReflectDown;
		renderstate->Uniforms.Set(down);
		renderstate->Viewport = { 0, 0, dst.Width, dst.Height };
		renderstate->SetInputTexture(0, &src, PPFilterMode::Linear);
		renderstate->SetOutputTexture(&dst);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// While TAA accumulates, the ray jitter changes every frame so rough
	// reflections converge instead of showing a fixed noise pattern.
	ReflectFrame = ReflectFrame % 64 + 1;

	SSRUniforms u = {};
	u.UVToViewA = { 2.0f * invFocalLenX, 2.0f * invFocalLenY };
	u.UVToViewB = { -invFocalLenX, -invFocalLenY };
	u.Scale = sceneScale;
	u.Offset = sceneOffset;
	u.InvSize = { 1.0f / width, 1.0f / height };
	u.Strength = r_ssr_strength;
	u.MaxDistance = r_ssr > 1 ? 1536.0f : 1024.0f;
	u.Thickness = 24.0f;
	u.Steps = r_ssr > 1 ? 40 : 24;
	u.SampleIndex = 0;
	u.Stretch = clamp(R_SSRStretch(), 0.0f, 8.0f);	// r_ssr_stretch or the map's ssrstretch
	u.Jitter = hw_postprocess.taa.Enabled() ? (float)ReflectFrame : 0.0f;
	u.FarDepth = screen->GetZFar();
	u.LevelHeight = (float)height;

	renderstate->Clear();
	renderstate->Shader = multisample ? &ReflectMS : &Reflect;
	renderstate->Uniforms.Set(u);
	renderstate->Viewport = viewport;
	renderstate->SetInputTexture(0, &TraceDepth, PPFilterMode::Nearest);
	renderstate->SetInputSceneNormal(1);
	renderstate->SetInputSceneFog(2);
	renderstate->SetInputSceneColor(3, PPFilterMode::Linear);
	for (int i = 0; i < NumReflectionLevels; i++)
		renderstate->SetInputTexture(4 + i, &ReflectLevels[i], PPFilterMode::Linear);
	renderstate->SetOutputTexture(&Reflections);
	renderstate->SetNoBlend();
	renderstate->Draw();

	// Full-resolution composite: the reflectivity in the G-buffer gives crisp
	// puddle edges, and translucent surfaces drawn on top are left alone.
	SSRCompositeUniforms composite = {};
	composite.Scale = sceneScale;
	composite.Offset = sceneOffset;
	composite.SampleIndex = 0;
	composite.Strength = r_ssr_strength;
	renderstate->Clear();
	renderstate->Shader = multisample ? &ReflectCompositeMS : &ReflectComposite;
	renderstate->Uniforms.Set(composite);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &Reflections, PPFilterMode::Linear);
	renderstate->SetInputSceneFog(1);
	renderstate->SetOutputSceneColor();
	renderstate->SetAlphaBlend();
	renderstate->Draw();

	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

void PPFinish::Render(PPRenderState *renderstate)
{
	if (r_sharpen <= 0.0f && r_vignette <= 0.0f && r_grain <= 0.0f)
		return;

	FinishUniforms u;
	u.InvSize = { 1.0f / screen->mScreenViewport.width, 1.0f / screen->mScreenViewport.height };
	u.Sharpen = clamp((float)r_sharpen, 0.0f, 1.0f);
	u.Vignette = clamp((float)r_vignette, 0.0f, 1.0f);
	u.Grain = clamp((float)r_grain, 0.0f, 1.0f);
	u.Time = (float)((screen->FrameTime % 100000) * 0.001);
	u.Aspect = screen->mScreenViewport.width / (float)max(screen->mScreenViewport.height, 1);
	u.MaxValue = sqrtf(screen->GetHdrHeadroom());

	renderstate->PushGroup("finish");
	renderstate->Clear();
	renderstate->Shader = &Finish;
	renderstate->Uniforms.Set(u);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();
	renderstate->PopGroup();
}

/////////////////////////////////////////////////////////////////////////////

static float Halton(unsigned index, unsigned base)
{
	float f = 1.0f, r = 0.0f;
	while (index > 0)
	{
		f /= base;
		r += f * (index % base);
		index /= base;
	}
	return r;
}

bool PPTemporalAA::Enabled() const
{
	// MSAA already resolves edges and its depth can't be sampled here.
	return r_taa && gl_multisample <= 1;
}

bool PPTemporalAA::ReactiveMask() const
{
	return Enabled() && r_taa_reactive;
}

IntRect PPTemporalAA::BeginUpscale(const IntRect &sceneViewport)
{
	Upscaling = false;
	UpscaleRatio = 1.0f;
	float ratio = clamp<float>(r_upscale, 0.5f, 1.0f);
	if (!Enabled() || ratio > 0.99f || sceneViewport.width < 64 || sceneViewport.height < 64)
		return sceneViewport;

	Upscaling = true;
	UpscaleRatio = ratio;
	OutputViewport = sceneViewport;
	IntRect input = sceneViewport;
	input.width = max(1, (int)round(sceneViewport.width * ratio));
	input.height = max(1, (int)round(sceneViewport.height * ratio));
	return input;
}

void PPTemporalAA::EndUpscale()
{
	if (Upscaling)
	{
		screen->mSceneViewport = OutputViewport;
		Upscaling = false;
	}
}

FVector2 PPTemporalAA::BeginFrame(int sceneWidth, int sceneHeight)
{
	CameraValid = false;
	if (!Enabled() || sceneWidth <= 0 || sceneHeight <= 0)
	{
		HistoryValid = false;
		PrevValid = false;
		JitterNDC = { 0.0f, 0.0f };
		return JitterNDC;
	}
	// A pause in 3D rendering (menus, wipes, console) invalidates the history.
	if (screen->FrameTime - LastFrame > 250)
		HistoryValid = false;
	// Upscaling needs more distinct sample positions per output pixel.
	unsigned phases = Upscaling ? (UpscaleRatio < 0.7f ? 32 : 16) : 8;
	FrameIndex = FrameIndex % phases + 1;
	JitterNDC.X = (Halton(FrameIndex, 2) - 0.5f) * 2.0f / sceneWidth;
	JitterNDC.Y = (Halton(FrameIndex, 3) - 0.5f) * 2.0f / sceneHeight;
	return JitterNDC;
}

void PPTemporalAA::SetCamera(const VSMatrix &projection, const VSMatrix &view, const DVector3 &pos)
{
	if (!Enabled())
		return;
	CurProj = projection;
	CurView = view;
	CurPos = pos;
	CameraValid = true;
}

void PPTemporalAA::UpdateTextures(int width, int height)
{
	if (width == Width && height == Height)
		return;
	Width = width;
	Height = height;
	History = { width, height, PixelFormat::Rgba16f };
	HistoryValid = false;
}

bool PPTemporalAA::PlatformUpscale(PPRenderState *renderstate, const TemporalUniforms &u, bool reset)
{
	// Render-resolution inputs: color, depth and camera motion of the rendered region.
	int inputWidth = max(1, (int)round(u.InputScale.X * screen->mScreenViewport.width));
	int inputHeight = max(1, (int)round(u.InputScale.Y * screen->mScreenViewport.height));
	int outputWidth = screen->mSceneViewport.width;
	int outputHeight = screen->mSceneViewport.height;
	if (inputWidth != UpscaleInputWidth || inputHeight != UpscaleInputHeight)
	{
		UpscaleColor = { inputWidth, inputHeight, PixelFormat::Rgba16f };
		UpscaleDepth = { inputWidth, inputHeight, PixelFormat::R32f };
		UpscaleMotion = { inputWidth, inputHeight, PixelFormat::Rg16f };
		UpscaleReactive = { inputWidth, inputHeight, PixelFormat::R16f };
		UpscaleInputWidth = inputWidth;
		UpscaleInputHeight = inputHeight;
		reset = true;
	}
	if (outputWidth != UpscaleOutputWidth || outputHeight != UpscaleOutputHeight)
	{
		UpscaleOutput = { outputWidth, outputHeight, PixelFormat::Rgba16f };
		UpscaleOutputWidth = outputWidth;
		UpscaleOutputHeight = outputHeight;
		reset = true;
	}

	renderstate->PushGroup("upscale");

	CompositeUniforms copy;
	copy.Scale = u.InputScale;
	copy.Offset = u.InputOffset;
	renderstate->Clear();
	renderstate->Shader = &Copy;
	renderstate->Uniforms.Set(copy);
	renderstate->Viewport = { 0, 0, inputWidth, inputHeight };
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetOutputTexture(&UpscaleColor);
	renderstate->SetNoBlend();
	renderstate->Draw();

	bool reactive = ReactiveMask();
	for (int pass = 0; pass < (reactive ? 3 : 2); pass++)
	{
		PPShader *shaders[3] = { &UpscaleDepthShader, &UpscaleMotionShader, &UpscaleReactiveShader };
		PPTexture *outputs[3] = { &UpscaleDepth, &UpscaleMotion, &UpscaleReactive };
		renderstate->Clear();
		renderstate->Shader = shaders[pass];
		renderstate->Uniforms.Set(u);
		renderstate->Viewport = { 0, 0, inputWidth, inputHeight };
		if (pass < 2) renderstate->SetInputSceneDepth(0);
		else renderstate->SetInputSceneNormal(0);
		renderstate->SetOutputTexture(outputs[pass]);
		renderstate->SetNoBlend();
		renderstate->Draw();
	}

	// The projection was offset by Jitter (scene UV units) this frame.
	if (!screen->PlatformTemporalUpscale(&UpscaleColor, &UpscaleDepth, &UpscaleMotion, reactive ? &UpscaleReactive : nullptr, &UpscaleOutput, u.Jitter.X * inputWidth, u.Jitter.Y * inputHeight, reset))
	{
		renderstate->PopGroup();
		return false; // Use the TAA resolve this frame if MetalFX allocation failed.
	}

	copy.Scale = { 1.0f, 1.0f };
	copy.Offset = { 0.0f, 0.0f };
	renderstate->Clear();
	renderstate->Shader = &Copy;
	renderstate->Uniforms.Set(copy);
	renderstate->Viewport = screen->mSceneViewport;
	renderstate->SetInputTexture(0, &UpscaleOutput, PPFilterMode::Nearest);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();
	return true;
}

void PPTemporalAA::Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight)
{
	if (!Enabled() || !CameraValid)
	{
		HistoryValid = false;
		PrevValid = false;
		EndUpscale();
		return;
	}
	CameraValid = false;
	LastFrame = screen->FrameTime;

	UpdateTextures(screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Teleports and level changes start over instead of smearing.
	if (PrevValid && (CurPos - PrevPos).LengthSquared() > 128.0 * 128.0)
		HistoryValid = false;
	if (!PrevValid)
	{
		PrevProj = CurProj;
		PrevView = CurView;
	}

	// current view space -> world -> previous clip space
	VSMatrix invView;
	VSMatrix view = CurView;
	view.inverseMatrix(invView);
	VSMatrix reprojection = PrevProj;
	reprojection.multMatrix(PrevView);
	reprojection.multMatrix(invView);

	// The scene was rendered into the input viewport; the result covers the full scene viewport.
	FVector2 inputScale = screen->SceneScale();
	FVector2 inputOffset = screen->SceneOffset();
	bool upscaling = Upscaling;
	EndUpscale();

	TemporalUniforms u;
	const FLOATTYPE *m = reprojection.get();
	for (int i = 0; i < 16; i++)
		u.Reprojection[i] = (float)m[i];
	float m5 = CurProj.get()[5];
	float tanHalfFovy = 1.0f / m5;
	float invFocalLenX = tanHalfFovy * (sceneWidth / (float)sceneHeight);
	u.UVToViewA = { 2.0f * invFocalLenX, 2.0f * tanHalfFovy };
	u.UVToViewB = { -invFocalLenX, -tanHalfFovy };
	u.Scale = screen->SceneScale();
	u.Offset = screen->SceneOffset();
	u.InvSize = { 1.0f / screen->mScreenViewport.width, 1.0f / screen->mScreenViewport.height };
	u.Jitter = { JitterNDC.X * 0.5f, JitterNDC.Y * 0.5f };
	u.InputScale = inputScale;
	u.InputOffset = inputOffset;
	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);
	u.Feedback = HistoryValid ? 0.9f : 0.0f;
	u.Upscale = upscaling ? 1.0f : 0.0f;
	u.Reactive = ReactiveMask() ? 1.0f : 0.0f;
	u.Padding0 = u.Padding1 = u.Padding2 = 0.0f;

	if (upscaling && r_upscale_metalfx && screen->SupportsPlatformUpscaler() && PlatformUpscale(renderstate, u, !HistoryValid))
	{
		HistoryValid = true;
		PrevValid = true;
		PrevProj = CurProj;
		PrevView = CurView;
		PrevPos = CurPos;
		return;
	}

	renderstate->PushGroup("taa");

	renderstate->Clear();
	renderstate->Shader = &Resolve;
	renderstate->Uniforms.Set(u);
	renderstate->Viewport = screen->mScreenViewport;
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetInputTexture(1, &History, PPFilterMode::Linear);
	renderstate->SetInputSceneDepth(2);
	renderstate->SetInputSceneNormal(3);
	renderstate->SetOutputNext();
	renderstate->SetNoBlend();
	renderstate->Draw();

	CompositeUniforms copy;
	copy.Scale = { 1.0f, 1.0f };
	copy.Offset = { 0.0f, 0.0f };
	renderstate->Clear();
	renderstate->Shader = &Copy;
	renderstate->Uniforms.Set(copy);
	renderstate->Viewport = { 0, 0, Width, Height };
	renderstate->SetInputCurrent(0, PPFilterMode::Nearest);
	renderstate->SetOutputTexture(&History);
	renderstate->SetNoBlend();
	renderstate->Draw();

	renderstate->PopGroup();

	HistoryValid = true;
	PrevValid = true;
	PrevProj = CurProj;
	PrevView = CurView;
	PrevPos = CurPos;
}
