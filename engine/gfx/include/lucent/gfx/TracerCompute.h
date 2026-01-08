#pragma once

#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Buffer.h"
#include "lucent/gfx/Image.h"
#include "lucent/gfx/RenderSettings.h"
#include "lucent/gfx/EnvironmentMap.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace lucent::gfx {

// BVH node for GPU traversal (32 bytes, good alignment)
struct BVHNode {
    glm::vec3 aabbMin;
    uint32_t leftFirst;  // If count > 0: first primitive. Else: left child index
    glm::vec3 aabbMax;
    uint32_t count;      // Primitive count (0 = internal node)
};

// Triangle for GPU (48 bytes)
struct GPUTriangle {
    glm::vec3 v0;
    uint32_t materialId;
    glm::vec3 v1;
    uint32_t pad0;
    glm::vec3 v2;
    uint32_t pad1;
};

// Instance for GPU (80 bytes)
struct GPUInstance {
    glm::mat4 transform;
    uint32_t meshBVHOffset;   // Offset into BVH node buffer
    uint32_t triangleOffset;  // Offset into triangle buffer
    uint32_t triangleCount;
    uint32_t materialId;
};

// Material for GPU (48 bytes)
struct GPUMaterial {
    glm::vec4 baseColor;       // RGB + alpha
    glm::vec4 emissive;        // RGB + intensity
    float metallic;
    float roughness;
    float ior;
    uint32_t flags;            // Various material flags
};

// Volume instance for GPU (includes world-space bounds for V1)
struct GPUVolume {
    glm::mat4 transform;       // World to local space (inverse model) - optional, V1 may use world AABB
    glm::vec3 scatterColor;    // Scattering color
    float density;             // Volume density
    glm::vec3 absorption;      // Absorption coefficient
    float anisotropy;          // Phase function anisotropy (-1 to 1)
    glm::vec3 emission;        // Volume emission color
    float emissionStrength;    // Emission multiplier
    glm::vec3 aabbMin;         // World-space bounds (V1)
    float pad0;
    glm::vec3 aabbMax;         // World-space bounds (V1)
    float pad1;
};

// Camera for GPU
struct GPUCamera {
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::vec3 position;
    float fov;
    glm::vec2 resolution;
    float nearPlane;
    float farPlane;
};

// Light types (matching scene::LightType)
enum class GPULightType : uint32_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Area = 3
};

// Area light shapes
enum class GPUAreaShape : uint32_t {
    Disk = 0,
    Rect = 1
};

// Light for GPU (64 bytes, aligned)
struct GPULight {
    glm::vec3 position;      // World position (point/spot/area) or direction (directional)
    uint32_t type;           // GPULightType
    glm::vec3 color;         // RGB color
    float intensity;         // Light intensity
    glm::vec3 direction;     // Light direction (for spot/directional/area normal)
    float range;             // Attenuation range (point/spot)
    float innerAngle;        // Spot inner cone angle (radians)
    float outerAngle;        // Spot outer cone angle (radians)
    float areaWidth;         // Area light width (rect) or radius (disk)
    float areaHeight;        // Area light height (rect only)
    glm::vec3 areaTangent;   // Area light tangent (for rect orientation)
    uint32_t areaShape;      // GPUAreaShape (0=disk, 1=rect)
};

// Push constants for compute shader
struct TracerPushConstants {
    uint32_t frameIndex;
    uint32_t sampleIndex;
    uint32_t maxBounces;
    float clampValue;
    uint32_t lightCount;
    float envIntensity;
    float envRotation;
    uint32_t useEnvMap;
    uint32_t transparentBackground;
    uint32_t volumeCount;  // Number of volume instances
    uint32_t pad0;
    uint32_t pad1;
};

// Scene data for GPU
struct SceneGPU {
    // Geometry
    Buffer triangleBuffer;
    Buffer bvhNodeBuffer;
    Buffer instanceBuffer;
    Buffer materialBuffer;
    Buffer lightBuffer;
    Buffer volumeBuffer;  // Volume instances
    
    // Counts
    uint32_t triangleCount = 0;
    uint32_t bvhNodeCount = 0;
    uint32_t instanceCount = 0;
    uint32_t materialCount = 0;
    uint32_t lightCount = 0;
    uint32_t volumeCount = 0;
    
    bool valid = false;
};

// CPU-side BVH builder
class BVHBuilder {
public:
    struct Triangle {
        glm::vec3 v0, v1, v2;
        glm::vec3 n0, n1, n2;  // Normals
        glm::vec2 uv0, uv1, uv2;
        uint32_t materialId;
    };
    
    void Build(const std::vector<Triangle>& triangles);
    
    const std::vector<BVHNode>& GetNodes() const { return m_Nodes; }
    const std::vector<uint32_t>& GetTriangleIndices() const { return m_TriangleIndices; }
    
private:
    void BuildRecursive(uint32_t nodeIdx, uint32_t start, uint32_t end);
    float EvaluateSAH(uint32_t nodeIdx, int axis, float splitPos, uint32_t start, uint32_t end);
    
    std::vector<BVHNode> m_Nodes;
    std::vector<Triangle> m_Triangles;
    std::vector<uint32_t> m_TriangleIndices;
};

// Compute-based path tracer
class TracerCompute {
public:
    TracerCompute() = default;
    ~TracerCompute();
    
    bool Init(VulkanContext* context, Device* device);
    void Shutdown();
    
    // Update scene with pre-built triangle data
    void UpdateScene(const std::vector<BVHBuilder::Triangle>& triangles,
                     const std::vector<GPUMaterial>& materials,
                     const std::vector<GPULight>& lights = {},
                     const std::vector<GPUVolume>& volumes = {});
    
    // Set environment map for IBL
    void SetEnvironmentMap(EnvironmentMap* envMap);

    // Update only light data (no BVH rebuild)
    void UpdateLights(const std::vector<GPULight>& lights = {});
    
    // Render a sample
    void Trace(VkCommandBuffer cmd, 
               const GPUCamera& camera,
               const RenderSettings& settings,
               Image* outputImage);
    
    // Reset accumulation
    void ResetAccumulation();
    
    // Check if ready to trace
    bool IsReady() const { return m_Ready; }
    
    // Get accumulation image
    Image* GetAccumulationImage() { return &m_AccumulationImage; }
    
    // Get AOV images for denoiser
    Image* GetAlbedoImage() { return &m_AlbedoImage; }
    Image* GetNormalImage() { return &m_NormalImage; }
    
private:
    bool CreateComputePipeline();
    bool CreateDescriptorSets();
    bool CreateAccumulationImage(uint32_t width, uint32_t height);
    void UpdateDescriptors();
    
private:
    VulkanContext* m_Context = nullptr;
    Device* m_Device = nullptr;
    
    // Scene data
    SceneGPU m_SceneGPU;
    bool m_SceneDirty = true;
    bool m_DescriptorsDirty = true;  // Only update descriptors when needed
    
    // Environment map
    EnvironmentMap* m_EnvMap = nullptr;
    
    // Compute pipeline
    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkShaderModule m_ComputeShader = VK_NULL_HANDLE;
    
    // Accumulation and AOV images
    Image m_AccumulationImage;
    Image m_AlbedoImage;   // First-hit albedo for denoiser
    Image m_NormalImage;   // First-hit normal for denoiser
    Buffer m_CameraBuffer;
    uint32_t m_AccumWidth = 0;
    uint32_t m_AccumHeight = 0;
    uint32_t m_FrameIndex = 0;
    
    // Descriptor pool (simple, just for this tracer)
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    
    bool m_Ready = false;
};

} // namespace lucent::gfx
