#pragma once

#include "lucent/core/Core.h"
#include "lucent/gfx/Buffer.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace lucent::assets {

// Vertex format for mesh rendering
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness
    
    static std::vector<VkVertexInputBindingDescription> GetBindingDescriptions();
    static std::vector<VkVertexInputAttributeDescription> GetAttributeDescriptions();
};

// Submesh for multi-material meshes
struct Submesh {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
};

// Bounding volumes
struct AABB {
    glm::vec3 min = glm::vec3(FLT_MAX);
    glm::vec3 max = glm::vec3(-FLT_MAX);
    
    void Expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    
    glm::vec3 GetCenter() const { return (min + max) * 0.5f; }
    glm::vec3 GetExtents() const { return (max - min) * 0.5f; }
};

class Mesh : public NonCopyable {
public:
    Mesh() = default;
    ~Mesh();
    
    // Create from vertex/index data
    bool Create(gfx::Device* device, 
                const std::vector<Vertex>& vertices, 
                const std::vector<uint32_t>& indices,
                const std::string& name = "Mesh");
    
    void Destroy();
    
    // Bind for rendering
    void Bind(VkCommandBuffer cmd) const;
    void Draw(VkCommandBuffer cmd, uint32_t instanceCount = 1) const;
    
    // Submesh support
    void AddSubmesh(uint32_t indexOffset, uint32_t indexCount, uint32_t materialIndex = 0);
    const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
    void DrawSubmesh(VkCommandBuffer cmd, uint32_t submeshIndex, uint32_t instanceCount = 1) const;
    
    // Getters
    uint32_t GetVertexCount() const { return m_VertexCount; }
    uint32_t GetIndexCount() const { return m_IndexCount; }
    const AABB& GetBounds() const { return m_Bounds; }
    const std::string& GetName() const { return m_Name; }
    
    gfx::Buffer* GetVertexBuffer() { return &m_VertexBuffer; }
    gfx::Buffer* GetIndexBuffer() { return &m_IndexBuffer; }
    
    // CPU-side geometry access (for path tracing)
    const std::vector<Vertex>& GetCPUVertices() const { return m_CPUVertices; }
    const std::vector<uint32_t>& GetCPUIndices() const { return m_CPUIndices; }
    
private:
    gfx::Device* m_Device = nullptr;
    
    gfx::Buffer m_VertexBuffer;
    gfx::Buffer m_IndexBuffer;
    
    uint32_t m_VertexCount = 0;
    uint32_t m_IndexCount = 0;
    
    std::vector<Submesh> m_Submeshes;
    AABB m_Bounds;
    std::string m_Name;
    
    // CPU-side copies for path tracing
    std::vector<Vertex> m_CPUVertices;
    std::vector<uint32_t> m_CPUIndices;
};

// Primitive mesh generators
namespace Primitives {
    void GenerateCube(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float size = 1.0f);
    void GenerateSphere(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16);
    void GeneratePlane(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float width = 1.0f, float height = 1.0f, uint32_t subdivisions = 1);
    void GenerateCylinder(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);
    void GenerateCone(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);
}

} // namespace lucent::assets

