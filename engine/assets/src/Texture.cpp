#include "lucent/assets/Texture.h"
#include "lucent/core/Log.h"
#include "lucent/gfx/Buffer.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <algorithm>

namespace lucent::assets {

Texture::~Texture() {
    Destroy();
}

bool Texture::LoadFromFile(gfx::Device* device, const TextureDesc& desc) {
    m_Device = device;
    m_Path = desc.path;
    m_Type = desc.type;
    m_Name = desc.debugName ? desc.debugName : desc.path;
    
    // Configure stb_image
    stbi_set_flip_vertically_on_load(desc.flipVertically ? 1 : 0);
    
    // Load image
    int width, height, channels;
    int desiredChannels = 4; // Always load as RGBA
    
    unsigned char* data = nullptr;
    float* hdrData = nullptr;
    bool isHDR = stbi_is_hdr(desc.path.c_str());
    
    if (isHDR) {
        hdrData = stbi_loadf(desc.path.c_str(), &width, &height, &channels, desiredChannels);
        if (!hdrData) {
            LUCENT_CORE_ERROR("Failed to load HDR texture: {} - {}", desc.path, stbi_failure_reason());
            return false;
        }
    } else {
        data = stbi_load(desc.path.c_str(), &width, &height, &channels, desiredChannels);
        if (!data) {
            LUCENT_CORE_ERROR("Failed to load texture: {} - {}", desc.path, stbi_failure_reason());
            return false;
        }
    }
    
    m_Width = static_cast<uint32_t>(width);
    m_Height = static_cast<uint32_t>(height);
    
    // Calculate mip levels
    if (desc.generateMips) {
        m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    } else {
        m_MipLevels = 1;
    }
    
    // Determine Vulkan format
    VkFormat vkFormat;
    size_t pixelSize;
    
    if (isHDR) {
        vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
        pixelSize = sizeof(float) * 4;
    } else {
        switch (desc.format) {
            case TextureFormat::RGBA8_SRGB:
                vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
                pixelSize = 4;
                break;
            case TextureFormat::RGBA8_UNORM:
                vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
                pixelSize = 4;
                break;
            default:
                vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
                pixelSize = 4;
                break;
        }
    }
    
    // Create staging buffer
    VkDeviceSize imageSize = m_Width * m_Height * pixelSize;
    
    gfx::BufferDesc stagingDesc{};
    stagingDesc.size = imageSize;
    stagingDesc.usage = gfx::BufferUsage::Staging;
    stagingDesc.hostVisible = true;
    
    gfx::Buffer stagingBuffer;
    if (!stagingBuffer.Init(device, stagingDesc)) {
        if (data) stbi_image_free(data);
        if (hdrData) stbi_image_free(hdrData);
        return false;
    }
    
    stagingBuffer.Upload(isHDR ? static_cast<void*>(hdrData) : static_cast<void*>(data), imageSize);
    
    // Free CPU image data
    if (data) stbi_image_free(data);
    if (hdrData) stbi_image_free(hdrData);
    
    // Create image
    gfx::ImageDesc imageDesc{};
    imageDesc.width = m_Width;
    imageDesc.height = m_Height;
    imageDesc.format = vkFormat;
    imageDesc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageDesc.mipLevels = m_MipLevels;
    imageDesc.debugName = m_Name.c_str();
    
    if (!m_Image.Init(device, imageDesc)) {
        stagingBuffer.Shutdown();
        return false;
    }
    
    // Copy data to image (using immediate command buffer)
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();
    
    // Transition to transfer destination
    m_Image.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { m_Width, m_Height, 1 };
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer.GetHandle(), m_Image.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Generate mipmaps (also transitions to SHADER_READ_ONLY_OPTIMAL)
    if (m_MipLevels > 1) {
        GenerateMipmaps(cmd);
    } else {
        m_Image.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    
    device->EndSingleTimeCommands(cmd);
    
    stagingBuffer.Shutdown();
    
    // Create sampler
    if (!CreateSampler()) {
        return false;
    }
    
    LUCENT_CORE_DEBUG("Loaded texture '{}': {}x{}, {} mips", m_Name, m_Width, m_Height, m_MipLevels);
    return true;
}

bool Texture::CreateFromData(gfx::Device* device, const void* data, 
                             uint32_t width, uint32_t height, uint32_t channels,
                             TextureFormat format, const std::string& name) {
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_MipLevels = 1;
    m_Name = name;
    
    (void)channels; // We expect the data to match the format
    
    VkFormat vkFormat;
    size_t pixelSize;
    
    switch (format) {
        case TextureFormat::RGBA8_SRGB:
            vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
            pixelSize = 4;
            break;
        case TextureFormat::RGBA8_UNORM:
            vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
            pixelSize = 4;
            break;
        default:
            vkFormat = VK_FORMAT_R8G8B8A8_SRGB;
            pixelSize = 4;
            break;
    }
    
    VkDeviceSize imageSize = width * height * pixelSize;
    
    // Create staging buffer
    gfx::BufferDesc stagingDesc{};
    stagingDesc.size = imageSize;
    stagingDesc.usage = gfx::BufferUsage::Staging;
    stagingDesc.hostVisible = true;
    
    gfx::Buffer stagingBuffer;
    if (!stagingBuffer.Init(device, stagingDesc)) {
        return false;
    }
    stagingBuffer.Upload(data, imageSize);
    
    // Create image
    gfx::ImageDesc imageDesc{};
    imageDesc.width = width;
    imageDesc.height = height;
    imageDesc.format = vkFormat;
    imageDesc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageDesc.mipLevels = 1;
    imageDesc.debugName = name.c_str();
    
    if (!m_Image.Init(device, imageDesc)) {
        stagingBuffer.Shutdown();
        return false;
    }
    
    // Copy
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();
    
    m_Image.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer.GetHandle(), m_Image.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    m_Image.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    device->EndSingleTimeCommands(cmd);
    
    stagingBuffer.Shutdown();
    
    if (!CreateSampler()) {
        return false;
    }
    
    return true;
}

bool Texture::CreateSolidColor(gfx::Device* device, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                               const std::string& name) {
    uint8_t pixel[4] = { r, g, b, a };
    return CreateFromData(device, pixel, 1, 1, 4, TextureFormat::RGBA8_SRGB, name);
}

void Texture::Destroy() {
    if (m_Device && m_Sampler != VK_NULL_HANDLE) {
        // Texture sampler might still be referenced by in-flight descriptor sets.
        vkDeviceWaitIdle(m_Device->GetContext()->GetDevice());
        vkDestroySampler(m_Device->GetContext()->GetDevice(), m_Sampler, nullptr);
        m_Sampler = VK_NULL_HANDLE;
    }
    m_Image.Shutdown();
}

bool Texture::CreateSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    // Only enable anisotropy if the feature is enabled on the logical device.
    // (Some GPUs support it but it must be explicitly enabled at device creation.)
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(m_Device->GetContext()->GetPhysicalDevice(), &features);
    if (features.samplerAnisotropy) {
        samplerInfo.anisotropyEnable = VK_TRUE;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_Device->GetContext()->GetPhysicalDevice(), &props);
        samplerInfo.maxAnisotropy = (props.limits.maxSamplerAnisotropy > 0.0f)
            ? std::min(16.0f, props.limits.maxSamplerAnisotropy)
            : 1.0f;
    } else {
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
    }
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_MipLevels);
    
    VkDevice vkDevice = m_Device->GetContext()->GetDevice();
    if (vkCreateSampler(vkDevice, &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create texture sampler");
        return false;
    }
    
    return true;
}

void Texture::GenerateMipmaps(VkCommandBuffer cmd) {
    VkImage image = m_Image.GetHandle();
    
    int32_t mipWidth = static_cast<int32_t>(m_Width);
    int32_t mipHeight = static_cast<int32_t>(m_Height);
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;
    
    for (uint32_t i = 1; i < m_MipLevels; i++) {
        // Transition previous mip to transfer source
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        
        // Blit from previous mip to current
        VkImageBlit blit{};
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        
        int32_t nextWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        int32_t nextHeight = mipHeight > 1 ? mipHeight / 2 : 1;
        
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        
        vkCmdBlitImage(cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);
        
        // Transition previous mip to shader read
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
        
        mipWidth = nextWidth;
        mipHeight = nextHeight;
    }
    
    // Transition last mip to shader read
    barrier.subresourceRange.baseMipLevel = m_MipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}

// ============================================================================
// Default Textures
// ============================================================================

bool DefaultTextures::Init(gfx::Device* device) {
    if (m_Initialized) return true;
    
    // White (1, 1, 1, 1)
    if (!m_White.CreateSolidColor(device, 255, 255, 255, 255, "Default_White")) {
        return false;
    }
    
    // Black (0, 0, 0, 1)
    if (!m_Black.CreateSolidColor(device, 0, 0, 0, 255, "Default_Black")) {
        return false;
    }
    
    // Flat normal (0.5, 0.5, 1.0)
    if (!m_Normal.CreateSolidColor(device, 128, 128, 255, 255, "Default_Normal")) {
        return false;
    }
    
    // Mid-gray roughness (0.5)
    if (!m_Roughness.CreateSolidColor(device, 128, 128, 128, 255, "Default_Roughness")) {
        return false;
    }
    
    m_Initialized = true;
    LUCENT_CORE_INFO("Default textures initialized");
    return true;
}

void DefaultTextures::Shutdown() {
    m_Roughness.Destroy();
    m_Normal.Destroy();
    m_Black.Destroy();
    m_White.Destroy();
    m_Initialized = false;
}

} // namespace lucent::assets

