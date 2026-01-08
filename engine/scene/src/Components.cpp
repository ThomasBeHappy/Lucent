#include "lucent/scene/Components.h"
#include "lucent/mesh/EditableMesh.h"
#include "lucent/mesh/Triangulator.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Buffer.h"
#include "lucent/core/Log.h"

#include <cmath>

namespace lucent::scene {

// ============================================================================
// Primitive Mesh Generation (local helpers)
// ============================================================================

namespace {

// Generate cube data as triangles (for rendering primitive meshes)
void GenerateCubeDataTriangles(std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals,
                               std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices, float size = 1.0f) {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    
    float h = size * 0.5f;
    
    // 6 faces, 4 vertices each (24 total for proper normals per face)
    // Front (Z+)
    positions.push_back({-h, -h, h}); normals.push_back({0, 0, 1}); uvs.push_back({0, 0});
    positions.push_back({ h, -h, h}); normals.push_back({0, 0, 1}); uvs.push_back({1, 0});
    positions.push_back({ h,  h, h}); normals.push_back({0, 0, 1}); uvs.push_back({1, 1});
    positions.push_back({-h,  h, h}); normals.push_back({0, 0, 1}); uvs.push_back({0, 1});
    
    // Back (Z-)
    positions.push_back({ h, -h, -h}); normals.push_back({0, 0, -1}); uvs.push_back({0, 0});
    positions.push_back({-h, -h, -h}); normals.push_back({0, 0, -1}); uvs.push_back({1, 0});
    positions.push_back({-h,  h, -h}); normals.push_back({0, 0, -1}); uvs.push_back({1, 1});
    positions.push_back({ h,  h, -h}); normals.push_back({0, 0, -1}); uvs.push_back({0, 1});
    
    // Top (Y+)
    positions.push_back({-h, h,  h}); normals.push_back({0, 1, 0}); uvs.push_back({0, 0});
    positions.push_back({ h, h,  h}); normals.push_back({0, 1, 0}); uvs.push_back({1, 0});
    positions.push_back({ h, h, -h}); normals.push_back({0, 1, 0}); uvs.push_back({1, 1});
    positions.push_back({-h, h, -h}); normals.push_back({0, 1, 0}); uvs.push_back({0, 1});
    
    // Bottom (Y-)
    positions.push_back({-h, -h, -h}); normals.push_back({0, -1, 0}); uvs.push_back({0, 0});
    positions.push_back({ h, -h, -h}); normals.push_back({0, -1, 0}); uvs.push_back({1, 0});
    positions.push_back({ h, -h,  h}); normals.push_back({0, -1, 0}); uvs.push_back({1, 1});
    positions.push_back({-h, -h,  h}); normals.push_back({0, -1, 0}); uvs.push_back({0, 1});
    
    // Right (X+)
    positions.push_back({h, -h,  h}); normals.push_back({1, 0, 0}); uvs.push_back({0, 0});
    positions.push_back({h, -h, -h}); normals.push_back({1, 0, 0}); uvs.push_back({1, 0});
    positions.push_back({h,  h, -h}); normals.push_back({1, 0, 0}); uvs.push_back({1, 1});
    positions.push_back({h,  h,  h}); normals.push_back({1, 0, 0}); uvs.push_back({0, 1});
    
    // Left (X-)
    positions.push_back({-h, -h, -h}); normals.push_back({-1, 0, 0}); uvs.push_back({0, 0});
    positions.push_back({-h, -h,  h}); normals.push_back({-1, 0, 0}); uvs.push_back({1, 0});
    positions.push_back({-h,  h,  h}); normals.push_back({-1, 0, 0}); uvs.push_back({1, 1});
    positions.push_back({-h,  h, -h}); normals.push_back({-1, 0, 0}); uvs.push_back({0, 1});
    
    // Indices (2 triangles per face)
    for (uint32_t face = 0; face < 6; face++) {
        uint32_t base = face * 4;
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

// Generate cube as ngon faces (8 vertices, 6 quad faces) for editable mesh
void GenerateCubeNgon(std::vector<glm::vec3>& positions, std::vector<std::vector<uint32_t>>& faces, float size = 1.0f) {
    positions.clear();
    faces.clear();
    
    float h = size * 0.5f;
    
    // 8 corner vertices
    positions.push_back({-h, -h, -h}); // 0: left-bottom-back
    positions.push_back({ h, -h, -h}); // 1: right-bottom-back
    positions.push_back({ h,  h, -h}); // 2: right-top-back
    positions.push_back({-h,  h, -h}); // 3: left-top-back
    positions.push_back({-h, -h,  h}); // 4: left-bottom-front
    positions.push_back({ h, -h,  h}); // 5: right-bottom-front
    positions.push_back({ h,  h,  h}); // 6: right-top-front
    positions.push_back({-h,  h,  h}); // 7: left-top-front
    
    // 6 quad faces with counter-clockwise winding (viewed from outside)
    // Front (Z+): normal pointing +Z
    faces.push_back({7, 6, 5, 4});
    // Back (Z-): normal pointing -Z
    faces.push_back({2, 3, 0, 1});
    // Top (Y+): normal pointing +Y
    faces.push_back({7, 6, 2, 3});
    // Bottom (Y-): normal pointing -Y
    faces.push_back({0, 1, 5, 4});
    // Right (X+): normal pointing +X
    faces.push_back({6, 2, 1, 5});
    // Left (X-): normal pointing -X
    faces.push_back({3, 7, 4, 0});
}

void GenerateSphereData(std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals,
                        std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices,
                        float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16) {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    
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
            positions.push_back(normal * radius);
            normals.push_back(normal);
            uvs.push_back({float(seg) / float(segments), float(ring) / float(rings)});
        }
    }
    
    for (uint32_t ring = 0; ring < rings; ring++) {
        for (uint32_t seg = 0; seg < segments; seg++) {
            uint32_t current = ring * (segments + 1) + seg;
            uint32_t next = current + segments + 1;
            
            indices.push_back(current);
            indices.push_back(next);
            indices.push_back(current + 1);
            
            indices.push_back(current + 1);
            indices.push_back(next);
            indices.push_back(next + 1);
        }
    }
}

void GeneratePlaneData(std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals,
                       std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices,
                       float width = 1.0f, float height = 1.0f) {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    
    float hw = width * 0.5f;
    float hh = height * 0.5f;
    
    positions.push_back({-hw, 0, -hh}); normals.push_back({0, 1, 0}); uvs.push_back({0, 0});
    positions.push_back({ hw, 0, -hh}); normals.push_back({0, 1, 0}); uvs.push_back({1, 0});
    positions.push_back({ hw, 0,  hh}); normals.push_back({0, 1, 0}); uvs.push_back({1, 1});
    positions.push_back({-hw, 0,  hh}); normals.push_back({0, 1, 0}); uvs.push_back({0, 1});
    
    indices = {0, 2, 1, 0, 3, 2};
}

void GenerateCylinderData(std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals,
                          std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices,
                          float radius = 0.5f, float height = 1.0f, uint32_t segments = 32) {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    
    const float PI = 3.14159265359f;
    float hh = height * 0.5f;
    
    // Side vertices
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        float u = float(i) / float(segments);
        
        // Bottom
        positions.push_back({x * radius, -hh, z * radius});
        normals.push_back({x, 0, z});
        uvs.push_back({u, 0});
        
        // Top
        positions.push_back({x * radius, hh, z * radius});
        normals.push_back({x, 0, z});
        uvs.push_back({u, 1});
    }
    
    // Side indices
    for (uint32_t i = 0; i < segments; i++) {
        uint32_t base = i * 2;
        indices.push_back(base);
        indices.push_back(base + 2);
        indices.push_back(base + 1);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
    
    // Cap centers
    uint32_t bottomCenter = static_cast<uint32_t>(positions.size());
    positions.push_back({0, -hh, 0});
    normals.push_back({0, -1, 0});
    uvs.push_back({0.5f, 0.5f});
    
    uint32_t topCenter = static_cast<uint32_t>(positions.size());
    positions.push_back({0, hh, 0});
    normals.push_back({0, 1, 0});
    uvs.push_back({0.5f, 0.5f});
    
    // Cap rim vertices
    uint32_t bottomRimStart = static_cast<uint32_t>(positions.size());
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        positions.push_back({x * radius, -hh, z * radius});
        normals.push_back({0, -1, 0});
        uvs.push_back({x * 0.5f + 0.5f, z * 0.5f + 0.5f});
    }
    
    uint32_t topRimStart = static_cast<uint32_t>(positions.size());
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        positions.push_back({x * radius, hh, z * radius});
        normals.push_back({0, 1, 0});
        uvs.push_back({x * 0.5f + 0.5f, z * 0.5f + 0.5f});
    }
    
    // Cap indices
    for (uint32_t i = 0; i < segments; i++) {
        indices.push_back(bottomCenter);
        indices.push_back(bottomRimStart + i + 1);
        indices.push_back(bottomRimStart + i);
        
        indices.push_back(topCenter);
        indices.push_back(topRimStart + i);
        indices.push_back(topRimStart + i + 1);
    }
}

void GenerateConeData(std::vector<glm::vec3>& positions, std::vector<glm::vec3>& normals,
                      std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices,
                      float radius = 0.5f, float height = 1.0f, uint32_t segments = 32) {
    positions.clear();
    normals.clear();
    uvs.clear();
    indices.clear();
    
    const float PI = 3.14159265359f;
    float hh = height * 0.5f;
    float slope = radius / height;
    
    // Apex
    positions.push_back({0, hh, 0});
    normals.push_back({0, 1, 0});
    uvs.push_back({0.5f, 1});
    
    // Side rim
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        glm::vec3 normal = glm::normalize(glm::vec3(x, slope, z));
        
        positions.push_back({x * radius, -hh, z * radius});
        normals.push_back(normal);
        uvs.push_back({float(i) / float(segments), 0});
    }
    
    // Side indices
    for (uint32_t i = 0; i < segments; i++) {
        indices.push_back(0);
        indices.push_back(1 + i);
        indices.push_back(1 + i + 1);
    }
    
    // Base center
    uint32_t baseCenter = static_cast<uint32_t>(positions.size());
    positions.push_back({0, -hh, 0});
    normals.push_back({0, -1, 0});
    uvs.push_back({0.5f, 0.5f});
    
    // Base rim
    uint32_t baseRimStart = static_cast<uint32_t>(positions.size());
    for (uint32_t i = 0; i <= segments; i++) {
        float theta = 2.0f * PI * float(i) / float(segments);
        float x = std::cos(theta);
        float z = std::sin(theta);
        
        positions.push_back({x * radius, -hh, z * radius});
        normals.push_back({0, -1, 0});
        uvs.push_back({x * 0.5f + 0.5f, z * 0.5f + 0.5f});
    }
    
    // Base indices
    for (uint32_t i = 0; i < segments; i++) {
        indices.push_back(baseCenter);
        indices.push_back(baseRimStart + i + 1);
        indices.push_back(baseRimStart + i);
    }
}

} // anonymous namespace

// ============================================================================
// EditableMeshComponent Implementation
// ============================================================================

void EditableMeshComponent::InitFromPrimitive(MeshRendererComponent::PrimitiveType type) {
    // For cube, use ngon representation (8 verts, 6 quads) instead of triangles
    if (type == MeshRendererComponent::PrimitiveType::Cube) {
        std::vector<glm::vec3> positions;
        std::vector<std::vector<uint32_t>> faces;
        GenerateCubeNgon(positions, faces);
        
        mesh = std::make_unique<mesh::EditableMesh>(
            mesh::EditableMesh::FromFaces(positions, faces)
        );
        sourcePrimitive = type;
        fromImport = false;
        dirty = true;
        
        LUCENT_CORE_DEBUG("Created editable cube: {} verts, {} faces",
                          mesh->VertexCount(), mesh->FaceCount());
        return;
    }
    
    // For other primitives, use triangle representation
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
    
    switch (type) {
        case MeshRendererComponent::PrimitiveType::Cube:
            // Handled above
            break;
        case MeshRendererComponent::PrimitiveType::Sphere:
            GenerateSphereData(positions, normals, uvs, indices);
            break;
        case MeshRendererComponent::PrimitiveType::Plane:
            GeneratePlaneData(positions, normals, uvs, indices);
            break;
        case MeshRendererComponent::PrimitiveType::Cylinder:
            GenerateCylinderData(positions, normals, uvs, indices);
            break;
        case MeshRendererComponent::PrimitiveType::Cone:
            GenerateConeData(positions, normals, uvs, indices);
            break;
        default:
            LUCENT_CORE_WARN("Cannot create editable mesh from primitive type: None");
            return;
    }
    
    mesh = std::make_unique<mesh::EditableMesh>(
        mesh::EditableMesh::FromTriangles(positions, normals, uvs, indices)
    );
    sourcePrimitive = type;
    fromImport = false;
    dirty = true;
    
    LUCENT_CORE_DEBUG("Created editable mesh from primitive: {} verts, {} faces",
                      mesh->VertexCount(), mesh->FaceCount());
}

void EditableMeshComponent::InitFromTriangles(
    const std::vector<glm::vec3>& positions,
    const std::vector<glm::vec3>& normals,
    const std::vector<glm::vec2>& uvs,
    const std::vector<uint32_t>& indices
) {
    mesh = std::make_unique<mesh::EditableMesh>(
        mesh::EditableMesh::FromTriangles(positions, normals, uvs, indices)
    );
    sourcePrimitive = MeshRendererComponent::PrimitiveType::None;
    fromImport = true;
    dirty = true;
    
    LUCENT_CORE_DEBUG("Created editable mesh from triangles: {} verts, {} faces",
                      mesh->VertexCount(), mesh->FaceCount());
}

bool EditableMeshComponent::GetTriangulatedOutput(
    std::vector<glm::vec3>& outPositions,
    std::vector<glm::vec3>& outNormals,
    std::vector<glm::vec2>& outUVs,
    std::vector<glm::vec4>& outTangents,
    std::vector<uint32_t>& outIndices
) {
    if (!mesh || !dirty) {
        return false;
    }
    
    // Triangulate the editable mesh
    auto output = mesh->ToTriangles();
    
    if (output.vertices.empty() || output.indices.empty()) {
        LUCENT_CORE_WARN("EditableMesh triangulation produced no geometry");
        return false;
    }
    
    // Copy to output vectors
    outPositions.clear();
    outNormals.clear();
    outUVs.clear();
    outTangents.clear();
    outIndices.clear();
    
    outPositions.reserve(output.vertices.size());
    outNormals.reserve(output.vertices.size());
    outUVs.reserve(output.vertices.size());
    outTangents.reserve(output.vertices.size());
    
    for (const auto& v : output.vertices) {
        outPositions.push_back(v.position);
        outNormals.push_back(v.normal);
        outUVs.push_back(v.uv);
        outTangents.push_back(v.tangent);
    }
    
    outIndices = std::move(output.indices);
    
    dirty = false;
    
    LUCENT_CORE_DEBUG("EditableMesh triangulated: {} vertices, {} indices",
                      outPositions.size(), outIndices.size());
    
    return true;
}

} // namespace lucent::scene
