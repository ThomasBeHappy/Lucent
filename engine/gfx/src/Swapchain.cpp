#include "lucent/gfx/Swapchain.h"
#include "lucent/gfx/VkResultUtils.h"
#include <algorithm>

namespace lucent::gfx {

Swapchain::~Swapchain() {
    Shutdown();
}

bool Swapchain::Init(VulkanContext* context, const SwapchainConfig& config) {
    m_Context = context;
    m_Config = config;
    m_Vsync = config.vsync;
    
    return CreateSwapchain() && CreateImageViews();
}

void Swapchain::Shutdown() {
    DestroySwapchain();
    m_Context = nullptr;
}

bool Swapchain::Recreate(uint32_t width, uint32_t height) {
    // Check if new extent is valid before destroying old swapchain
    if (width == 0 || height == 0) {
        m_NeedsRecreate = true;
        return false;
    }
    
    m_Config.width = width;
    m_Config.height = height;
    
    m_Context->WaitIdle();
    DestroySwapchain();
    
    if (!CreateSwapchain() || !CreateImageViews()) {
        return false;
    }
    
    m_NeedsRecreate = false;
    return true;
}

bool Swapchain::AcquireNextImage(VkSemaphore signalSemaphore, uint32_t& imageIndex) {
    VkResult result = vkAcquireNextImageKHR(
        m_Context->GetDevice(),
        m_Swapchain,
        UINT64_MAX,
        signalSemaphore,
        VK_NULL_HANDLE,
        &imageIndex
    );
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_NOT_READY) {
        m_NeedsRecreate = true;
        return false;
    }
    
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        LUCENT_CORE_ERROR("vkAcquireNextImageKHR failed: {} ({})", VkResultToString(result), static_cast<int>(result));
        m_NeedsRecreate = true; // Try recreating on any failure
        return false;
    }
    
    if (result == VK_SUBOPTIMAL_KHR) {
        m_NeedsRecreate = true;
    }
    
    return true;
}

bool Swapchain::Present(VkSemaphore waitSemaphore, uint32_t imageIndex) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &imageIndex;
    
    VkResult result = vkQueuePresentKHR(m_Context->GetPresentQueue(), &presentInfo);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_NeedsRecreate = true;
        return true; // Not a fatal error
    }
    
    if (result != VK_SUCCESS) {
        LUCENT_CORE_ERROR("vkQueuePresentKHR failed: {} ({})", VkResultToString(result), static_cast<int>(result));
        return false;
    }
    
    return true;
}

SwapchainSupportDetails Swapchain::QuerySupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupportDetails details;
    
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
    
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount > 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }
    
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }
    
    return details;
}

bool Swapchain::CreateSwapchain() {
    SwapchainSupportDetails support = QuerySupport(m_Context->GetPhysicalDevice(), m_Context->GetSurface());
    
    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
    VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
    VkExtent2D extent = ChooseExtent(support.capabilities);
    
    // Don't create swapchain with zero extent (e.g., window minimized)
    if (extent.width == 0 || extent.height == 0) {
        m_NeedsRecreate = true;
        return false;
    }
    
    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }
    
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_Context->GetSurface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    
    const auto& queueFamilies = m_Context->GetQueueFamilies();
    uint32_t queueFamilyIndices[] = { queueFamilies.graphics, queueFamilies.present };
    
    if (queueFamilies.graphics != queueFamilies.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    
    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;
    
    VkResult result = vkCreateSwapchainKHR(m_Context->GetDevice(), &createInfo, nullptr, &m_Swapchain);
    if (result != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create swapchain: {}", static_cast<int>(result));
        return false;
    }
    
    // Get swapchain images
    vkGetSwapchainImagesKHR(m_Context->GetDevice(), m_Swapchain, &imageCount, nullptr);
    m_Images.resize(imageCount);
    vkGetSwapchainImagesKHR(m_Context->GetDevice(), m_Swapchain, &imageCount, m_Images.data());
    
    m_Format = surfaceFormat.format;
    m_Extent = extent;
    
    LUCENT_CORE_INFO("Swapchain created: {}x{}, {} images, format {}", 
        extent.width, extent.height, imageCount, static_cast<int>(m_Format));
    
    return true;
}

bool Swapchain::CreateImageViews() {
    m_ImageViews.resize(m_Images.size());
    
    for (size_t i = 0; i < m_Images.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_Format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        
        if (vkCreateImageView(m_Context->GetDevice(), &viewInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create swapchain image view {}", i);
            return false;
        }
    }
    
    return true;
}

void Swapchain::DestroySwapchain() {
    if (!m_Context) return;
    
    VkDevice device = m_Context->GetDevice();
    
    for (auto imageView : m_ImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
    }
    m_ImageViews.clear();
    m_Images.clear();
    
    if (m_Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
}

VkSurfaceFormatKHR Swapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    // Prefer SRGB
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && 
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    
    // Fallback to first available
    return formats[0];
}

VkPresentModeKHR Swapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    if (!m_Vsync) {
        // Prefer mailbox (triple buffering) for low latency
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        
        // Try immediate if mailbox not available
        for (const auto& mode : modes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                return mode;
            }
        }
    }
    
    // FIFO is guaranteed to be available (vsync)
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    
    VkExtent2D actualExtent = { m_Config.width, m_Config.height };
    
    actualExtent.width = std::clamp(actualExtent.width, 
        capabilities.minImageExtent.width, 
        capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height, 
        capabilities.minImageExtent.height, 
        capabilities.maxImageExtent.height);
    
    return actualExtent;
}

} // namespace lucent::gfx

