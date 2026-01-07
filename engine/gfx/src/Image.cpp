#include "lucent/gfx/Image.h"
#include "lucent/gfx/DebugUtils.h"

namespace lucent::gfx {

Image::~Image() {
    Shutdown();
}

bool Image::Init(Device* device, const ImageDesc& desc) {
    m_Device = device;
    m_Format = desc.format;
    m_Extent = { desc.width, desc.height, desc.depth };
    m_Aspect = desc.aspect;
    m_MipLevels = desc.mipLevels;
    m_ArrayLayers = desc.arrayLayers;
    
    VkDevice vkDevice = device->GetHandle();
    
    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = desc.format;
    imageInfo.extent = m_Extent;
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = desc.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (desc.isCubemap) {
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &m_Image) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create image");
        return false;
    }
    
    // Allocate memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(vkDevice, m_Image, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device->FindMemoryType(
        memRequirements.memoryTypeBits, 
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        LUCENT_CORE_ERROR("Failed to find suitable memory type for image");
        return false;
    }
    
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &m_Memory) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to allocate image memory");
        return false;
    }
    
    vkBindImageMemory(vkDevice, m_Image, m_Memory, 0);
    
    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_Image;
    
    if (desc.isCubemap) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    } else if (desc.arrayLayers > 1) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    } else if (desc.depth > 1) {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    } else {
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    }
    
    viewInfo.format = desc.format;
    viewInfo.subresourceRange.aspectMask = desc.aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;
    
    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create image view");
        return false;
    }
    
    // Set debug names
    if (desc.debugName) {
        DebugUtils::SetObjectName(vkDevice, m_Image, VK_OBJECT_TYPE_IMAGE, desc.debugName);
        std::string viewName = std::string(desc.debugName) + "_view";
        DebugUtils::SetObjectName(vkDevice, m_ImageView, VK_OBJECT_TYPE_IMAGE_VIEW, viewName.c_str());
    }
    
    return true;
}

void Image::Shutdown() {
    if (!m_Device) return;
    
    VkDevice device = m_Device->GetHandle();
    
    if (m_ImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_ImageView, nullptr);
        m_ImageView = VK_NULL_HANDLE;
    }
    
    if (m_Image != VK_NULL_HANDLE) {
        vkDestroyImage(device, m_Image, nullptr);
        m_Image = VK_NULL_HANDLE;
    }
    
    if (m_Memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_Memory, nullptr);
        m_Memory = VK_NULL_HANDLE;
    }
    
    m_Device = nullptr;
}

void Image::TransitionLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) {
    // Check if synchronization2 is available
    bool useSync2 = m_Device->GetContext()->GetDeviceFeatures().synchronization2;
    
    if (useSync2) {
        // Vulkan 1.3 path - use VkImageMemoryBarrier2
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_Image;
        barrier.subresourceRange.aspectMask = m_Aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = m_MipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = m_ArrayLayers;
        
        // Source stage and access mask
        switch (oldLayout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                barrier.srcAccessMask = 0;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
            default:
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
        }
        
        // Destination stage and access mask
        switch (newLayout) {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
                barrier.dstAccessMask = 0;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
            default:
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
        }
        
        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependencyInfo.imageMemoryBarrierCount = 1;
        dependencyInfo.pImageMemoryBarriers = &barrier;
        
        vkCmdPipelineBarrier2(cmd, &dependencyInfo);
    } else {
        // Vulkan 1.1/1.2 fallback - use legacy VkImageMemoryBarrier
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_Image;
        barrier.subresourceRange.aspectMask = m_Aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = m_MipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = m_ArrayLayers;
        
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        
        // Source stage and access mask
        switch (oldLayout) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                barrier.srcAccessMask = 0;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                break;
            default:
                srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                break;
        }
        
        // Destination stage and access mask
        switch (newLayout) {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
                barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
                barrier.dstAccessMask = 0;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                break;
            default:
                dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                break;
        }
        
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    
    m_CurrentLayout = newLayout;
}

} // namespace lucent::gfx

