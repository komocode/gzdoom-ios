/*
** sprite_materials.h
** Authored sprite material maps. See sprite_materials.cpp.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
*/

#pragma once

class FTexture;

// Adapt an authored data map to the sprite's texel dimensions, or provide a
// constant when a partial material omits a channel. No artwork is inferred.
enum class ESpriteMaterialChannel { Normal, Metallic, Roughness, AmbientOcclusion, Specular };
FTexture* CreateSpriteMaterialLayer(FTexture* source, int width, int height, ESpriteMaterialChannel channel);
