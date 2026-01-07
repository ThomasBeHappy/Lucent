#pragma once

#include "lucent/core/Core.h"
#include "lucent/assets/Mesh.h"
#include "lucent/assets/Texture.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace lucent::assets {

// Material data extracted from glTF
struct MaterialData {
    std::string name;
    
    // PBR Metallic-Roughness
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    
    // Textures (indices into Model's textures array, -1 = none)
    int32_t baseColorTexture = -1;
    int32_t metallicRoughnessTexture = -1;
    int32_t normalTexture = -1;
    int32_t occlusionTexture = -1;
    int32_t emissiveTexture = -1;
    
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    
    // Alpha mode
    enum class AlphaMode { Opaque, Mask, Blend };
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    
    bool doubleSided = false;
};

// Camera data extracted from glTF
struct CameraData {
    std::string name;
    bool perspective = true;  // true = perspective, false = orthographic
    float fov = 60.0f;        // Vertical FOV in degrees (perspective)
    float orthoSize = 10.0f;  // Orthographic half-height
    float nearClip = 0.1f;
    float farClip = 1000.0f;
    float aspectRatio = 1.0f;
};

// Light data extracted from glTF (KHR_lights_punctual)
struct LightData {
    std::string name;
    enum class Type { Directional, Point, Spot };
    Type type = Type::Point;
    glm::vec3 color = glm::vec3(1.0f);
    float intensity = 1.0f;
    float range = 0.0f;         // 0 = infinite (for directional)
    float innerAngle = 0.0f;    // Spot inner cone (radians)
    float outerAngle = 0.785f;  // Spot outer cone (radians)
};

// Node in the scene hierarchy
struct NodeData {
    std::string name;
    glm::mat4 localTransform = glm::mat4(1.0f);
    
    int32_t meshIndex = -1;    // Index into Model's meshes array, -1 = no mesh
    int32_t cameraIndex = -1;  // Index into Model's cameras array, -1 = no camera
    int32_t lightIndex = -1;   // Index into Model's lights array, -1 = no light
    std::vector<uint32_t> children;
};

// A loaded model containing meshes, materials, textures, and scene hierarchy
class Model : public NonCopyable {
public:
    Model() = default;
    ~Model() = default;
    
    // Loaded data
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<std::unique_ptr<Texture>> textures;
    std::vector<MaterialData> materials;
    std::vector<CameraData> cameras;
    std::vector<LightData> lights;
    std::vector<NodeData> nodes;
    std::vector<uint32_t> rootNodes;  // Indices of root nodes
    
    std::string name;
    std::string sourcePath;
    
    // Bounds of the entire model
    AABB bounds;
};

// glTF/GLB loader
class ModelLoader : public NonCopyable {
public:
    ModelLoader() = default;
    
    // Load a glTF or GLB file
    std::unique_ptr<Model> LoadGLTF(gfx::Device* device, const std::string& path);
    
    // Load OBJ file (simpler format)
    std::unique_ptr<Model> LoadOBJ(gfx::Device* device, const std::string& path);
    
    // Get last error message
    const std::string& GetLastError() const { return m_LastError; }
    
private:
    std::string m_LastError;
};

} // namespace lucent::assets

