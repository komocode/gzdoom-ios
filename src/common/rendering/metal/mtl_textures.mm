/*
** mtl_textures.mm
** Textures, materials, samplers and render targets for the Metal backend.
*/

#include "mtl_renderdevice.h"
#include "mtl_postprocess.h"
#include "c_cvars.h"
#include "hw_cvars.h"
#include "hw_material.h"
#include "textures.h"
#include "palettecontainer.h"
#include "i_interface.h"
#include "engineerrors.h"

static int GetMipLevels(int w, int h)
{
	int levels = 1;
	while (w > 1 || h > 1)
	{
		w = std::max(w >> 1, 1);
		h = std::max(h >> 1, 1);
		levels++;
	}
	return levels;
}

/////////////////////////////////////////////////////////////////////////////

MtlHardwareTexture::MtlHardwareTexture(MetalRenderDevice *fb, int numchannels) : fb(fb)
{
	mTexelsize = numchannels;
	fb->GetTextureManager()->AddTexture(this);
}

MtlHardwareTexture::~MtlHardwareTexture()
{
	if (fb)
		fb->GetTextureManager()->RemoveTexture(this);
}

void MtlHardwareTexture::Reset()
{
	mImage.Reset();
	mDepthStencil.Reset();
	mLinearBuffer = nil;
}

MtlTextureImage *MtlHardwareTexture::GetImage(FTexture *tex, int translation, int flags)
{
	if (!mImage.Texture)
		CreateImage(tex, translation, flags);
	return &mImage;
}

MtlTextureImage *MtlHardwareTexture::GetDepthStencil(FTexture *tex)
{
	if (!mDepthStencil.Texture)
	{
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MtlRenderBuffers::DepthStencilFormat width:tex->GetWidth() height:tex->GetHeight() mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget;
		desc.storageMode = MTLStorageModePrivate;
		mDepthStencil.Texture = [fb->device newTextureWithDescriptor:desc];
	}
	return &mDepthStencil;
}

void MtlHardwareTexture::CreateImage(FTexture *tex, int translation, int flags)
{
	if (!tex->isHardwareCanvas())
	{
		FTextureBuffer texbuffer = tex->CreateTexBuffer(translation, flags | CTF_ProcessData);
		bool indexed = flags & CTF_Indexed;
		CreateTexture(texbuffer.mWidth, texbuffer.mHeight, indexed ? 1 : 4, indexed ? MTLPixelFormatR8Unorm : MTLPixelFormatBGRA8Unorm, texbuffer.mBuffer, !indexed);
	}
	else
	{
		MTLPixelFormat format = tex->IsHDR() ? MTLPixelFormatRGBA32Float : MTLPixelFormatRGBA8Unorm;
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:tex->GetWidth() height:tex->GetHeight() mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModePrivate;
		mImage.Texture = [fb->device newTextureWithDescriptor:desc];
	}
}

void MtlHardwareTexture::CreateTexture(int w, int h, int pixelsize, MTLPixelFormat format, const void *pixels, bool mipmap)
{
	if (w <= 0 || h <= 0)
		throw CMetalError("Trying to create zero size texture");

	int levels = mipmap ? GetMipLevels(w, h) : 1;
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:levels > 1 ? YES : NO];
	desc.mipmapLevelCount = levels;
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = fb->UploadStorageMode();
	mImage.Texture = [fb->device newTextureWithDescriptor:desc];
	[mImage.Texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels bytesPerRow:w * pixelsize];
	if (levels > 1)
		[fb->GetUploadEncoder() generateMipmapsForTexture:mImage.Texture];
}

void MtlHardwareTexture::AllocateBuffer(int w, int h, int texelsize)
{
	if (mImage.Texture && (mImage.Width() != w || mImage.Height() != h || mTexelsize != texelsize))
		Reset();

	if (!mImage.Texture)
	{
		// A texture over a linear buffer: the software renderer writes the pixels in place.
		MTLPixelFormat format = texelsize == 4 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatR8Unorm;
		NSUInteger align = [fb->device minimumLinearTextureAlignmentForPixelFormat:format];
		NSUInteger pitch = (w * texelsize + align - 1) / align * align;
		mLinearBuffer = [fb->device newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];

		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = MTLStorageModeShared;
		mImage.Texture = [mLinearBuffer newTextureWithDescriptor:desc offset:0 bytesPerRow:pitch];
		mTexelsize = texelsize;
		bufferpitch = int(pitch / texelsize);
	}
}

uint8_t *MtlHardwareTexture::MapBuffer()
{
	if (!mLinearBuffer)
		return nullptr;
	// The software renderer alternates between two of these. With more frames
	// in flight than that, wait until the frame that last showed this one is done.
	if (mLinearFrame && mLinearFrame < fb->CurrentFrame())
		fb->WaitForFrame(mLinearFrame);
	mLinearFrame = fb->CurrentFrame();
	return (uint8_t*)mLinearBuffer.contents;
}

unsigned int MtlHardwareTexture::CreateTexture(unsigned char *buffer, int w, int h, int texunit, bool mipmap, const char *name)
{
	// Used by the software renderer to create a screen output, normally without data.
	if (buffer)
		CreateTexture(w, h, mTexelsize, mTexelsize == 4 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatR8Unorm, buffer, mipmap);
	return 0;
}

void MtlHardwareTexture::CreateWipeTexture(int w, int h, const char *name)
{
	// Same format as the postprocess pipeline images so the frame can be copied directly.
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = fb->UploadStorageMode();
	mImage.Texture = [fb->device newTextureWithDescriptor:desc];
	mTexelsize = 4;

	if (fb->GetBuffers()->GetWidth() > 0 && fb->GetBuffers()->GetHeight() > 0)
	{
		fb->GetPostprocess()->BlitCurrentToImage(&mImage);
	}
	else
	{
		// The frame buffer was never written: hand back black.
		TArray<uint16_t> black(w * 4, true);
		for (int x = 0; x < w; x++)
		{
			black[x * 4] = black[x * 4 + 1] = black[x * 4 + 2] = 0;
			black[x * 4 + 3] = 0x3c00; // half-float 1.0
		}
		for (int y = 0; y < h; y++)
			[mImage.Texture replaceRegion:MTLRegionMake2D(0, y, w, 1) mipmapLevel:0 withBytes:black.Data() bytesPerRow:w * 8];
	}
}

/////////////////////////////////////////////////////////////////////////////

MtlMaterial::MtlMaterial(MetalRenderDevice *fb, FGameTexture *tex, int scaleflags) : FMaterial(tex, scaleflags), fb(fb)
{
}

MtlMaterial::~MtlMaterial()
{
}

void MtlMaterial::DeleteDescriptors()
{
	mBindings.clear();
}

const MtlMaterial::Binding *MtlMaterial::GetBinding(const FMaterialState &state)
{
	auto base = Source();
	int clampmode = state.mClampMode;
	int translation = state.mTranslation;
	auto translationp = IsLuminosityTranslation(translation) ? translation : intptr_t(GPalette.GetTranslation(GetTranslationType(translation), GetTranslationIndex(translation)));

	clampmode = base->GetClampMode(clampmode);

	int generation = fb->GetSamplerManager()->Generation;
	for (auto &binding : mBindings)
	{
		if (binding->generation == generation && binding->clampmode == clampmode && binding->remap == translationp)
			return binding.get();
	}
	if (!mBindings.empty() && mBindings[0]->generation != generation)
		mBindings.clear();

	auto binding = std::make_unique<Binding>();
	binding->generation = generation;
	binding->clampmode = clampmode;
	binding->remap = translationp;
	binding->sampler = fb->GetSamplerManager()->Get(clampmode);

	int numLayers = NumLayers();
	MaterialLayerInfo *layer;
	auto systex = static_cast<MtlHardwareTexture*>(GetLayer(0, state.mTranslation, &layer));
	binding->textures[0] = systex->GetImage(layer->layerTexture, state.mTranslation, layer->scaleFlags)->Texture;

	if (!(layer->scaleFlags & CTF_Indexed))
	{
		for (int i = 1; i < numLayers && i < MTLTEX_MATERIAL_COUNT; i++)
		{
			auto syslayer = static_cast<MtlHardwareTexture*>(GetLayer(i, 0, &layer));
			binding->textures[i] = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags)->Texture;
		}
	}
	else
	{
		for (int i = 1; i < 3; i++)
		{
			auto syslayer = static_cast<MtlHardwareTexture*>(GetLayer(i, translation, &layer));
			binding->textures[i] = syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags)->Texture;
		}
		numLayers = 3;
	}

	for (int i = std::max(numLayers, 1); i < MTLTEX_MATERIAL_COUNT; i++)
		binding->textures[i] = fb->GetTextureManager()->NullTexture;

	mBindings.push_back(std::move(binding));
	return mBindings.back().get();
}

/////////////////////////////////////////////////////////////////////////////

struct MtlTexFilter
{
	MTLSamplerMinMagFilter minFilter;
	MTLSamplerMinMagFilter magFilter;
	MTLSamplerMipFilter mipFilter;
};

static const MtlTexFilter TexFilter[] =
{
	{ MTLSamplerMinMagFilterNearest, MTLSamplerMinMagFilterNearest, MTLSamplerMipFilterNotMipmapped },
	{ MTLSamplerMinMagFilterNearest, MTLSamplerMinMagFilterNearest, MTLSamplerMipFilterNearest },
	{ MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear, MTLSamplerMipFilterNotMipmapped },
	{ MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear, MTLSamplerMipFilterNearest },
	{ MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterLinear, MTLSamplerMipFilterLinear },
	{ MTLSamplerMinMagFilterNearest, MTLSamplerMinMagFilterNearest, MTLSamplerMipFilterLinear },
	{ MTLSamplerMinMagFilterLinear, MTLSamplerMinMagFilterNearest, MTLSamplerMipFilterLinear },
};

static const MTLSamplerAddressMode TexClamp[4][2] =
{
	{ MTLSamplerAddressModeRepeat, MTLSamplerAddressModeRepeat },
	{ MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat },
	{ MTLSamplerAddressModeRepeat, MTLSamplerAddressModeClampToEdge },
	{ MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeClampToEdge },
};

MtlSamplerManager::MtlSamplerManager(MetalRenderDevice *fb) : fb(fb)
{
	CreateHWSamplers();

	MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
	desc.minFilter = MTLSamplerMinMagFilterNearest;
	desc.magFilter = MTLSamplerMinMagFilterNearest;
	desc.sAddressMode = desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
	ShadowmapSampler = [fb->device newSamplerStateWithDescriptor:desc];

	desc = [MTLSamplerDescriptor new];
	desc.minFilter = MTLSamplerMinMagFilterLinear;
	desc.magFilter = MTLSamplerMinMagFilterLinear;
	desc.mipFilter = MTLSamplerMipFilterLinear;
	desc.sAddressMode = desc.tAddressMode = desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
	LightmapSampler = [fb->device newSamplerStateWithDescriptor:desc];
}

void MtlSamplerManager::ResetHWSamplers()
{
	CreateHWSamplers();
	Generation++;
}

void MtlSamplerManager::CreateHWSamplers()
{
	int filter = sysCallbacks.DisableTextureFilter && sysCallbacks.DisableTextureFilter() ? 0 : gl_texture_filter;
	filter = std::clamp(filter, 0, 6);
	const MtlTexFilter &f = TexFilter[filter];
	bool mipmapping = f.mipFilter != MTLSamplerMipFilterNotMipmapped;

	for (int i = CLAMP_NONE; i <= CLAMP_XY; i++)
	{
		MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
		desc.minFilter = f.minFilter;
		desc.magFilter = f.magFilter;
		desc.mipFilter = f.mipFilter;
		desc.sAddressMode = TexClamp[i][0];
		desc.tAddressMode = TexClamp[i][1];
		if (mipmapping)
			desc.maxAnisotropy = std::clamp((int)gl_texture_filter_anisotropic, 1, 16);
		mSamplers[i] = [fb->device newSamplerStateWithDescriptor:desc];
	}

	MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
	desc.minFilter = f.magFilter;
	desc.magFilter = f.magFilter;
	desc.sAddressMode = desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
	mSamplers[CLAMP_XY_NOMIP] = [fb->device newSamplerStateWithDescriptor:desc];

	for (int i = CLAMP_NOFILTER; i <= CLAMP_NOFILTER_XY; i++)
	{
		desc = [MTLSamplerDescriptor new];
		desc.minFilter = MTLSamplerMinMagFilterNearest;
		desc.magFilter = MTLSamplerMinMagFilterNearest;
		desc.sAddressMode = TexClamp[i - CLAMP_NOFILTER][0];
		desc.tAddressMode = TexClamp[i - CLAMP_NOFILTER][1];
		mSamplers[i] = [fb->device newSamplerStateWithDescriptor:desc];
	}

	// CAMTEX is repeating with texture filter and no mipmap
	desc = [MTLSamplerDescriptor new];
	desc.minFilter = f.magFilter;
	desc.magFilter = f.magFilter;
	desc.sAddressMode = desc.tAddressMode = MTLSamplerAddressModeRepeat;
	mSamplers[CLAMP_CAMTEX] = [fb->device newSamplerStateWithDescriptor:desc];
}

id<MTLSamplerState> MtlSamplerManager::Get(PPFilterMode filter, PPWrapMode wrap)
{
	int index = (((int)filter) << 1) | (int)wrap;
	if (!mPPSamplers[index])
	{
		MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
		desc.minFilter = desc.magFilter = filter == PPFilterMode::Nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
		desc.sAddressMode = desc.tAddressMode = wrap == PPWrapMode::Clamp ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
		mPPSamplers[index] = [fb->device newSamplerStateWithDescriptor:desc];
	}
	return mPPSamplers[index];
}

/////////////////////////////////////////////////////////////////////////////

MtlPPTexture::MtlPPTexture(MetalRenderDevice *fb, PPTexture *texture) : fb(fb)
{
	int pixelsize;
	switch (texture->Format)
	{
	default:
	case PixelFormat::Rgba8: Format = MTLPixelFormatRGBA8Unorm; pixelsize = 4; break;
	case PixelFormat::Rgba16f: Format = MTLPixelFormatRGBA16Float; pixelsize = 8; break;
	case PixelFormat::R32f: Format = MTLPixelFormatR32Float; pixelsize = 4; break;
	case PixelFormat::Rg16f: Format = MTLPixelFormatRG16Float; pixelsize = 4; break;
	case PixelFormat::Rgba16_snorm: Format = MTLPixelFormatRGBA16Snorm; pixelsize = 8; break;
	case PixelFormat::R16f: Format = MTLPixelFormatR16Float; pixelsize = 2; break;
	}

	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:Format width:texture->Width height:texture->Height mipmapped:NO];
	if (texture->Data)
	{
		desc.usage = MTLTextureUsageShaderRead;
		desc.storageMode = fb->UploadStorageMode();
	}
	else
	{
		// Shader write lets MetalFX use it as its output.
		desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
		desc.storageMode = MTLStorageModePrivate;
	}
	TexImage.Texture = [fb->device newTextureWithDescriptor:desc];

	if (texture->Data)
	{
		[TexImage.Texture replaceRegion:MTLRegionMake2D(0, 0, texture->Width, texture->Height) mipmapLevel:0 withBytes:texture->Data.get() bytesPerRow:texture->Width * pixelsize];
	}
	else
	{
		// Zero it like the render targets. This can run inside a render pass, so
		// it goes through the upload commands, which execute before the draws.
		NSUInteger size = (NSUInteger)texture->Width * texture->Height * pixelsize;
		id<MTLBuffer> zero = [fb->device newBufferWithLength:size options:MTLResourceStorageModePrivate];
		id<MTLBlitCommandEncoder> blit = fb->GetUploadEncoder();
		[blit fillBuffer:zero range:NSMakeRange(0, size) value:0];
		[blit copyFromBuffer:zero sourceOffset:0 sourceBytesPerRow:texture->Width * pixelsize sourceBytesPerImage:size sourceSize:MTLSizeMake(texture->Width, texture->Height, 1)
			toTexture:TexImage.Texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	}

	fb->GetTextureManager()->AddPPTexture(this);
}

MtlPPTexture::~MtlPPTexture()
{
	if (fb)
		fb->GetTextureManager()->RemovePPTexture(this);
}

/////////////////////////////////////////////////////////////////////////////

MtlTextureManager::MtlTextureManager(MetalRenderDevice *fb) : fb(fb)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = fb->UploadStorageMode();
	NullTexture = [fb->device newTextureWithDescriptor:desc];
	uint32_t zero = 0;
	[NullTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];

	CreateShadowmap();

	TArray<uint16_t> data;
	data.Push(0);
	data.Push(0);
	data.Push(0);
	data.Push(0x3c00); // half-float 1.0
	SetLightmap(1, 1, data);
	SetIrradianceVolume(0, 0, 0, {});
	SetWeatherMap(0, 0, {});
}

MtlTextureManager::~MtlTextureManager()
{
	Deinit();
}

void MtlTextureManager::Deinit()
{
	while (!Textures.empty())
		RemoveTexture(Textures.back());
	while (!PPTextures.empty())
		RemovePPTexture(PPTextures.back());
}

void MtlTextureManager::BeginFrame()
{
	SceneDepth = nil;
	if (Shadowmap.Width() != gl_shadowmap_quality)
		CreateShadowmap();
}

void MtlTextureManager::CreateShadowmap()
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:gl_shadowmap_quality height:1024 mipmapped:NO];
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	Shadowmap.Texture = [fb->device newTextureWithDescriptor:desc];
}

void MtlTextureManager::SetLightmap(int size, int count, const TArray<uint16_t> &data)
{
	UploadHalfArray(Lightmap, size, size, count, data.Data(), 3);
}

void MtlTextureManager::SetIrradianceVolume(int width, int height, int layers, const TArray<uint16_t> &rgbaHalfData)
{
	if (width <= 0 || height <= 0 || layers <= 0 || rgbaHalfData.Size() < unsigned(width * height * layers * 4))
	{
		uint16_t black[4] = { 0, 0, 0, 0 };
		UploadHalfArray(Irradiance, 1, 1, 1, black, 4);
		return;
	}
	UploadHalfArray(Irradiance, width, height, layers, rgbaHalfData.Data(), 4);
}

void MtlTextureManager::SetWeatherMap(int width, int height, const TArray<uint16_t> &rgbaHalfData)
{
	if (width <= 0 || height <= 0 || rgbaHalfData.Size() < unsigned(width * height * 4))
	{
		uint16_t black[4] = { 0, 0, 0, 0 };
		UploadHalfArray(WeatherMap, 1, 1, 1, black, 4);
		return;
	}
	UploadHalfArray(WeatherMap, width, height, 1, rgbaHalfData.Data(), 4);
}

void MtlTextureManager::UploadHalfArray(MtlTextureImage &image, int w, int h, int count, const uint16_t *src, int srcChannels)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2DArray;
	desc.pixelFormat = MTLPixelFormatRGBA16Float;
	desc.width = w;
	desc.height = h;
	desc.arrayLength = count;
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = fb->UploadStorageMode();
	image.Texture = [fb->device newTextureWithDescriptor:desc];

	TArray<uint16_t> slice(w * h * 4, true);
	for (int layer = 0; layer < count; layer++)
	{
		const uint16_t *data = src;
		if (srcChannels == 4)
		{
			data = src + size_t(layer) * w * h * 4;
		}
		else
		{
			const uint16_t *in = src + size_t(layer) * w * h * 3;
			for (int i = 0; i < w * h; i++)
			{
				slice[i * 4] = in[i * 3];
				slice[i * 4 + 1] = in[i * 3 + 1];
				slice[i * 4 + 2] = in[i * 3 + 2];
				slice[i * 4 + 3] = 0x3c00;
			}
			data = slice.Data();
		}
		[image.Texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 slice:layer withBytes:data bytesPerRow:w * 8 bytesPerImage:w * h * 8];
	}
}

void MtlTextureManager::AddTexture(MtlHardwareTexture *texture)
{
	texture->it = Textures.insert(Textures.end(), texture);
}

void MtlTextureManager::RemoveTexture(MtlHardwareTexture *texture)
{
	texture->Reset();
	texture->fb = nullptr;
	Textures.erase(texture->it);
}

void MtlTextureManager::AddPPTexture(MtlPPTexture *texture)
{
	texture->it = PPTextures.insert(PPTextures.end(), texture);
}

void MtlTextureManager::RemovePPTexture(MtlPPTexture *texture)
{
	texture->Reset();
	texture->fb = nullptr;
	PPTextures.erase(texture->it);
}

MtlPPTexture *MtlTextureManager::GetMtlTexture(PPTexture *texture)
{
	if (!texture->Backend)
		texture->Backend = std::make_unique<MtlPPTexture>(fb, texture);
	return static_cast<MtlPPTexture*>(texture->Backend.get());
}

MTLPixelFormat MtlTextureManager::GetTextureFormat(PPTexture *texture)
{
	return GetMtlTexture(texture)->Format;
}

MtlTextureImage *MtlTextureManager::GetTexture(PPTextureType type, PPTexture *pptexture)
{
	auto buffers = fb->GetBuffers();
	switch (type)
	{
	case PPTextureType::CurrentPipelineTexture:
	case PPTextureType::NextPipelineTexture:
	{
		int idx = fb->GetPostprocess()->GetCurrentPipelineImage();
		if (type == PPTextureType::NextPipelineTexture)
			idx = (idx + 1) % MtlRenderBuffers::NumPipelineImages;
		return &buffers->PipelineImage[idx];
	}
	case PPTextureType::PPTexture: return &GetMtlTexture(pptexture)->TexImage;
	case PPTextureType::SceneColor: return &buffers->SceneColor;
	case PPTextureType::SceneNormal: return &buffers->SceneNormal;
	case PPTextureType::SceneFog: return &buffers->SceneFog;
	case PPTextureType::SceneDepth: return &buffers->SceneDepthStencil;
	case PPTextureType::ShadowMap: return &Shadowmap;
	case PPTextureType::SwapChain: return nullptr;
	}
	return nullptr;
}

/////////////////////////////////////////////////////////////////////////////

MtlRenderBuffers::MtlRenderBuffers(MetalRenderDevice *fb) : fb(fb)
{
}

id<MTLTexture> MtlRenderBuffers::CreateTarget(int width, int height, MTLPixelFormat format, int samples)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
	if (samples > 1)
	{
		desc.textureType = MTLTextureType2DMultisample;
		desc.sampleCount = samples;
	}
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	id<MTLTexture> texture = [fb->device newTextureWithDescriptor:desc];

	// New textures can hold stale memory. NaNs there would survive every blended
	// draw (NaN * 0 is NaN), e.g. 2D screens that never render a scene first.
	if (format != DepthStencilFormat)
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		[[fb->GetDrawCommands() renderCommandEncoderWithDescriptor:pass] endEncoding];
	}
	return texture;
}

int MtlRenderBuffers::GetBestSampleCount()
{
	int requested = std::clamp((int)gl_multisample, 1, 8);
	int best = 1;
	for (int samples = 2; samples <= requested; samples <<= 1)
	{
		if ([fb->device supportsTextureSampleCount:samples])
			best = samples;
	}
	return best;
}

void MtlRenderBuffers::BeginFrame(int width, int height, int sceneWidth, int sceneHeight)
{
	int samples = GetBestSampleCount();
	bool resize = width != mWidth || height != mHeight;
	if (resize || samples != mSamples)
	{
		// Make sure the GPU no longer needs the old targets before the render state drops them.
		fb->WaitForCommands(false);

		if (resize)
		{
			for (auto &image : PipelineImage)
				image.Texture = CreateTarget(width, height, MTLPixelFormatRGBA16Float);
			PipelineDepthStencil.Texture = CreateTarget(width, height, DepthStencilFormat);
		}

		SceneColor.Texture = CreateTarget(width, height, MTLPixelFormatRGBA16Float, samples);
		SceneDepthStencil.Texture = CreateTarget(width, height, DepthStencilFormat, samples);
		SceneNormal.Texture = CreateTarget(width, height, SceneNormalFormat, samples);
		SceneFog.Texture = CreateTarget(width, height, MTLPixelFormatRGBA8Unorm, samples);
	}

	mSamples = samples;
	mWidth = width;
	mHeight = height;
	mSceneWidth = sceneWidth;
	mSceneHeight = sceneHeight;
}
