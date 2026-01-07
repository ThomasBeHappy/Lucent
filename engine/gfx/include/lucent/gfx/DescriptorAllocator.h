#pragma once

#include "lucent/gfx/Device.h"
#include <vector>
#include <unordered_map>

namespace lucent::gfx {

struct DescriptorPoolSizes {
    std::vector<std::pair<VkDescriptorType, float>> sizes = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 0.5f },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4.0f },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4.0f },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1.0f },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1.0f },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1.0f },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2.0f },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2.0f },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1.0f },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1.0f },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 0.5f },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1.0f }
    };
};

class DescriptorAllocator : public NonCopyable {
public:
    DescriptorAllocator() = default;
    ~DescriptorAllocator();
    
    bool Init(Device* device, uint32_t maxSets = 1000);
    void Shutdown();
    
    // Allocate a descriptor set
    VkDescriptorSet Allocate(VkDescriptorSetLayout layout);
    
    // Reset all allocations (typically per-frame)
    void ResetPools();
    
private:
    VkDescriptorPool GrabPool();
    VkDescriptorPool CreatePool(uint32_t count, VkDescriptorPoolCreateFlags flags);
    
private:
    Device* m_Device = nullptr;
    
    VkDescriptorPool m_CurrentPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_UsedPools;
    std::vector<VkDescriptorPool> m_FreePools;
    
    DescriptorPoolSizes m_PoolSizes;
    uint32_t m_MaxSets = 1000;
};

// Helper class for building descriptor set layouts
class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder& AddBinding(uint32_t binding, VkDescriptorType type, 
                                        VkShaderStageFlags stages, uint32_t count = 1);
    
    VkDescriptorSetLayout Build(VkDevice device, VkDescriptorSetLayoutCreateFlags flags = 0);
    void Clear();
    
private:
    std::vector<VkDescriptorSetLayoutBinding> m_Bindings;
};

// Helper class for writing descriptor sets
class DescriptorWriter {
public:
    DescriptorWriter& WriteBuffer(uint32_t binding, VkBuffer buffer, size_t size, 
                                  size_t offset = 0, VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    DescriptorWriter& WriteImage(uint32_t binding, VkImageView view, VkSampler sampler,
                                 VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    
    void UpdateSet(VkDevice device, VkDescriptorSet set);
    void Clear();
    
private:
    std::vector<VkDescriptorBufferInfo> m_BufferInfos;
    std::vector<VkDescriptorImageInfo> m_ImageInfos;
    std::vector<VkWriteDescriptorSet> m_Writes;
};

} // namespace lucent::gfx

