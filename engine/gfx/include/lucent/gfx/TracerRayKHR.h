#pragma once

#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Buffer.h"
#include "lucent/gfx/Image.h"
#include "lucent/gfx/RenderSettings.h"
#include "lucent/gfx/TracerCompute.h" // Reuse GPUCamera, GPUMaterial
#include "lucent/gfx/EnvironmentMap.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace lucent::gfx {

// Bottom-level acceleration structure (per mesh)
struct BLAS {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer buffer;
    VkDeviceAddress deviceAddress = 0;
    uint32_t triangleCount = 0;
};

// Top-level acceleration structure (per scene)
struct TLAS {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer buffer;
    Buffer instanceBuffer;
    VkDeviceAddress deviceAddress = 0;
    uint32_t instanceCount = 0;
};

// Mesh data for ray tracing
struct RTMesh {
    Buffer vertexBuffer;
    Buffer indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t materialId = 0;
    BLAS blas;
};

// Instance transform for TLAS
struct RTInstance {
    glm::mat4 transform;
    uint32_t meshIndex;
    uint32_t materialId;
};

// RT vertex for shader access (pos + normal + uv, 32 bytes)
struct RTVertex {
    glm::vec3 position;
    float pad0;
    glm::vec3 normal;
    float pad1;
    glm::vec2 uv;
    glm::vec2 pad2;
};

// Push constants for ray tracing shaders
struct RTPushConstants {
    uint32_t frameIndex;
    uint32_t sampleIndex;
    uint32_t maxBounces;
    float clampValue;
    uint32_t lightCount;
    float envIntensity;
    float envRotation;
    uint32_t useEnvMap;
    uint32_t volumeCount;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

// Vulkan KHR ray tracing based path tracer
class TracerRayKHR {
public:
    TracerRayKHR() = default;
    ~TracerRayKHR();
    
    bool Init(VulkanContext* context, Device* device);
    void Shutdown();
    
    // Check if ray tracing is supported
    bool IsSupported() const { return m_Supported; }
    
    // Update scene - builds/updates acceleration structures
    void UpdateScene(const std::vector<BVHBuilder::Triangle>& triangles,
                     const std::vector<GPUMaterial>& materials,
                     const std::vector<GPULight>& lights = {},
                     const std::vector<GPUVolume>& volumes = {});
    
    // Set environment map for IBL
    void SetEnvironmentMap(EnvironmentMap* envMap);

    // Update only light data (no BLAS/TLAS rebuild)
    void UpdateLights(const std::vector<GPULight>& lights = {});
    
    // Trace rays for one sample
    void Trace(VkCommandBuffer cmd,
               const GPUCamera& camera,
               const RenderSettings& settings,
               Image* outputImage);
    
    // Reset accumulation
    void ResetAccumulation();
    
    // Get accumulation image
    Image* GetAccumulationImage() { return &m_AccumulationImage; }
    
    // Get AOV images for denoiser
    Image* GetAlbedoImage() { return &m_AlbedoImage; }
    Image* GetNormalImage() { return &m_NormalImage; }
    
    bool IsReady() const { return m_Ready; }
    
private:
    bool LoadRayTracingFunctions();
    bool CreateRayTracingPipeline();
    bool CreateShaderBindingTable();
    bool CreateDescriptorSets();
    bool CreateAccumulationImage(uint32_t width, uint32_t height);
    
    bool BuildBLAS(const std::vector<BVHBuilder::Triangle>& triangles);
    bool BuildVolumeBLAS(const std::vector<GPUVolume>& volumes);
    bool BuildTLAS();
    
private:
    VulkanContext* m_Context = nullptr;
    Device* m_Device = nullptr;
    
    bool m_Supported = false;
    bool m_Ready = false;
    
    // Ray tracing function pointers
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    
    // Acceleration structures
    BLAS m_BLAS;
    BLAS m_VolumeBLAS;
    TLAS m_TLAS;

    // Volume AABB source buffer for procedural BLAS
    Buffer m_VolumeAABBBuffer;
    
    // Scene data
    Buffer m_PositionBuffer;        // Positions only for BLAS geometry
    Buffer m_VertexBuffer;          // Full vertices (RTVertex) for shader access
    Buffer m_IndexBuffer;
    Buffer m_PrimitiveMaterialBuffer;
    Buffer m_MaterialBuffer;
    Buffer m_LightBuffer;
    Buffer m_VolumeBuffer;
    uint32_t m_TriangleCount = 0;
    uint32_t m_LightCount = 0;
    uint32_t m_VolumeCount = 0;
    
    // Environment map
    EnvironmentMap* m_EnvMap = nullptr;
    
    // Ray tracing pipeline
    VkDescriptorSetLayout m_DescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    bool m_DescriptorsDirty = true;
    
    // Shader modules
    VkShaderModule m_RaygenShader = VK_NULL_HANDLE;
    VkShaderModule m_MissShader = VK_NULL_HANDLE;
    VkShaderModule m_ClosestHitShader = VK_NULL_HANDLE;
    VkShaderModule m_ShadowMissShader = VK_NULL_HANDLE;
    VkShaderModule m_ShadowClosestHitShader = VK_NULL_HANDLE;
    VkShaderModule m_VolumeIntersectionShader = VK_NULL_HANDLE;
    VkShaderModule m_VolumeClosestHitShader = VK_NULL_HANDLE;
    
    // Shader binding table
    Buffer m_SBTBuffer;
    VkStridedDeviceAddressRegionKHR m_RaygenRegion{};
    VkStridedDeviceAddressRegionKHR m_MissRegion{};
    VkStridedDeviceAddressRegionKHR m_HitRegion{};
    VkStridedDeviceAddressRegionKHR m_CallableRegion{};
    
    // Accumulation and AOV images
    Image m_AccumulationImage;
    Image m_AlbedoImage;   // First-hit albedo for denoiser
    Image m_NormalImage;   // First-hit normal for denoiser
    Buffer m_CameraBuffer;
    uint32_t m_AccumWidth = 0;
    uint32_t m_AccumHeight = 0;
    uint32_t m_FrameIndex = 0;
    
    // Descriptor pool
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
};

} // namespace lucent::gfx


