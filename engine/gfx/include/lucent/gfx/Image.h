#pragma once

#include "lucent/gfx/Device.h"

namespace lucent::gfx {

enum class ImageUsage {
    ColorAttachment,
    DepthAttachment,
    Sampled,
    Storage,
    TransferSrc,
    TransferDst
};

struct ImageDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool isCubemap = false;
    const char* debugName = nullptr;
};

class Image : public NonCopyable {
public:
    Image() = default;
    ~Image();
    
    bool Init(Device* device, const ImageDesc& desc);
    void Shutdown();
    
    // Layout transitions
    void TransitionLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout);
    
    // Getters
    VkImage GetHandle() const { return m_Image; }
    VkImageView GetView() const { return m_ImageView; }
    VkDeviceMemory GetMemory() const { return m_Memory; }
    VkFormat GetFormat() const { return m_Format; }
    VkExtent3D GetExtent() const { return m_Extent; }
    VkImageLayout GetCurrentLayout() const { return m_CurrentLayout; }
    
    uint32_t GetWidth() const { return m_Extent.width; }
    uint32_t GetHeight() const { return m_Extent.height; }
    
private:
    Device* m_Device = nullptr;
    
    VkImage m_Image = VK_NULL_HANDLE;
    VkImageView m_ImageView = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    
    VkFormat m_Format = VK_FORMAT_UNDEFINED;
    VkExtent3D m_Extent = { 0, 0, 0 };
    VkImageAspectFlags m_Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_MipLevels = 1;
    uint32_t m_ArrayLayers = 1;
};

} // namespace lucent::gfx

