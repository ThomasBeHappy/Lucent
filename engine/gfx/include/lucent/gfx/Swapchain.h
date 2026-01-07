#pragma once

#include "lucent/gfx/VulkanContext.h"
#include <vector>

namespace lucent::gfx {

struct SwapchainConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    uint32_t framesInFlight = 2;
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class Swapchain : public NonCopyable {
public:
    Swapchain() = default;
    ~Swapchain();
    
    bool Init(VulkanContext* context, const SwapchainConfig& config);
    void Shutdown();
    
    // Recreate swapchain (on resize, etc.)
    bool Recreate(uint32_t width, uint32_t height);
    
    // Frame operations
    bool AcquireNextImage(VkSemaphore signalSemaphore, uint32_t& imageIndex);
    bool Present(VkSemaphore waitSemaphore, uint32_t imageIndex);
    
    // Getters
    VkSwapchainKHR GetHandle() const { return m_Swapchain; }
    VkFormat GetFormat() const { return m_Format; }
    VkExtent2D GetExtent() const { return m_Extent; }
    uint32_t GetImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
    
    VkImage GetImage(uint32_t index) const { return m_Images[index]; }
    VkImageView GetImageView(uint32_t index) const { return m_ImageViews[index]; }
    
    bool NeedsRecreate() const { return m_NeedsRecreate; }
    void SetNeedsRecreate() { m_NeedsRecreate = true; }
    
    static SwapchainSupportDetails QuerySupport(VkPhysicalDevice device, VkSurfaceKHR surface);
    
private:
    bool CreateSwapchain();
    bool CreateImageViews();
    void DestroySwapchain();
    
    VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    
private:
    VulkanContext* m_Context = nullptr;
    SwapchainConfig m_Config;
    
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_Format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_Extent = { 0, 0 };
    
    std::vector<VkImage> m_Images;
    std::vector<VkImageView> m_ImageViews;
    
    bool m_NeedsRecreate = false;
    bool m_Vsync = true;
};

} // namespace lucent::gfx

