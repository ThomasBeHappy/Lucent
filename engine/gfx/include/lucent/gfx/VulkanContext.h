#pragma once

#include "lucent/core/Core.h"
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <string>

namespace lucent::gfx {

struct VulkanContextConfig {
    const char* appName = "Lucent Editor";
    uint32_t appVersion = VK_MAKE_VERSION(0, 1, 0);
    bool enableValidation = true;
    bool enableRayTracing = true;  // Probe for RT, don't require
    // Optional: prefer a specific GPU by (substring) name match. Requires restart to change.
    const char* preferredDeviceName = nullptr;
};

struct QueueFamilyIndices {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;
    uint32_t compute = UINT32_MAX;
    uint32_t transfer = UINT32_MAX;
    
    bool IsComplete() const {
        return graphics != UINT32_MAX && present != UINT32_MAX;
    }
};

struct DeviceFeatures {
    // Core features (Vulkan 1.2+)
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    
    // Vulkan 1.3 features (optional - fallback available)
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool maintenance4 = false;
    
    // Ray tracing features (optional)
    bool rayTracingPipeline = false;
    bool accelerationStructure = false;
    bool rayQuery = false;
    
    // RT properties
    uint32_t maxRayRecursionDepth = 0;
    uint32_t shaderGroupHandleSize = 0;
    uint32_t shaderGroupBaseAlignment = 0;
    
    // Helper to check if we have Vulkan 1.3 level features
    bool HasVulkan13Features() const {
        return dynamicRendering && synchronization2;
    }
};

class VulkanContext : public NonCopyable {
public:
    VulkanContext() = default;
    ~VulkanContext();
    
    bool Init(const VulkanContextConfig& config, GLFWwindow* window);
    void Shutdown();
    
    // Getters
    VkInstance GetInstance() const { return m_Instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice GetDevice() const { return m_Device; }
    VkSurfaceKHR GetSurface() const { return m_Surface; }
    
    VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
    VkQueue GetPresentQueue() const { return m_PresentQueue; }
    VkQueue GetComputeQueue() const { return m_ComputeQueue; }
    VkQueue GetTransferQueue() const { return m_TransferQueue; }
    
    const QueueFamilyIndices& GetQueueFamilies() const { return m_QueueFamilies; }
    const DeviceFeatures& GetDeviceFeatures() const { return m_DeviceFeatures; }
    
    bool IsRayTracingSupported() const { return m_DeviceFeatures.rayTracingPipeline; }
    
    // Utility
    void WaitIdle() const;
    
private:
    bool CreateInstance(const VulkanContextConfig& config);
    bool SetupDebugMessenger();
    bool CreateSurface(GLFWwindow* window);
    bool SelectPhysicalDevice(const VulkanContextConfig& config);
    bool CreateLogicalDevice(const VulkanContextConfig& config);
    
    QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
    bool CheckDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*>& extensions) const;
    void QueryDeviceFeatures(VkPhysicalDevice device, DeviceFeatures& features) const;
    int RateDeviceSuitability(VkPhysicalDevice device, const VulkanContextConfig& config) const;
    
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData);

private:
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    
    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    VkQueue m_PresentQueue = VK_NULL_HANDLE;
    VkQueue m_ComputeQueue = VK_NULL_HANDLE;
    VkQueue m_TransferQueue = VK_NULL_HANDLE;
    
    QueueFamilyIndices m_QueueFamilies;
    DeviceFeatures m_DeviceFeatures;
    
    bool m_ValidationEnabled = false;
};

} // namespace lucent::gfx

