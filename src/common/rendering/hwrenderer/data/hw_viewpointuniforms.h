#pragma once

#include "matrix.h"

struct HWDrawInfo;

enum class ELightBlendMode : uint8_t
{
	CLAMP = 0,
	CLAMP_COLOR = 1,
	NOCLAMP = 2,

	DEFAULT = CLAMP,
};

struct HWViewpointUniforms
{
	VSMatrix mProjectionMatrix;
	VSMatrix mViewMatrix;
	VSMatrix mNormalViewMatrix;
	FVector4 mCameraPos;
	FVector4 mClipLine;

	float mGlobVis = 1.f;
	int mPalLightLevels = 0;
	int mViewHeight = 0;
	float mClipHeight = 0.f;
	float mClipHeightDirection = 0.f;
	int mShadowmapFilter = 1;

	int mLightBlendMode = 0;

	float mThickFogDistance = -1.f;
	float mThickFogMultiplier = 30.f;

	// Modern lighting (see hw_modernrender.h). Layout must match std140 in the shaders.
	int mRenderFeatures = 0;
	float mGIStrength = 0.f;
	float mGISaturation = 0.f;
	float mSpecularStrength = 0.f;
	float mNormalStrength = 0.f;
	float mEmissiveStrength = 1.f;	// scale of HDR emission (gamma-space, sqrt of the linear strength)
	float mMipBias = 0.f;			// texture LOD bias for TAA / upscaling
	FVector4 mGIOrigin = { 0, 0, 0, 1 };	// irradiance volume origin (x, height, y) and probe spacing
	FVector4 mGISize = { 0, 0, 0, 0 };		// probe counts (x, height, y), w = 1 when a volume is bound
	// Sky exposure map (hw_weather.h): map origin (x, y), 1 / cell size, cell count x; w = 0 when no map is bound
	FVector4 mWeatherMap = { 0, 0, 0, 0 };
	FVector4 mWeatherParams = { 0, 0, 0, 0 };	// cell count y, lightning flash (0-1), time in seconds, unused
	FVector4 mSceneDepthMap = { 0, 0, 0, 0 };	// fragment position to half-res linear depth texel: scale (xy), offset (zw)

	void CalcDependencies()
	{
		mNormalViewMatrix.computeNormalMatrix(mViewMatrix);
	}
};



