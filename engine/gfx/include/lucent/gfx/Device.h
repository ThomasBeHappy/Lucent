#pragma once

#include "lucent/gfx/VulkanContext.h"
#include <functional>

namespace lucent::gfx {

// Forward declarations
class Buffer;
class Image;

class Device : public NonCopyable {
public:
    Device() = default;
    ~Device();
    
    bool Init(VulkanContext* context);
    void Shutdown();
    
    // Command pool management
    VkCommandPool GetGraphicsCommandPool() const { return m_GraphicsCommandPool; }
    VkCommandPool GetTransferCommandPool() const { return m_TransferCommandPool; }
    
    // Single-time command buffer utilities
    VkCommandBuffer BeginSingleTimeCommands(VkCommandPool pool = VK_NULL_HANDLE);
    void EndSingleTimeCommands(VkCommandBuffer commandBuffer, VkCommandPool pool = VK_NULL_HANDLE);
    
    // Immediate submit for quick GPU operations
    void ImmediateSubmit(std::function<void(VkCommandBuffer)>&& function);
    
    // Memory allocation
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    
    // Context access
    VulkanContext* GetContext() const { return m_Context; }
    VkDevice GetHandle() const { return m_Context->GetDevice(); }
    VkPhysicalDevice GetPhysicalDevice() const { return m_Context->GetPhysicalDevice(); }
    
private:
    VulkanContext* m_Context = nullptr;
    
    VkCommandPool m_GraphicsCommandPool = VK_NULL_HANDLE;
    VkCommandPool m_TransferCommandPool = VK_NULL_HANDLE;
    
    // Immediate submit resources
    VkFence m_ImmediateFence = VK_NULL_HANDLE;
    VkCommandBuffer m_ImmediateCommandBuffer = VK_NULL_HANDLE;
};

} // namespace lucent::gfx

