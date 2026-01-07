#pragma once

#include "lucent/gfx/Device.h"

namespace lucent::gfx {

enum class BufferUsage {
    Vertex,
    Index,
    Uniform,
    Storage,
    Staging,
    AccelerationStructure,
    ShaderBindingTable
};

struct BufferDesc {
    size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    bool hostVisible = false;
    bool deviceAddress = false;
    const char* debugName = nullptr;
};

class Buffer : public NonCopyable {
public:
    Buffer() = default;
    ~Buffer();
    
    bool Init(Device* device, const BufferDesc& desc);
    void Shutdown();
    
    // Data operations
    void Upload(const void* data, size_t size, size_t offset = 0);
    void* Map();
    void Unmap();
    
    // Getters
    VkBuffer GetHandle() const { return m_Buffer; }
    VkDeviceMemory GetMemory() const { return m_Memory; }
    size_t GetSize() const { return m_Size; }
    VkDeviceAddress GetDeviceAddress() const { return m_DeviceAddress; }
    
private:
    Device* m_Device = nullptr;
    
    VkBuffer m_Buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    size_t m_Size = 0;
    VkDeviceAddress m_DeviceAddress = 0;
    
    bool m_HostVisible = false;
    void* m_MappedData = nullptr;
};

} // namespace lucent::gfx

