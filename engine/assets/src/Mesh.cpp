#include "lucent/assets/Mesh.h"
#include "lucent/core/Log.h"
#include <cmath>

namespace lucent::assets {

// Vertex input descriptions
std::vector<VkVertexInputBindingDescription> Vertex::GetBindingDescriptions() {
    return {
        { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX }
    };
}

std::vector<VkVertexInputAttributeDescription> Vertex::GetAttributeDescriptions() {
    return {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv) },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent) }
    };
}

Mesh::~Mesh() {
    Destroy();
}

bool Mesh::Create(gfx::Device* device, 
                  const std::vector<Vertex>& vertices, 
                  const std::vector<uint32_t>& indices,
                  const std::string& name) {
    m_Device = device;
    m_Name = name;
    m_VertexCount = static_cast<uint32_t>(vertices.size());
    m_IndexCount = static_cast<uint32_t>(indices.size());
    
    // Calculate bounds
    m_Bounds = AABB();
    for (const auto& v : vertices) {
        m_Bounds.Expand(v.position);
    }
    
    // Create vertex buffer
    gfx::BufferDesc vbDesc{};
    vbDesc.size = vertices.size() * sizeof(Vertex);
    vbDesc.usage = gfx::BufferUsage::Vertex;
    vbDesc.hostVisible = true; // For simplicity; use staging buffer in production
    vbDesc.debugName = (name + "_VB").c_str();
    
    if (!m_VertexBuffer.Init(device, vbDesc)) {
        LUCENT_CORE_ERROR("Failed to create vertex buffer for mesh: {}", name);
        return false;
    }
    m_VertexBuffer.Upload(vertices.data(), vbDesc.size);
    
    // Create index buffer
    gfx::BufferDesc ibDesc{};
    ibDesc.size = indices.size() * sizeof(uint32_t);
    ibDesc.usage = gfx::BufferUsage::Index;
    ibDesc.hostVisible = true;
    ibDesc.debugName = (name + "_IB").c_str();
    
    if (!m_IndexBuffer.Init(device, ibDesc)) {
        LUCENT_CORE_ERROR("Failed to create index buffer for mesh: {}", name);
        return false;
    }
    m_IndexBuffer.Upload(indices.data(), ibDesc.size);
    
    // Default submesh covering entire mesh
    if (m_Submeshes.empty()) {
        AddSubmesh(0, m_IndexCount, 0);
    }
    
    LUCENT_CORE_DEBUG("Created mesh '{}': {} vertices, {} indices", name, m_VertexCount, m_IndexCount);
    return true;
}

void Mesh::Destroy() {
    m_IndexBuffer.Shutdown();
    m_VertexBuffer.Shutdown();
    m_Submeshes.clear();
    m_VertexCount = 0;
    m_IndexCount = 0;
}

void Mesh::Bind(VkCommandBuffer cmd) const {
    VkBuffer vertexBuffers[] = { m_VertexBuffer.GetHandle() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_IndexBuffer.GetHandle(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::Draw(VkCommandBuffer cmd, uint32_t instanceCount) const {
    vkCmdDrawIndexed(cmd, m_IndexCount, instanceCount, 0, 0, 0);
}

void Mesh::AddSubmesh(uint32_t indexOffset, uint32_t indexCount, uint32_t materialIndex) {
    m_Submeshes.push_back({ indexOffset, indexCount, materialIndex });
}

void Mesh::DrawSubmesh(VkCommandBuffer cmd, uint32_t submeshIndex, uint32_t instanceCount) const {
    if (submeshIndex >= m_Submeshes.size()) return;
    const auto& submesh = m_Submeshes[submeshIndex];
    vkCmdDrawIndexed(cmd, submesh.indexCount, instanceCount, submesh.indexOffset, 0, 0);
}

// ============================================================================
// Primitive Generators
// ============================================================================

namespace Primitives {

void GenerateCube(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, float size) {
    outVertices.clear();
    outIndices.clear();
    
    float h = size * 0.5f;
    
    // 6 faces, 4 vertices each (for proper normals)
    // Front face (Z+)
    outVertices.push_back({ {-h, -h, h}, {0, 0, 1}, {0, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h, -h, h}, {0, 0, 1}, {1, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h,  h, h}, {0, 0, 1}, {1, 1}, {1, 0, 0, 1} });
    outVertices.push_back({ {-h,  h, h}, {0, 0, 1}, {0, 1}, {1, 0, 0, 1} });
    
    // Back face (Z-)
    outVertices.push_back({ { h, -h, -h}, {0, 0, -1}, {0, 0}, {-1, 0, 0, 1} });
    outVertices.push_back({ {-h, -h, -h}, {0, 0, -1}, {1, 0}, {-1, 0, 0, 1} });
    outVertices.push_back({ {-h,  h, -h}, {0, 0, -1}, {1, 1}, {-1, 0, 0, 1} });
    outVertices.push_back({ { h,  h, -h}, {0, 0, -1}, {0, 1}, {-1, 0, 0, 1} });
    
    // Top face (Y+)
    outVertices.push_back({ {-h, h,  h}, {0, 1, 0}, {0, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h, h,  h}, {0, 1, 0}, {1, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h, h, -h}, {0, 1, 0}, {1, 1}, {1, 0, 0, 1} });
    outVertices.push_back({ {-h, h, -h}, {0, 1, 0}, {0, 1}, {1, 0, 0, 1} });
    
    // Bottom face (Y-)
    outVertices.push_back({ {-h, -h, -h}, {0, -1, 0}, {0, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h, -h, -h}, {0, -1, 0}, {1, 0}, {1, 0, 0, 1} });
    outVertices.push_back({ { h, -h,  h}, {0, -1, 0}, {1, 1}, {1, 0, 0, 1} });
    outVertices.push_back({ {-h, -h,  h}, {0, -1, 0}, {0, 1}, {1, 0, 0, 1} });
    
    // Right face (X+)
    outVertices.push_back({ {h, -h,  h}, {1, 0, 0}, {0, 0}, {0, 0, -1, 1} });
    outVertices.push_back({ {h, -h, -h}, {1, 0, 0}, {1, 0}, {0, 0, -1, 1} });
    outVertices.push_back({ {h,  h, -h}, {1, 0, 0}, {1, 1}, {0, 0, -1, 1} });
    outVertices.push_back({ {h,  h,  h}, {1, 0, 0}, {0, 1}, {0, 0, -1, 1} });
    
    // Left face (X-)
    outVertices.push_back({ {-h, -h, -h}, {-1, 0, 0}, {0, 0}, {0, 0, 1, 1} });
    outVertices.push_back({ {-h, -h,  h}, {-1, 0, 0}, {1, 0}, {0, 0, 1, 1} });
    outVertices.push_back({ {-h,  h,  h}, {-1, 0, 0}, {1, 1}, {0, 0, 1, 1} });
    outVertices.push_back({ {-h,  h, -h}, {-1, 0, 0}, {0, 1}, {0, 0, 1, 1} });
    
    // Indices (2 triangles per face) - counter-clockwise winding
    for (uint32_t face = 0; face < 6; face++) {
        uint32_t base = face * 4;
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 0);
        outIndices.push_back(base + 3);
        outIndices.push_back(base + 2);
    }
}

void GenerateSphere(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, 
                    float radius, uint32_t segments, uint32_t rings) {
    outVertices.clear();
    outIndices.clear();
    
    const float PI = 3.14159265359f;
    
    for (uint32_t ring = 0; ring <= rings; ring++) {
        float phi = PI * float(ring) / float(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);
        
        for (uint32_t seg = 0; seg <= segments; seg++) {
            float theta = 2.0f * PI * float(seg) / float(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);
            
            glm::vec3 normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
            glm::vec3 position = normal * radius;
            glm::vec2 uv(float(seg) / float(segments), float(ring) / float(rings));
            
            // Tangent (along theta direction)
            glm::vec3 tangent(-sinTheta, 0.0f, cosTheta);
            
            outVertices.push_back({ position, normal, uv, glm::vec4(tangent, 1.0f) });
        }
    }
    
    // Indices
    for (uint32_t ring = 0; ring < rings; ring++) {
        for (uint32_t seg = 0; seg < segments; seg++) {
            uint32_t current = ring * (segments + 1) + seg;
            uint32_t next = current + segments + 1;
            
            outIndices.push_back(current);
            outIndices.push_back(next);
            outIndices.push_back(current + 1);
            
            outIndices.push_back(current + 1);
            outIndices.push_back(next);
            outIndices.push_back(next + 1);
        }
    }
}

void GeneratePlane(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
                   float width, float height, uint32_t subdivisions) {
    outVertices.clear();
    outIndices.clear();
    
    float hw = width * 0.5f;
    float hh = height * 0.5f;
    
    uint32_t vertsPerSide = subdivisions + 1;
    
    for (uint32_t z = 0; z <= subdivisions; z++) {
        for (uint32_t x = 0; x <= subdivisions; x++) {
            float u = float(x) / float(subdivisions);
            float v = float(z) / float(subdivisions);
            
            glm::vec3 position(-hw + u * width, 0.0f, -hh + v * height);
            glm::vec3 normal(0.0f, 1.0f, 0.0f);
            glm::vec2 uv(u, v);
            glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            
            outVertices.push_back({ position, normal, uv, tangent });
        }
    }
    
    // Indices
    for (uint32_t z = 0; z < subdivisions; z++) {
        for (uint32_t x = 0; x < subdivisions; x++) {
            uint32_t current = z * vertsPerSide + x;
            uint32_t next = current + vertsPerSide;
            
            outIndices.push_back(current);
            outIndices.push_back(next);
            outIndices.push_back(current + 1);
            
            outIndices.push_back(current + 1);
            outIndices.push_back(next);
            outIndices.push_back(next + 1);
        }
    }
}

void GenerateCylinder(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
                      float radius, float height, uint32_t segments) {
    outVertices.clear();
    outIndices.clear();
    
    const float PI = 3.14159265359f;
    float hh = height * 0.5f;
    
    // Side vertices
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        float u = float(i) / float(segments);
        
        // Bottom vertex
        outVertices.push_back({ 
            {x * radius, -hh, z * radius}, 
            {x, 0.0f, z}, 
            {u, 0.0f}, 
            {-z, 0.0f, x, 1.0f} 
        });
        
        // Top vertex
        outVertices.push_back({ 
            {x * radius, hh, z * radius}, 
            {x, 0.0f, z}, 
            {u, 1.0f}, 
            {-z, 0.0f, x, 1.0f} 
        });
    }
    
    // Side indices
    for (uint32_t i = 0; i < segments; i++) {
        uint32_t base = i * 2;
        outIndices.push_back(base);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 1);
        
        outIndices.push_back(base + 1);
        outIndices.push_back(base + 2);
        outIndices.push_back(base + 3);
    }
    
    // Cap centers
    uint32_t bottomCenter = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back({ {0, -hh, 0}, {0, -1, 0}, {0.5f, 0.5f}, {1, 0, 0, 1} });
    
    uint32_t topCenter = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back({ {0, hh, 0}, {0, 1, 0}, {0.5f, 0.5f}, {1, 0, 0, 1} });
    
    // Cap rim vertices
    uint32_t bottomRimStart = static_cast<uint32_t>(outVertices.size());
    uint32_t topRimStart = bottomRimStart + segments + 1;
    
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        // Bottom cap
        outVertices.push_back({ 
            {x * radius, -hh, z * radius}, 
            {0, -1, 0}, 
            {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, 
            {1, 0, 0, 1} 
        });
        
        // Top cap
        outVertices.push_back({ 
            {x * radius, hh, z * radius}, 
            {0, 1, 0}, 
            {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, 
            {1, 0, 0, 1} 
        });
    }
    
    // Cap indices
    for (uint32_t i = 0; i < segments; i++) {
        // Bottom cap (reverse winding)
        outIndices.push_back(bottomCenter);
        outIndices.push_back(bottomRimStart + i + 1);
        outIndices.push_back(bottomRimStart + i);
        
        // Top cap
        outIndices.push_back(topCenter);
        outIndices.push_back(topRimStart + i);
        outIndices.push_back(topRimStart + i + 1);
    }
}

void GenerateCone(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
                  float radius, float height, uint32_t segments) {
    outVertices.clear();
    outIndices.clear();
    
    const float PI = 3.14159265359f;
    float hh = height * 0.5f;
    
    // Apex
    uint32_t apexIndex = 0;
    outVertices.push_back({ {0, hh, 0}, {0, 1, 0}, {0.5f, 1.0f}, {1, 0, 0, 1} });
    
    // Slope for normal calculation
    float slope = radius / height;
    
    // Side vertices (ring at base, one vertex per triangle for smooth shading approximation)
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        // Normal pointing outward and up
        glm::vec3 normal = glm::normalize(glm::vec3(x, slope, z));
        
        outVertices.push_back({ 
            {x * radius, -hh, z * radius}, 
            normal, 
            {float(i) / float(segments), 0.0f}, 
            {-z, 0.0f, x, 1.0f} 
        });
    }
    
    // Side indices
    for (uint32_t i = 0; i < segments; i++) {
        outIndices.push_back(apexIndex);
        outIndices.push_back(1 + i);
        outIndices.push_back(1 + i + 1);
    }
    
    // Base center
    uint32_t baseCenter = static_cast<uint32_t>(outVertices.size());
    outVertices.push_back({ {0, -hh, 0}, {0, -1, 0}, {0.5f, 0.5f}, {1, 0, 0, 1} });
    
    // Base rim
    uint32_t baseRimStart = static_cast<uint32_t>(outVertices.size());
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        outVertices.push_back({ 
            {x * radius, -hh, z * radius}, 
            {0, -1, 0}, 
            {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, 
            {1, 0, 0, 1} 
        });
    }
    
    // Base indices (reverse winding)
    for (uint32_t i = 0; i < segments; i++) {
        outIndices.push_back(baseCenter);
        outIndices.push_back(baseRimStart + i + 1);
        outIndices.push_back(baseRimStart + i);
    }
}

} // namespace Primitives

} // namespace lucent::assets

