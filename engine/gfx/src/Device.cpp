#include "lucent/gfx/Device.h"
#include "lucent/gfx/VkResultUtils.h"

namespace lucent::gfx {

Device::~Device() {
    Shutdown();
}

bool Device::Init(VulkanContext* context) {
    m_Context = context;
    VkDevice device = context->GetDevice();
    
    // Create graphics command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context->GetQueueFamilies().graphics;
    
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_GraphicsCommandPool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create graphics command pool");
        return false;
    }
    
    // Create transfer command pool
    poolInfo.queueFamilyIndex = context->GetQueueFamilies().transfer != UINT32_MAX 
        ? context->GetQueueFamilies().transfer 
        : context->GetQueueFamilies().graphics;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_TransferCommandPool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create transfer command pool");
        return false;
    }
    
    // Create immediate submit resources
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    
    if (vkCreateFence(device, &fenceInfo, nullptr, &m_ImmediateFence) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create immediate fence");
        return false;
    }
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_GraphicsCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    
    if (vkAllocateCommandBuffers(device, &allocInfo, &m_ImmediateCommandBuffer) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to allocate immediate command buffer");
        return false;
    }
    
    LUCENT_CORE_DEBUG("Device resources initialized");
    return true;
}

void Device::Shutdown() {
    if (!m_Context) return;
    
    VkDevice device = m_Context->GetDevice();
    
    if (m_ImmediateFence != VK_NULL_HANDLE) {
        vkDestroyFence(device, m_ImmediateFence, nullptr);
        m_ImmediateFence = VK_NULL_HANDLE;
    }
    
    if (m_TransferCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, m_TransferCommandPool, nullptr);
        m_TransferCommandPool = VK_NULL_HANDLE;
    }
    
    if (m_GraphicsCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, m_GraphicsCommandPool, nullptr);
        m_GraphicsCommandPool = VK_NULL_HANDLE;
    }
    
    m_Context = nullptr;
}

VkCommandBuffer Device::BeginSingleTimeCommands(VkCommandPool pool) {
    if (pool == VK_NULL_HANDLE) {
        pool = m_GraphicsCommandPool;
    }
    
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_Context->GetDevice(), &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    
    return commandBuffer;
}

void Device::EndSingleTimeCommands(VkCommandBuffer commandBuffer, VkCommandPool pool) {
    if (pool == VK_NULL_HANDLE) {
        pool = m_GraphicsCommandPool;
    }
    
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    VkResult submitRes = vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    if (submitRes != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Device::EndSingleTimeCommands vkQueueSubmit failed: {} ({})",
            VkResultToString(submitRes), static_cast<int>(submitRes));
    }
    VkResult idleRes = vkQueueWaitIdle(m_Context->GetGraphicsQueue());
    if (idleRes != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Device::EndSingleTimeCommands vkQueueWaitIdle failed: {} ({})",
            VkResultToString(idleRes), static_cast<int>(idleRes));
    }
    
    vkFreeCommandBuffers(m_Context->GetDevice(), pool, 1, &commandBuffer);
}

void Device::ImmediateSubmit(std::function<void(VkCommandBuffer)>&& function) {
    VkDevice device = m_Context->GetDevice();
    
    // Wait for previous immediate submit to complete
    vkWaitForFences(device, 1, &m_ImmediateFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &m_ImmediateFence);
    
    vkResetCommandBuffer(m_ImmediateCommandBuffer, 0);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    vkBeginCommandBuffer(m_ImmediateCommandBuffer, &beginInfo);
    
    function(m_ImmediateCommandBuffer);
    
    vkEndCommandBuffer(m_ImmediateCommandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_ImmediateCommandBuffer;
    
    VkResult submitRes = vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, m_ImmediateFence);
    if (submitRes != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Device::ImmediateSubmit vkQueueSubmit failed: {} ({})",
            VkResultToString(submitRes), static_cast<int>(submitRes));
    }
}

uint32_t Device::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_Context->GetPhysicalDevice(), &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    
    LUCENT_CORE_ERROR("Failed to find suitable memory type");
    return UINT32_MAX;
}

} // namespace lucent::gfx

