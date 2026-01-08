#pragma once

#include "lucent/core/Core.h"
#include "lucent/mesh/EditableMesh.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <memory>

namespace lucent::scene {

// Tag component for naming entities
struct TagComponent {
    std::string name = "Entity";
    
    TagComponent() = default;
    TagComponent(const std::string& n) : name(n) {}
};

// Transform component
struct TransformComponent {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f); // Euler angles in degrees
    glm::vec3 scale = glm::vec3(1.0f);
    
    TransformComponent() = default;
    TransformComponent(const glm::vec3& pos) : position(pos) {}
    
    glm::mat4 GetLocalMatrix() const {
        // Keep transform math consistent with the editor's Euler rotation semantics via quaternion conversion.
        glm::mat4 rotationMatrix = glm::toMat4(glm::quat(glm::radians(rotation)));
        return glm::translate(glm::mat4(1.0f), position)
             * rotationMatrix
             * glm::scale(glm::mat4(1.0f), scale);
    }
    
    glm::vec3 GetForward() const {
        glm::quat q = glm::quat(glm::radians(rotation));
        return glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));
    }
    
    glm::vec3 GetRight() const {
        glm::quat q = glm::quat(glm::radians(rotation));
        return glm::normalize(q * glm::vec3(1.0f, 0.0f, 0.0f));
    }
    
    glm::vec3 GetUp() const {
        glm::quat q = glm::quat(glm::radians(rotation));
        return glm::normalize(q * glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

// Hierarchy component for parent-child relationships
struct HierarchyComponent {
    uint32_t parent = UINT32_MAX; // EntityID
    uint32_t firstChild = UINT32_MAX;
    uint32_t nextSibling = UINT32_MAX;
    uint32_t prevSibling = UINT32_MAX;
    
    bool HasParent() const { return parent != UINT32_MAX; }
};

// Camera component
struct CameraComponent {
    enum class ProjectionType { Perspective, Orthographic };
    
    ProjectionType projectionType = ProjectionType::Perspective;
    float fov = 60.0f; // Vertical FOV in degrees
    float nearClip = 0.1f;
    float farClip = 1000.0f;
    float orthoSize = 10.0f;
    bool primary = true; // Is this the main camera?
    
    glm::mat4 GetProjection(float aspectRatio) const {
        if (projectionType == ProjectionType::Perspective) {
            return glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip);
        } else {
            float orthoWidth = orthoSize * aspectRatio;
            return glm::ortho(-orthoWidth, orthoWidth, -orthoSize, orthoSize, nearClip, farClip);
        }
    }
};

// Light types
enum class LightType {
    Directional,
    Point,
    Spot,
    Area
};

// Area light shapes
enum class AreaShape {
    Disk,
    Rect
};

// Light component
struct LightComponent {
    LightType type = LightType::Point;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    
    // Point/Spot light properties
    float range = 10.0f;
    
    // Spot light properties
    float innerAngle = 30.0f; // degrees
    float outerAngle = 45.0f; // degrees
    
    // Area light properties
    AreaShape areaShape = AreaShape::Rect;
    float areaWidth = 1.0f;   // Width for rect, radius for disk
    float areaHeight = 1.0f;  // Height for rect (ignored for disk)
    
    // Soft shadow radius (for point/spot/directional)
    float shadowSoftness = 0.0f;
    
    // Shadows
    bool castShadows = true;
};

// Mesh renderer component
struct MeshRendererComponent {
    uint32_t meshAssetID = UINT32_MAX; // Reference to mesh asset
    uint32_t materialAssetID = UINT32_MAX; // Reference to material asset
    std::string materialPath; // Path to material asset file (.lmat)
    bool visible = true;
    bool castShadows = true;
    bool receiveShadows = true;
    
    // Primitive type for built-in meshes
    enum class PrimitiveType { None, Cube, Sphere, Plane, Cylinder, Cone };
    PrimitiveType primitiveType = PrimitiveType::None;
    
    // Inline material properties (for quick editing without full material system)
    // Used when materialPath is empty
    glm::vec3 baseColor = glm::vec3(0.8f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    glm::vec3 emissive = glm::vec3(0.0f);
    float emissiveIntensity = 0.0f;
    
    bool UsesMaterialAsset() const { return !materialPath.empty(); }
};

// Editable mesh component for mesh editing in Edit Mode
// Stores per-instance mesh data that can be edited (ngons supported)
struct EditableMeshComponent {
    // The editable mesh data (ngon-capable, half-edge structure)
    std::unique_ptr<mesh::EditableMesh> mesh;
    
    // Cached triangulated mesh ID for rendering (updated when mesh changes)
    uint32_t runtimeMeshID = UINT32_MAX;
    
    // Dirty flag - set when mesh needs re-triangulation
    bool dirty = true;
    
    // Source primitive type (if created from primitive, used for reset)
    MeshRendererComponent::PrimitiveType sourcePrimitive = MeshRendererComponent::PrimitiveType::None;
    
    // Whether mesh was imported from file (vs created from primitive)
    bool fromImport = false;
    
    EditableMeshComponent() = default;
    ~EditableMeshComponent() = default;
    
    // Move only (unique_ptr)
    EditableMeshComponent(EditableMeshComponent&&) = default;
    EditableMeshComponent& operator=(EditableMeshComponent&&) = default;
    
    // No copy
    EditableMeshComponent(const EditableMeshComponent&) = delete;
    EditableMeshComponent& operator=(const EditableMeshComponent&) = delete;
    
    bool HasMesh() const { return mesh != nullptr; }
    
    // Create editable mesh from current MeshRenderer primitive
    void InitFromPrimitive(MeshRendererComponent::PrimitiveType type);
    
    // Create editable mesh from triangle data
    void InitFromTriangles(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<glm::vec2>& uvs,
        const std::vector<uint32_t>& indices
    );
    
    // Mark mesh as modified (triggers re-triangulation)
    void MarkDirty() { dirty = true; }
    
    // Get triangulated output for rendering
    // Returns true if mesh was re-triangulated
    bool GetTriangulatedOutput(
        std::vector<glm::vec3>& outPositions,
        std::vector<glm::vec3>& outNormals,
        std::vector<glm::vec2>& outUVs,
        std::vector<glm::vec4>& outTangents,
        std::vector<uint32_t>& outIndices
    );
};

} // namespace lucent::scene

