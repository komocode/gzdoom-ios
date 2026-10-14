
#pragma once

#include <zvulkan/vulkanobjects.h>
#include "vulkan/textures/vk_imagetransition.h"
#include <list>

class VulkanRenderDevice;
class VkHardwareTexture;
class VkMaterial;
class VkPPTexture;
class VkTextureImage;
enum class PPTextureType;
class PPTexture;

class VkTextureManager
{
public:
	VkTextureManager(VulkanRenderDevice* fb);
	~VkTextureManager();

	void Deinit();

	void BeginFrame();

	void SetLightmap(int LMTextureSize, int LMTextureCount, const TArray<uint16_t>& LMTextureData);
	void SetIrradianceVolume(int width, int height, int layers, const TArray<uint16_t>& rgbaHalfData);
	void SetWeatherMap(int width, int height, const TArray<uint16_t>& rgbaHalfData);

	VkTextureImage* GetTexture(const PPTextureType& type, PPTexture* tex);
	VkFormat GetTextureFormat(PPTexture* texture);

	void AddTexture(VkHardwareTexture* texture);
	void RemoveTexture(VkHardwareTexture* texture);

	void AddPPTexture(VkPPTexture* texture);
	void RemovePPTexture(VkPPTexture* texture);

	VulkanImage* GetNullTexture() { return NullTexture.get(); }
	VulkanImageView* GetNullTextureView() { return NullTextureView.get(); }

	VkTextureImage Shadowmap;
	VkTextureImage Lightmap;
	VkTextureImage Irradiance;
	VkTextureImage WeatherMap;

private:
	void CreateNullTexture();
	void CreateShadowmap();
	void CreateLightmap();
	void UploadHalfArray(VkTextureImage& image, int w, int h, int count, const uint16_t* src, int srcChannels, const char* name);

	VkPPTexture* GetVkTexture(PPTexture* texture);

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareTexture*> Textures;
	std::list<VkPPTexture*> PPTextures;

	std::unique_ptr<VulkanImage> NullTexture;
	std::unique_ptr<VulkanImageView> NullTextureView;
};
