#include "lucent/gfx/DescriptorAllocator.h"

namespace lucent::gfx {

DescriptorAllocator::~DescriptorAllocator() {
    Shutdown();
}

bool DescriptorAllocator::Init(Device* device, uint32_t maxSets) {
    m_Device = device;
    m_MaxSets = maxSets;
    return true;
}

void DescriptorAllocator::Shutdown() {
    if (!m_Device) return;
    
    VkDevice device = m_Device->GetHandle();
    
    for (auto pool : m_FreePools) {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    m_FreePools.clear();
    
    for (auto pool : m_UsedPools) {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    m_UsedPools.clear();
    
    m_CurrentPool = VK_NULL_HANDLE;
    m_Device = nullptr;
}

VkDescriptorSet DescriptorAllocator::Allocate(VkDescriptorSetLayout layout) {
    if (m_CurrentPool == VK_NULL_HANDLE) {
        m_CurrentPool = GrabPool();
        m_UsedPools.push_back(m_CurrentPool);
    }
    
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_CurrentPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    
    VkDescriptorSet set;
    VkResult result = vkAllocateDescriptorSets(m_Device->GetHandle(), &allocInfo, &set);
    
    if (result == VK_SUCCESS) {
        return set;
    }
    
    // Try again with new pool if allocation failed
    if (result == VK_ERROR_FRAGMENTED_POOL || result == VK_ERROR_OUT_OF_POOL_MEMORY) {
        m_CurrentPool = GrabPool();
        m_UsedPools.push_back(m_CurrentPool);
        
        allocInfo.descriptorPool = m_CurrentPool;
        result = vkAllocateDescriptorSets(m_Device->GetHandle(), &allocInfo, &set);
        
        if (result == VK_SUCCESS) {
            return set;
        }
    }
    
    LUCENT_CORE_ERROR("Failed to allocate descriptor set: {}", static_cast<int>(result));
    return VK_NULL_HANDLE;
}

void DescriptorAllocator::ResetPools() {
    VkDevice device = m_Device->GetHandle();
    
    for (auto pool : m_UsedPools) {
        vkResetDescriptorPool(device, pool, 0);
        m_FreePools.push_back(pool);
    }
    m_UsedPools.clear();
    m_CurrentPool = VK_NULL_HANDLE;
}

VkDescriptorPool DescriptorAllocator::GrabPool() {
    if (!m_FreePools.empty()) {
        VkDescriptorPool pool = m_FreePools.back();
        m_FreePools.pop_back();
        return pool;
    }
    
    return CreatePool(m_MaxSets, 0);
}

VkDescriptorPool DescriptorAllocator::CreatePool(uint32_t count, VkDescriptorPoolCreateFlags flags) {
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(m_PoolSizes.sizes.size());

    // Only include descriptor types that are actually enabled on this logical device.
    // In particular, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR requires enabling
    // VK_KHR_acceleration_structure at device creation time.
    const bool accelStructEnabled =
        m_Device &&
        m_Device->GetContext() &&
        m_Device->GetContext()->GetDeviceFeatures().accelerationStructure;
    
    for (const auto& [type, ratio] : m_PoolSizes.sizes) {
        if (type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR && !accelStructEnabled) {
            continue;
        }
        sizes.push_back({ type, static_cast<uint32_t>(ratio * count) });
    }
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = flags;
    poolInfo.maxSets = count;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(m_Device->GetHandle(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create descriptor pool");
        return VK_NULL_HANDLE;
    }
    
    return pool;
}

// DescriptorLayoutBuilder

DescriptorLayoutBuilder& DescriptorLayoutBuilder::AddBinding(uint32_t binding, VkDescriptorType type,
                                                              VkShaderStageFlags stages, uint32_t count) {
    VkDescriptorSetLayoutBinding layoutBinding{};
    layoutBinding.binding = binding;
    layoutBinding.descriptorType = type;
    layoutBinding.descriptorCount = count;
    layoutBinding.stageFlags = stages;
    layoutBinding.pImmutableSamplers = nullptr;
    
    m_Bindings.push_back(layoutBinding);
    return *this;
}

VkDescriptorSetLayout DescriptorLayoutBuilder::Build(VkDevice device, VkDescriptorSetLayoutCreateFlags flags) {
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags = flags;
    layoutInfo.bindingCount = static_cast<uint32_t>(m_Bindings.size());
    layoutInfo.pBindings = m_Bindings.data();
    
    VkDescriptorSetLayout layout;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create descriptor set layout");
        return VK_NULL_HANDLE;
    }
    
    return layout;
}

void DescriptorLayoutBuilder::Clear() {
    m_Bindings.clear();
}

// DescriptorWriter

DescriptorWriter& DescriptorWriter::WriteBuffer(uint32_t binding, VkBuffer buffer, size_t size,
                                                 size_t offset, VkDescriptorType type) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = size;
    
    m_BufferInfos.push_back(bufferInfo);
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &m_BufferInfos.back();
    
    m_Writes.push_back(write);
    return *this;
}

DescriptorWriter& DescriptorWriter::WriteImage(uint32_t binding, VkImageView view, VkSampler sampler,
                                                VkImageLayout layout, VkDescriptorType type) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = view;
    imageInfo.imageLayout = layout;
    
    m_ImageInfos.push_back(imageInfo);
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = &m_ImageInfos.back();
    
    m_Writes.push_back(write);
    return *this;
}

void DescriptorWriter::UpdateSet(VkDevice device, VkDescriptorSet set) {
    for (auto& write : m_Writes) {
        write.dstSet = set;
    }
    
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(m_Writes.size()), m_Writes.data(), 0, nullptr);
}

void DescriptorWriter::Clear() {
    m_BufferInfos.clear();
    m_ImageInfos.clear();
    m_Writes.clear();
}

} // namespace lucent::gfx

