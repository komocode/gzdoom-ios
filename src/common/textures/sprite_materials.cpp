#include "sprite_materials.h"
#include "textures.h"
#include "bitmap.h"

#include <algorithm>
#include <cstring>

namespace
{
class FSpriteMaterialTexture final : public FTexture
{
	RefCountedPtr<FTexture> Source;
	ESpriteMaterialChannel Channel;
public:
	FSpriteMaterialTexture(FTexture* source, int width, int height, ESpriteMaterialChannel channel)
		: Source(source), Channel(channel)
	{
		Width = width;
		Height = height;
		Masked = false;
		bTranslucent = 0; // normal alpha may contain height data, never coverage
	}

	bool IsMaterialData() const override { return true; }

	FBitmap GetBgraBitmap(const PalEntry*, int* trans = nullptr) override
	{
		FBitmap result;
		result.Create(Width, Height);
		if (trans) *trans = 0;
		if (Source.get())
		{
			// All data maps use the color sprite's exact dimensions. In particular,
			// CTF_Expand must add the same normalized border to every layer. A map
			// authored at another resolution is resampled without palette remapping.
			auto source = Source->GetBgraBitmap(nullptr);
			for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x)
			{
				int sx = std::min(source.GetWidth() - 1, int((int64_t(x) * 2 + 1) * source.GetWidth() / (int64_t(Width) * 2)));
				int sy = std::min(source.GetHeight() - 1, int((int64_t(y) * 2 + 1) * source.GetHeight() / (int64_t(Height) * 2)));
				if (sx >= 0 && sy >= 0)
					std::memcpy(result.GetPixels() + y * result.GetPitch() + x * 4,
						source.GetPixels() + sy * source.GetPitch() + sx * 4, 4);
			}
		}
		else
		{
			uint8_t pixel[4] = { 0, 0, 0, 255 };
			switch (Channel)
			{
			case ESpriteMaterialChannel::Normal: pixel[0] = 255; pixel[1] = pixel[2] = 128; pixel[3] = 0; break;
			case ESpriteMaterialChannel::Roughness: pixel[0] = pixel[1] = pixel[2] = 209; break;
			case ESpriteMaterialChannel::AmbientOcclusion: pixel[0] = pixel[1] = pixel[2] = 255; break;
			default: break;
			}
			for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x)
				std::memcpy(result.GetPixels() + y * result.GetPitch() + x * 4, pixel, 4);
		}
		return result;
	}
};
}

FTexture* CreateSpriteMaterialLayer(FTexture* source, int width, int height, ESpriteMaterialChannel channel)
{
	if (source && (source->GetWidth() <= 0 || source->GetHeight() <= 0)) source = nullptr;
	// Even equal-sized maps use a data wrapper: normal alpha may contain height,
	// so color-image alpha edge processing must never alter normal RGB values.
	return new FSpriteMaterialTexture(source, width, height, channel);
}
