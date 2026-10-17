#pragma once

class FTexture;

// Adapt an authored data map to the sprite's texel dimensions, or provide a
// constant when a partial material omits a channel. No artwork is inferred.
enum class ESpriteMaterialChannel { Normal, Metallic, Roughness, AmbientOcclusion, Specular };
FTexture* CreateSpriteMaterialLayer(FTexture* source, int width, int height, ESpriteMaterialChannel channel);
