#include "lucent/gfx/Buffer.h"
#include "lucent/gfx/DebugUtils.h"

namespace lucent::gfx {

Buffer::~Buffer() {
    Shutdown();
}

bool Buffer::Init(Device* device, const BufferDesc& desc) {
    m_Device = device;
    m_Size = desc.size;
    m_HostVisible = desc.hostVisible;
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    // Set usage flags based on buffer type
    switch (desc.usage) {
        case BufferUsage::Vertex:
            bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        case BufferUsage::Index:
            bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        case BufferUsage::Uniform:
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        case BufferUsage::Storage:
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            break;
        case BufferUsage::Staging:
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            break;
        case BufferUsage::AccelerationStructure:
            // Ray tracing buffers often serve multiple purposes:
            // - AS storage (BLAS/TLAS buffers)
            // - AS build input (vertex/index/instance buffers)
            // - Shader-readable data (bound as storage buffers in shaders)
            //
            // In this codebase BufferUsage::AccelerationStructure is only used by TracerRayKHR,
            // so we include all required flags here.
            bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            break;
        case BufferUsage::ShaderBindingTable:
            bufferInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            break;
    }
    
    if (desc.deviceAddress) {
        bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    
    VkDevice vkDevice = device->GetHandle();
    
    if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &m_Buffer) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create buffer");
        return false;
    }
    
    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(vkDevice, m_Buffer, &memRequirements);
    
    // Allocate memory
    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    if (desc.deviceAddress) {
        allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    }
    
    VkMemoryPropertyFlags memProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc.hostVisible) {
        memProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = desc.deviceAddress ? &allocFlags : nullptr;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = device->FindMemoryType(memRequirements.memoryTypeBits, memProps);
    
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        LUCENT_CORE_ERROR("Failed to find suitable memory type for buffer");
        return false;
    }
    
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &m_Memory) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to allocate buffer memory");
        return false;
    }
    
    vkBindBufferMemory(vkDevice, m_Buffer, m_Memory, 0);
    
    // Get device address if requested
    if (desc.deviceAddress) {
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(vkDevice, &addressInfo);
    }
    
    // Set debug name
    if (desc.debugName) {
        DebugUtils::SetObjectName(vkDevice, m_Buffer, VK_OBJECT_TYPE_BUFFER, desc.debugName);
    }
    
    return true;
}

void Buffer::Shutdown() {
    if (!m_Device) return;
    
    VkDevice device = m_Device->GetHandle();
    
    if (m_MappedData) {
        Unmap();
    }
    
    if (m_Buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, m_Buffer, nullptr);
        m_Buffer = VK_NULL_HANDLE;
    }
    
    if (m_Memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, m_Memory, nullptr);
        m_Memory = VK_NULL_HANDLE;
    }
    
    m_Device = nullptr;
}

void Buffer::Upload(const void* data, size_t size, size_t offset) {
    LUCENT_CORE_ASSERT(m_HostVisible, "Cannot upload to non-host-visible buffer");
    LUCENT_CORE_ASSERT(offset + size <= m_Size, "Buffer upload exceeds buffer size");
    
    void* mapped = Map();
    memcpy(static_cast<char*>(mapped) + offset, data, size);
    Unmap();
}

void* Buffer::Map() {
    if (m_MappedData) {
        return m_MappedData;
    }
    
    LUCENT_CORE_ASSERT(m_HostVisible, "Cannot map non-host-visible buffer");
    
    vkMapMemory(m_Device->GetHandle(), m_Memory, 0, m_Size, 0, &m_MappedData);
    return m_MappedData;
}

void Buffer::Unmap() {
    if (m_MappedData) {
        vkUnmapMemory(m_Device->GetHandle(), m_Memory);
        m_MappedData = nullptr;
    }
}

} // namespace lucent::gfx

