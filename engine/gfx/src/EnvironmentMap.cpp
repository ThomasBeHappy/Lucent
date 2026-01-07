#include "lucent/gfx/EnvironmentMap.h"
#include "lucent/core/Log.h"
#include "lucent/gfx/Buffer.h"

#include <stb_image.h>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace lucent::gfx {

EnvironmentMap::~EnvironmentMap() {
    Shutdown();
}

bool EnvironmentMap::LoadFromFile(Device* device, const std::string& path) {
    m_Device = device;
    m_Path = path;
    
    // Load HDR image
    stbi_set_flip_vertically_on_load(1);
    
    int width, height, channels;
    float* hdrData = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
    
    if (!hdrData) {
        LUCENT_CORE_ERROR("Failed to load HDR environment: {} - {}", path, stbi_failure_reason());
        return false;
    }
    
    m_Width = static_cast<uint32_t>(width);
    m_Height = static_cast<uint32_t>(height);
    
    // Calculate luminance for each pixel (for importance sampling)
    std::vector<float> luminance(m_Width * m_Height);
    for (uint32_t y = 0; y < m_Height; y++) {
        // Weight by sin(theta) for equirectangular projection
        float theta = 3.14159265359f * (static_cast<float>(y) + 0.5f) / static_cast<float>(m_Height);
        float sinTheta = std::sin(theta);
        
        for (uint32_t x = 0; x < m_Width; x++) {
            size_t idx = (y * m_Width + x) * 4;
            float r = hdrData[idx + 0];
            float g = hdrData[idx + 1];
            float b = hdrData[idx + 2];
            // Luminance formula
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            luminance[y * m_Width + x] = lum * sinTheta;
        }
    }
    
    // Create environment texture
    VkDeviceSize imageSize = m_Width * m_Height * 4 * sizeof(float);
    
    BufferDesc stagingDesc{};
    stagingDesc.size = imageSize;
    stagingDesc.usage = BufferUsage::Staging;
    stagingDesc.hostVisible = true;
    
    Buffer stagingBuffer;
    if (!stagingBuffer.Init(device, stagingDesc)) {
        stbi_image_free(hdrData);
        return false;
    }
    stagingBuffer.Upload(hdrData, imageSize);
    
    stbi_image_free(hdrData);
    
    // Create HDR image
    ImageDesc imageDesc{};
    imageDesc.width = m_Width;
    imageDesc.height = m_Height;
    imageDesc.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageDesc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageDesc.mipLevels = 1;
    imageDesc.debugName = "EnvironmentMap";
    
    if (!m_EnvImage.Init(device, imageDesc)) {
        stagingBuffer.Shutdown();
        return false;
    }
    
    // Copy to GPU
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();
    
    m_EnvImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { m_Width, m_Height, 1 };
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer.GetHandle(), m_EnvImage.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    m_EnvImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    device->EndSingleTimeCommands(cmd);
    stagingBuffer.Shutdown();
    
    // Build importance sampling tables
    if (!BuildImportanceSamplingTables(luminance)) {
        LUCENT_CORE_WARN("Failed to build importance sampling tables, using uniform sampling");
    }
    
    // Create sampler
    if (!CreateSampler()) {
        return false;
    }
    
    m_Loaded = true;
    LUCENT_CORE_INFO("Loaded HDR environment: {} ({}x{})", path, m_Width, m_Height);
    return true;
}

bool EnvironmentMap::CreateDefaultSky(Device* device) {
    m_Device = device;
    m_Path = "<default_sky>";
    
    // Create a simple gradient sky (16x16 is enough for a gradient)
    m_Width = 64;
    m_Height = 32;
    
    std::vector<float> hdrData(m_Width * m_Height * 4);
    std::vector<float> luminance(m_Width * m_Height);
    
    for (uint32_t y = 0; y < m_Height; y++) {
        float v = static_cast<float>(y) / static_cast<float>(m_Height - 1);
        float theta = 3.14159265359f * v;
        float sinTheta = std::sin(theta);
        
        // Gradient from zenith (blue) to horizon (white) to nadir (dark)
        float r, g, b;
        if (v < 0.5f) {
            // Upper hemisphere: blue to white
            float t = v * 2.0f;
            r = 0.5f + 0.5f * t;
            g = 0.7f + 0.3f * t;
            b = 1.0f;
        } else {
            // Lower hemisphere: white to dark ground
            float t = (v - 0.5f) * 2.0f;
            r = 1.0f - 0.9f * t;
            g = 1.0f - 0.9f * t;
            b = 1.0f - 0.9f * t;
        }
        
        // Apply some intensity variation
        float intensity = 0.3f;
        r *= intensity;
        g *= intensity;
        b *= intensity;
        
        for (uint32_t x = 0; x < m_Width; x++) {
            size_t idx = (y * m_Width + x) * 4;
            hdrData[idx + 0] = r;
            hdrData[idx + 1] = g;
            hdrData[idx + 2] = b;
            hdrData[idx + 3] = 1.0f;
            
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            luminance[y * m_Width + x] = lum * sinTheta;
        }
    }
    
    // Create environment texture
    VkDeviceSize imageSize = m_Width * m_Height * 4 * sizeof(float);
    
    BufferDesc stagingDesc{};
    stagingDesc.size = imageSize;
    stagingDesc.usage = BufferUsage::Staging;
    stagingDesc.hostVisible = true;
    
    Buffer stagingBuffer;
    if (!stagingBuffer.Init(device, stagingDesc)) {
        return false;
    }
    stagingBuffer.Upload(hdrData.data(), imageSize);
    
    // Create HDR image
    ImageDesc imageDesc{};
    imageDesc.width = m_Width;
    imageDesc.height = m_Height;
    imageDesc.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageDesc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageDesc.mipLevels = 1;
    imageDesc.debugName = "DefaultSky";
    
    if (!m_EnvImage.Init(device, imageDesc)) {
        stagingBuffer.Shutdown();
        return false;
    }
    
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();
    
    m_EnvImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { m_Width, m_Height, 1 };
    
    vkCmdCopyBufferToImage(cmd, stagingBuffer.GetHandle(), m_EnvImage.GetHandle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    m_EnvImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    device->EndSingleTimeCommands(cmd);
    stagingBuffer.Shutdown();
    
    // Build importance sampling tables
    BuildImportanceSamplingTables(luminance);
    
    if (!CreateSampler()) {
        return false;
    }
    
    m_Loaded = true;
    LUCENT_CORE_INFO("Created default sky environment");
    return true;
}

bool EnvironmentMap::BuildImportanceSamplingTables(const std::vector<float>& luminance) {
    // Build 2D CDF for importance sampling
    // Marginal CDF: P(row) - sum of each row
    // Conditional CDF: P(col | row) - cumulative sum within each row
    
    // Compute row sums (marginal)
    std::vector<float> rowSums(m_Height);
    for (uint32_t y = 0; y < m_Height; y++) {
        float sum = 0.0f;
        for (uint32_t x = 0; x < m_Width; x++) {
            sum += luminance[y * m_Width + x];
        }
        rowSums[y] = sum;
    }
    
    // Build marginal CDF (1D texture, height x 1)
    std::vector<float> marginalCDF(m_Height);
    float totalSum = 0.0f;
    for (uint32_t y = 0; y < m_Height; y++) {
        totalSum += rowSums[y];
        marginalCDF[y] = totalSum;
    }
    // Normalize
    if (totalSum > 0.0f) {
        for (uint32_t y = 0; y < m_Height; y++) {
            marginalCDF[y] /= totalSum;
        }
    }
    
    // Build conditional CDFs (2D texture, width x height)
    std::vector<float> conditionalCDF(m_Width * m_Height);
    for (uint32_t y = 0; y < m_Height; y++) {
        float sum = 0.0f;
        float rowTotal = rowSums[y];
        for (uint32_t x = 0; x < m_Width; x++) {
            sum += luminance[y * m_Width + x];
            conditionalCDF[y * m_Width + x] = (rowTotal > 0.0f) ? (sum / rowTotal) : (static_cast<float>(x + 1) / m_Width);
        }
    }
    
    // Create marginal CDF texture (R32_SFLOAT, height x 1)
    {
        BufferDesc stagingDesc{};
        stagingDesc.size = m_Height * sizeof(float);
        stagingDesc.usage = BufferUsage::Staging;
        stagingDesc.hostVisible = true;
        
        Buffer staging;
        if (!staging.Init(m_Device, stagingDesc)) return false;
        staging.Upload(marginalCDF.data(), stagingDesc.size);
        
        ImageDesc imgDesc{};
        imgDesc.width = m_Height;  // Store as 1D texture (width = height of env)
        imgDesc.height = 1;
        imgDesc.format = VK_FORMAT_R32_SFLOAT;
        imgDesc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgDesc.mipLevels = 1;
        imgDesc.debugName = "EnvMarginalCDF";
        
        if (!m_MarginalCDF.Init(m_Device, imgDesc)) {
            staging.Shutdown();
            return false;
        }
        
        VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
        m_MarginalCDF.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { m_Height, 1, 1 };
        
        vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_MarginalCDF.GetHandle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        m_MarginalCDF.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_Device->EndSingleTimeCommands(cmd);
        staging.Shutdown();
    }
    
    // Create conditional CDF texture (R32_SFLOAT, width x height)
    {
        BufferDesc stagingDesc{};
        stagingDesc.size = m_Width * m_Height * sizeof(float);
        stagingDesc.usage = BufferUsage::Staging;
        stagingDesc.hostVisible = true;
        
        Buffer staging;
        if (!staging.Init(m_Device, stagingDesc)) return false;
        staging.Upload(conditionalCDF.data(), stagingDesc.size);
        
        ImageDesc imgDesc{};
        imgDesc.width = m_Width;
        imgDesc.height = m_Height;
        imgDesc.format = VK_FORMAT_R32_SFLOAT;
        imgDesc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgDesc.mipLevels = 1;
        imgDesc.debugName = "EnvConditionalCDF";
        
        if (!m_ConditionalCDF.Init(m_Device, imgDesc)) {
            staging.Shutdown();
            return false;
        }
        
        VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
        m_ConditionalCDF.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { m_Width, m_Height, 1 };
        
        vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_ConditionalCDF.GetHandle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        m_ConditionalCDF.TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_Device->EndSingleTimeCommands(cmd);
        staging.Shutdown();
    }
    
    return true;
}

bool EnvironmentMap::CreateSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    
    VkDevice vkDevice = m_Device->GetContext()->GetDevice();
    if (vkCreateSampler(vkDevice, &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create environment map sampler");
        return false;
    }
    
    return true;
}

void EnvironmentMap::Shutdown() {
    if (m_Device && m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_Device->GetContext()->GetDevice(), m_Sampler, nullptr);
        m_Sampler = VK_NULL_HANDLE;
    }
    m_ConditionalCDF.Shutdown();
    m_MarginalCDF.Shutdown();
    m_EnvImage.Shutdown();
    m_Loaded = false;
}

} // namespace lucent::gfx

