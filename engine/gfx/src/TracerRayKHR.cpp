#include "lucent/gfx/TracerRayKHR.h"
#include "lucent/gfx/PipelineBuilder.h"
#include "lucent/core/Log.h"
#include <cstring>

namespace lucent::gfx {

TracerRayKHR::~TracerRayKHR() {
    Shutdown();
}

bool TracerRayKHR::Init(VulkanContext* context, Device* device) {
    m_Context = context;
    m_Device = device;
    
    // Check if ray tracing is supported
    if (!context->GetDeviceFeatures().rayTracingPipeline ||
        !context->GetDeviceFeatures().accelerationStructure) {
        LUCENT_CORE_WARN("TracerRayKHR: Ray tracing not supported on this device");
        m_Supported = false;
        return false;
    }
    
    m_Supported = true;
    
    // Load ray tracing functions
    if (!LoadRayTracingFunctions()) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to load ray tracing functions");
        m_Supported = false;
        return false;
    }
    
    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },  // accumImage + albedoImage + normalImage
        // vertices, indices, materials, primitiveMaterialIds, lights
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }  // env map + marginal CDF + conditional CDF
    };
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;
    
    if (vkCreateDescriptorPool(context->GetDevice(), &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create descriptor pool");
        return false;
    }
    
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr }, // vertices
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr }, // indices
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr }, // materials
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr },  // camera
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr }, // per-primitive material ids
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr },  // albedoImage
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr },  // normalImage
        { 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr },  // lights
        { 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, nullptr },  // envMap
        { 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, nullptr },  // envMarginalCDF
        { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR, nullptr }   // envConditionalCDF
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 13;
    layoutInfo.pBindings = bindings;
    
    if (vkCreateDescriptorSetLayout(context->GetDevice(), &layoutInfo, nullptr, &m_DescriptorLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create descriptor set layout");
        return false;
    }
    
    // Create camera UBO
    BufferDesc cameraDesc{};
    cameraDesc.size = sizeof(GPUCamera);
    cameraDesc.usage = BufferUsage::Uniform;
    cameraDesc.hostVisible = true;
    cameraDesc.debugName = "RTCameraUBO";
    m_CameraBuffer.Init(device, cameraDesc);
    
    LUCENT_CORE_INFO("TracerRayKHR initialized (ray tracing supported)");
    return true;
}

void TracerRayKHR::Shutdown() {
    VkDevice device = m_Context ? m_Context->GetDevice() : VK_NULL_HANDLE;
    if (!device) return;
    
    m_Context->WaitIdle();
    
    // Destroy acceleration structures
    if (m_BLAS.handle != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
        vkDestroyAccelerationStructureKHR(device, m_BLAS.handle, nullptr);
    }
    m_BLAS.buffer.Shutdown();
    
    if (m_TLAS.handle != VK_NULL_HANDLE && vkDestroyAccelerationStructureKHR) {
        vkDestroyAccelerationStructureKHR(device, m_TLAS.handle, nullptr);
    }
    m_TLAS.buffer.Shutdown();
    m_TLAS.instanceBuffer.Shutdown();
    
    // Destroy buffers
    m_PositionBuffer.Shutdown();
    m_VertexBuffer.Shutdown();
    m_IndexBuffer.Shutdown();
    m_PrimitiveMaterialBuffer.Shutdown();
    m_MaterialBuffer.Shutdown();
    m_SBTBuffer.Shutdown();
    m_CameraBuffer.Shutdown();
    
    // Destroy images
    m_AccumulationImage.Shutdown();
    m_AlbedoImage.Shutdown();
    m_NormalImage.Shutdown();
    
    // Destroy pipeline
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_Pipeline, nullptr);
    }
    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
    }
    
    // Destroy shaders
    if (m_RaygenShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_RaygenShader, nullptr);
        m_RaygenShader = VK_NULL_HANDLE;
    }
    if (m_MissShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_MissShader, nullptr);
        m_MissShader = VK_NULL_HANDLE;
    }
    if (m_ClosestHitShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_ClosestHitShader, nullptr);
        m_ClosestHitShader = VK_NULL_HANDLE;
    }
    if (m_ShadowMissShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_ShadowMissShader, nullptr);
        m_ShadowMissShader = VK_NULL_HANDLE;
    }
    if (m_ShadowClosestHitShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_ShadowClosestHitShader, nullptr);
        m_ShadowClosestHitShader = VK_NULL_HANDLE;
    }
    
    // Destroy descriptor resources
    if (m_DescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_DescriptorLayout, nullptr);
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
    }
    
    m_Ready = false;
    m_Supported = false;
}

bool TracerRayKHR::LoadRayTracingFunctions() {
    VkDevice device = m_Context->GetDevice();
    
    vkCreateAccelerationStructureKHR = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    vkDestroyAccelerationStructureKHR = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    vkGetAccelerationStructureBuildSizesKHR = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    vkGetAccelerationStructureDeviceAddressKHR = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    vkCmdBuildAccelerationStructuresKHR = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    vkCreateRayTracingPipelinesKHR = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
    vkGetRayTracingShaderGroupHandlesKHR = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesKHR"));
    vkCmdTraceRaysKHR = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        vkGetDeviceProcAddr(device, "vkCmdTraceRaysKHR"));
    
    return vkCreateAccelerationStructureKHR && 
           vkDestroyAccelerationStructureKHR &&
           vkGetAccelerationStructureBuildSizesKHR &&
           vkGetAccelerationStructureDeviceAddressKHR &&
           vkCmdBuildAccelerationStructuresKHR &&
           vkCreateRayTracingPipelinesKHR &&
           vkGetRayTracingShaderGroupHandlesKHR &&
           vkCmdTraceRaysKHR;
}

bool TracerRayKHR::CreateRayTracingPipeline() {
    VkDevice device = m_Context->GetDevice();
    
    // Load shaders
    m_RaygenShader = PipelineBuilder::LoadShaderModule(device, "shaders/rt_raygen.rgen.spv");
    m_MissShader = PipelineBuilder::LoadShaderModule(device, "shaders/rt_miss.rmiss.spv");
    m_ClosestHitShader = PipelineBuilder::LoadShaderModule(device, "shaders/rt_closesthit.rchit.spv");
    m_ShadowMissShader = PipelineBuilder::LoadShaderModule(device, "shaders/rt_shadow_miss.rmiss.spv");
    m_ShadowClosestHitShader = PipelineBuilder::LoadShaderModule(device, "shaders/rt_shadow.rchit.spv");
    
    if (!m_RaygenShader || !m_MissShader || !m_ClosestHitShader || !m_ShadowMissShader || !m_ShadowClosestHitShader) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to load ray tracing shaders");
        return false;
    }
    
    // Create pipeline layout
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(RTPushConstants);
    
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_DescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create pipeline layout");
        return false;
    }
    
    // Shader stages
    VkPipelineShaderStageCreateInfo stages[5] = {};
    
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = m_RaygenShader;
    stages[0].pName = "main";
    
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = m_MissShader;
    stages[1].pName = "main";
    
    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = m_ClosestHitShader;
    stages[2].pName = "main";

    stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[3].module = m_ShadowMissShader;
    stages[3].pName = "main";

    stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[4].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[4].module = m_ShadowClosestHitShader;
    stages[4].pName = "main";
    
    // Shader groups
    VkRayTracingShaderGroupCreateInfoKHR groups[5] = {};
    
    // Raygen group
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
    
    // Miss group
    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;
    
    // Shadow miss group
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 3;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Primary closest hit group
    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].generalShader = VK_SHADER_UNUSED_KHR;
    groups[3].closestHitShader = 2;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Shadow closest hit group
    groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].generalShader = VK_SHADER_UNUSED_KHR;
    groups[4].closestHitShader = 4;
    groups[4].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;
    
    // Create ray tracing pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = 5;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 5;
    pipelineInfo.pGroups = groups;
    // Need recursion for shadow rays from closest-hit
    uint32_t maxDepth = m_Context->GetDeviceFeatures().maxRayRecursionDepth;
    pipelineInfo.maxPipelineRayRecursionDepth = (maxDepth >= 2) ? 2 : 1;
    pipelineInfo.layout = m_PipelineLayout;
    
    if (vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create ray tracing pipeline");
        return false;
    }
    
    LUCENT_CORE_INFO("TracerRayKHR: Ray tracing pipeline created");
    return true;
}

bool TracerRayKHR::CreateShaderBindingTable() {
    if (!m_Pipeline) return false;
    
    VkDevice device = m_Context->GetDevice();
    const auto& features = m_Context->GetDeviceFeatures();
    
    uint32_t handleSize = features.shaderGroupHandleSize;
    uint32_t handleAlignment = features.shaderGroupBaseAlignment;
    uint32_t alignedHandleSize = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);
    
    // raygen (1) + miss (2 ray types) + hit (2 ray types)
    uint32_t groupCount = 5;
    uint32_t sbtSize = groupCount * alignedHandleSize;
    
    // Get shader group handles
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(device, m_Pipeline, 0, groupCount, 
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to get shader group handles");
        return false;
    }
    
    // Create SBT buffer
    BufferDesc sbtDesc{};
    sbtDesc.size = sbtSize;
    sbtDesc.usage = BufferUsage::ShaderBindingTable;
    sbtDesc.hostVisible = true;
    sbtDesc.deviceAddress = true;
    sbtDesc.debugName = "ShaderBindingTable";
    
    if (!m_SBTBuffer.Init(m_Device, sbtDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create SBT buffer");
        return false;
    }
    
    // Copy handles to SBT with alignment
    std::vector<uint8_t> sbtData(sbtSize, 0);
    for (uint32_t i = 0; i < groupCount; i++) {
        memcpy(sbtData.data() + i * alignedHandleSize, 
               handles.data() + i * handleSize, 
               handleSize);
    }
    m_SBTBuffer.Upload(sbtData.data(), sbtSize);
    
    // Setup regions
    VkDeviceAddress sbtAddress = m_SBTBuffer.GetDeviceAddress();
    
    m_RaygenRegion.deviceAddress = sbtAddress;
    m_RaygenRegion.stride = alignedHandleSize;
    m_RaygenRegion.size = alignedHandleSize;
    
    m_MissRegion.deviceAddress = sbtAddress + alignedHandleSize;
    m_MissRegion.stride = alignedHandleSize;
    m_MissRegion.size = 2 * alignedHandleSize;
    
    m_HitRegion.deviceAddress = sbtAddress + 3 * alignedHandleSize;
    m_HitRegion.stride = alignedHandleSize;
    m_HitRegion.size = 2 * alignedHandleSize;
    
    m_CallableRegion = {};
    
    LUCENT_CORE_DEBUG("TracerRayKHR: Shader binding table created");
    return true;
}

bool TracerRayKHR::CreateDescriptorSets() {
    VkDevice device = m_Context->GetDevice();
    
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorLayout;
    
    if (vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to allocate descriptor set");
        return false;
    }
    m_DescriptorsDirty = true;
    return true;
}

bool TracerRayKHR::CreateAccumulationImage(uint32_t width, uint32_t height) {
    if (width == m_AccumWidth && height == m_AccumHeight && m_AccumulationImage.GetHandle() != VK_NULL_HANDLE) {
        return true;
    }
    
    m_AccumulationImage.Shutdown();
    m_AlbedoImage.Shutdown();
    m_NormalImage.Shutdown();
    
    ImageDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    desc.debugName = "RTAccumulationImage";
    
    if (!m_AccumulationImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create accumulation image");
        return false;
    }
    
    // Create AOV images for denoiser
    desc.debugName = "RTAlbedoImage";
    if (!m_AlbedoImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create albedo image");
        return false;
    }
    
    desc.debugName = "RTNormalImage";
    if (!m_NormalImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create normal image");
        return false;
    }
    
    // Transition all to general layout
    VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
    m_AccumulationImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_AlbedoImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_NormalImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_Device->EndSingleTimeCommands(cmd);
    
    m_AccumWidth = width;
    m_AccumHeight = height;
    m_DescriptorsDirty = true;
    
    LUCENT_CORE_DEBUG("TracerRayKHR: Accumulation + AOV images created: {}x{}", width, height);
    return true;
}

bool TracerRayKHR::BuildBLAS(const std::vector<BVHBuilder::Triangle>& triangles) {
    if (triangles.empty()) return false;
    
    // Wait for GPU to finish using old buffers before rebuilding
    m_Context->WaitIdle();
    
    VkDevice device = m_Context->GetDevice();
    m_TriangleCount = static_cast<uint32_t>(triangles.size());
    
    // Create position buffer (positions only for BLAS building)
    std::vector<glm::vec3> positions;
    positions.reserve(triangles.size() * 3);
    for (const auto& tri : triangles) {
        positions.push_back(tri.v0);
        positions.push_back(tri.v1);
        positions.push_back(tri.v2);
    }
    
    // Create full vertex buffer (pos + normal + uv for shader access)
    std::vector<RTVertex> vertices;
    vertices.reserve(triangles.size() * 3);
    for (const auto& tri : triangles) {
        RTVertex v0{}, v1{}, v2{};
        v0.position = tri.v0; v0.normal = tri.n0; v0.uv = tri.uv0;
        v1.position = tri.v1; v1.normal = tri.n1; v1.uv = tri.uv1;
        v2.position = tri.v2; v2.normal = tri.n2; v2.uv = tri.uv2;
        vertices.push_back(v0);
        vertices.push_back(v1);
        vertices.push_back(v2);
    }

    // Per-primitive material ids (one per triangle, indexed by gl_PrimitiveID)
    std::vector<uint32_t> materialIds;
    materialIds.reserve(triangles.size());
    for (const auto& tri : triangles) {
        materialIds.push_back(tri.materialId);
    }
    
    // Position buffer for BLAS geometry (vec3 only)
    BufferDesc posDesc{};
    posDesc.size = positions.size() * sizeof(glm::vec3);
    posDesc.usage = BufferUsage::AccelerationStructure;
    posDesc.hostVisible = true;
    posDesc.deviceAddress = true;
    posDesc.debugName = "RTPositionBuffer";
    
    m_PositionBuffer.Shutdown();
    if (!m_PositionBuffer.Init(m_Device, posDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create position buffer");
        return false;
    }
    m_PositionBuffer.Upload(positions.data(), posDesc.size);
    
    // Full vertex buffer for shader access (RTVertex)
    BufferDesc vbDesc{};
    vbDesc.size = vertices.size() * sizeof(RTVertex);
    vbDesc.usage = BufferUsage::Storage;
    vbDesc.hostVisible = true;
    vbDesc.debugName = "RTVertexBuffer";
    
    m_VertexBuffer.Shutdown();
    if (!m_VertexBuffer.Init(m_Device, vbDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create vertex buffer");
        return false;
    }
    m_VertexBuffer.Upload(vertices.data(), vbDesc.size);
    
    // Create index buffer
    std::vector<uint32_t> indices;
    indices.reserve(triangles.size() * 3);
    for (uint32_t i = 0; i < triangles.size() * 3; i++) {
        indices.push_back(i);
    }
    
    BufferDesc ibDesc{};
    ibDesc.size = indices.size() * sizeof(uint32_t);
    ibDesc.usage = BufferUsage::AccelerationStructure;
    ibDesc.hostVisible = true;
    ibDesc.deviceAddress = true;
    ibDesc.debugName = "RTIndexBuffer";
    
    m_IndexBuffer.Shutdown();
    if (!m_IndexBuffer.Init(m_Device, ibDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create index buffer");
        return false;
    }
    m_IndexBuffer.Upload(indices.data(), ibDesc.size);

    // Create primitive material id buffer (shader-readable)
    BufferDesc pmDesc{};
    pmDesc.size = materialIds.size() * sizeof(uint32_t);
    pmDesc.usage = BufferUsage::Storage;
    pmDesc.hostVisible = true;
    pmDesc.debugName = "RTPrimitiveMaterialIds";
    m_PrimitiveMaterialBuffer.Shutdown();
    if (!m_PrimitiveMaterialBuffer.Init(m_Device, pmDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create primitive material id buffer");
        return false;
    }
    m_PrimitiveMaterialBuffer.Upload(materialIds.data(), pmDesc.size);
    
    // Geometry description (uses position buffer for BLAS, not full vertex buffer)
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexData.deviceAddress = m_PositionBuffer.GetDeviceAddress();
    geometry.geometry.triangles.vertexStride = sizeof(glm::vec3);
    geometry.geometry.triangles.maxVertex = static_cast<uint32_t>(positions.size()) - 1;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geometry.geometry.triangles.indexData.deviceAddress = m_IndexBuffer.GetDeviceAddress();
    
    // Build info
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    
    // Get build sizes
    uint32_t primitiveCount = m_TriangleCount;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &buildInfo, &primitiveCount, &sizeInfo);
    
    // Create BLAS buffer
    BufferDesc blasBufferDesc{};
    blasBufferDesc.size = sizeInfo.accelerationStructureSize;
    blasBufferDesc.usage = BufferUsage::AccelerationStructure;
    blasBufferDesc.hostVisible = false;
    blasBufferDesc.deviceAddress = true;
    blasBufferDesc.debugName = "BLAS";
    
    m_BLAS.buffer.Shutdown();
    if (!m_BLAS.buffer.Init(m_Device, blasBufferDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create BLAS buffer");
        return false;
    }
    
    // Create BLAS
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = m_BLAS.buffer.GetHandle();
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    
    if (m_BLAS.handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, m_BLAS.handle, nullptr);
    }
    
    if (vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &m_BLAS.handle) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create BLAS");
        return false;
    }
    
    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = m_BLAS.handle;
    m_BLAS.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
    m_BLAS.triangleCount = m_TriangleCount;
    
    // Create scratch buffer
    BufferDesc scratchDesc{};
    scratchDesc.size = sizeInfo.buildScratchSize;
    scratchDesc.usage = BufferUsage::Storage;
    scratchDesc.hostVisible = false;
    scratchDesc.deviceAddress = true;
    scratchDesc.debugName = "BLASScratch";
    
    Buffer scratchBuffer;
    if (!scratchBuffer.Init(m_Device, scratchDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create scratch buffer");
        return false;
    }
    
    // Build BLAS
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = m_BLAS.handle;
    buildInfo.scratchData.deviceAddress = scratchBuffer.GetDeviceAddress();
    
    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = m_TriangleCount;
    rangeInfo.primitiveOffset = 0;
    rangeInfo.firstVertex = 0;
    rangeInfo.transformOffset = 0;
    
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    
    VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    m_Device->EndSingleTimeCommands(cmd);
    
    scratchBuffer.Shutdown();
    
    LUCENT_CORE_INFO("TracerRayKHR: BLAS built: {} triangles", m_TriangleCount);
    return true;
}

bool TracerRayKHR::BuildTLAS() {
    if (m_BLAS.handle == VK_NULL_HANDLE) return false;
    
    VkDevice device = m_Context->GetDevice();
    
    // Create instance data
    VkAccelerationStructureInstanceKHR instance{};
    
    // Identity transform
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = m_BLAS.deviceAddress;
    
    // Create instance buffer
    BufferDesc instDesc{};
    instDesc.size = sizeof(VkAccelerationStructureInstanceKHR);
    instDesc.usage = BufferUsage::AccelerationStructure;
    instDesc.hostVisible = true;
    instDesc.deviceAddress = true;
    instDesc.debugName = "TLASInstances";
    
    m_TLAS.instanceBuffer.Shutdown();
    if (!m_TLAS.instanceBuffer.Init(m_Device, instDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create instance buffer");
        return false;
    }
    m_TLAS.instanceBuffer.Upload(&instance, sizeof(instance));
    
    // Geometry description
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = m_TLAS.instanceBuffer.GetDeviceAddress();
    
    // Build info
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    
    // Get build sizes
    uint32_t instanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &buildInfo, &instanceCount, &sizeInfo);
    
    // Create TLAS buffer
    BufferDesc tlasBufferDesc{};
    tlasBufferDesc.size = sizeInfo.accelerationStructureSize;
    tlasBufferDesc.usage = BufferUsage::AccelerationStructure;
    tlasBufferDesc.hostVisible = false;
    tlasBufferDesc.deviceAddress = true;
    tlasBufferDesc.debugName = "TLAS";
    
    m_TLAS.buffer.Shutdown();
    if (!m_TLAS.buffer.Init(m_Device, tlasBufferDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create TLAS buffer");
        return false;
    }
    
    // Create TLAS
    VkAccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    createInfo.buffer = m_TLAS.buffer.GetHandle();
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    
    if (m_TLAS.handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device, m_TLAS.handle, nullptr);
    }
    
    if (vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &m_TLAS.handle) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create TLAS");
        return false;
    }
    
    // Get device address
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = m_TLAS.handle;
    m_TLAS.deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
    m_TLAS.instanceCount = 1;
    
    // Create scratch buffer
    BufferDesc scratchDesc{};
    scratchDesc.size = sizeInfo.buildScratchSize;
    scratchDesc.usage = BufferUsage::Storage;
    scratchDesc.hostVisible = false;
    scratchDesc.deviceAddress = true;
    scratchDesc.debugName = "TLASScratch";
    
    Buffer scratchBuffer;
    if (!scratchBuffer.Init(m_Device, scratchDesc)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to create scratch buffer");
        return false;
    }
    
    // Build TLAS
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = m_TLAS.handle;
    buildInfo.scratchData.deviceAddress = scratchBuffer.GetDeviceAddress();
    
    VkAccelerationStructureBuildRangeInfoKHR rangeInfo{};
    rangeInfo.primitiveCount = 1;
    rangeInfo.primitiveOffset = 0;
    rangeInfo.firstVertex = 0;
    rangeInfo.transformOffset = 0;
    
    const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;
    
    VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
    m_Device->EndSingleTimeCommands(cmd);
    
    scratchBuffer.Shutdown();
    
    LUCENT_CORE_INFO("TracerRayKHR: TLAS built: {} instances", m_TLAS.instanceCount);
    return true;
}

void TracerRayKHR::UpdateScene(const std::vector<BVHBuilder::Triangle>& triangles,
                                const std::vector<GPUMaterial>& materials,
                                const std::vector<GPULight>& lights) {
    if (!m_Supported || triangles.empty()) return;
    
    // Build acceleration structures
    if (!BuildBLAS(triangles)) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to build BLAS");
        return;
    }
    
    if (!BuildTLAS()) {
        LUCENT_CORE_ERROR("TracerRayKHR: Failed to build TLAS");
        return;
    }
    
    // Upload material data
    std::vector<glm::vec4> packedMaterials;
    for (const auto& mat : materials) {
        packedMaterials.push_back(mat.baseColor);
        packedMaterials.push_back(mat.emissive);
        packedMaterials.push_back(glm::vec4(mat.metallic, mat.roughness, mat.ior, 
                                             glm::uintBitsToFloat(mat.flags)));
    }
    
    BufferDesc matDesc{};
    matDesc.size = packedMaterials.size() * sizeof(glm::vec4);
    matDesc.usage = BufferUsage::Storage;
    matDesc.hostVisible = true;
    matDesc.debugName = "RTMaterials";
    
    m_MaterialBuffer.Shutdown();
    if (m_MaterialBuffer.Init(m_Device, matDesc)) {
        m_MaterialBuffer.Upload(packedMaterials.data(), matDesc.size);
    }
    
    // Upload light data
    m_LightBuffer.Shutdown();
    size_t lightSize = std::max(lights.size(), size_t(1)) * sizeof(GPULight);
    BufferDesc lightDesc{};
    lightDesc.size = lightSize;
    lightDesc.usage = BufferUsage::Storage;
    lightDesc.hostVisible = true;
    lightDesc.debugName = "RTLights";
    
    if (m_LightBuffer.Init(m_Device, lightDesc)) {
        if (!lights.empty()) {
            m_LightBuffer.Upload(lights.data(), lights.size() * sizeof(GPULight));
            m_LightCount = static_cast<uint32_t>(lights.size());
        } else {
            // Default directional light (sun)
            GPULight defaultLight{};
            defaultLight.position = glm::vec3(0.0f);
            defaultLight.type = static_cast<uint32_t>(GPULightType::Directional);
            defaultLight.color = glm::vec3(1.0f, 0.98f, 0.95f);
            defaultLight.intensity = 2.5f;
            defaultLight.direction = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
            defaultLight.range = 0.0f;
            m_LightBuffer.Upload(&defaultLight, sizeof(GPULight));
            m_LightCount = 1;
        }
    }
    
    m_DescriptorsDirty = true;
    
    // Create pipeline and SBT if not done yet
    if (m_Pipeline == VK_NULL_HANDLE) {
        if (!CreateRayTracingPipeline()) return;
        if (!CreateShaderBindingTable()) return;
        if (!CreateDescriptorSets()) return;
    }
    
    m_Ready = true;
    LUCENT_CORE_INFO("TracerRayKHR: Scene updated with {} lights", m_LightCount);
}

void TracerRayKHR::UpdateLights(const std::vector<GPULight>& lights) {
    if (!m_Supported) return;
    if (!m_Ready) return;

    size_t lightCount = std::max(lights.size(), size_t(1));
    size_t lightSize = lightCount * sizeof(GPULight);

    if (m_LightBuffer.GetHandle() == VK_NULL_HANDLE || m_LightBuffer.GetSize() != lightSize) {
        m_LightBuffer.Shutdown();
        BufferDesc lightDesc{};
        lightDesc.size = lightSize;
        lightDesc.usage = BufferUsage::Storage;
        lightDesc.hostVisible = true;
        lightDesc.debugName = "RTLights";
        m_LightBuffer.Init(m_Device, lightDesc);
        m_DescriptorsDirty = true; // buffer handle/size changed
    }

    if (!lights.empty()) {
        m_LightBuffer.Upload(lights.data(), lights.size() * sizeof(GPULight));
        m_LightCount = static_cast<uint32_t>(lights.size());
    } else {
        // Default directional light (sun)
        GPULight defaultLight{};
        defaultLight.position = glm::vec3(0.0f);
        defaultLight.type = static_cast<uint32_t>(GPULightType::Directional);
        defaultLight.color = glm::vec3(1.0f, 0.98f, 0.95f);
        defaultLight.intensity = 2.5f;
        defaultLight.direction = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
        defaultLight.range = 0.0f;
        m_LightBuffer.Upload(&defaultLight, sizeof(GPULight));
        m_LightCount = 1;
    }

    m_DescriptorsDirty = true;
}

void TracerRayKHR::Trace(VkCommandBuffer cmd,
                          const GPUCamera& camera,
                          const RenderSettings& settings,
                          Image* outputImage) {
    if (!m_Ready || !m_Supported) return;
    
    uint32_t width = outputImage->GetWidth();
    uint32_t height = outputImage->GetHeight();
    
    // Ensure accumulation image is correct size
    if (!CreateAccumulationImage(width, height)) {
        return;
    }
    
    // Update camera
    m_CameraBuffer.Upload(&camera, sizeof(GPUCamera));

    // Update descriptors only when they actually changed (scene updated, image resized, descriptor set allocated).
    // Updating every frame can trip validation (descriptor set still in use by an in-flight command buffer).
    if (m_DescriptorsDirty) {
        VkDevice device = m_Context->GetDevice();

        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &m_TLAS.handle;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = m_AccumulationImage.GetView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo albedoInfo{};
        albedoInfo.imageView = m_AlbedoImage.GetView();
        albedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo normalInfo{};
        normalInfo.imageView = m_NormalImage.GetView();
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo vertexInfo{};
        vertexInfo.buffer = m_VertexBuffer.GetHandle();
        vertexInfo.offset = 0;
        vertexInfo.range = m_VertexBuffer.GetSize();

        VkDescriptorBufferInfo indexInfo{};
        indexInfo.buffer = m_IndexBuffer.GetHandle();
        indexInfo.offset = 0;
        indexInfo.range = m_IndexBuffer.GetSize();

        VkDescriptorBufferInfo materialInfo{};
        materialInfo.buffer = m_MaterialBuffer.GetHandle();
        materialInfo.offset = 0;
        materialInfo.range = m_MaterialBuffer.GetSize();

        VkDescriptorBufferInfo primMatInfo{};
        primMatInfo.buffer = m_PrimitiveMaterialBuffer.GetHandle();
        primMatInfo.offset = 0;
        primMatInfo.range = m_PrimitiveMaterialBuffer.GetSize();

        VkDescriptorBufferInfo cameraInfo{};
        cameraInfo.buffer = m_CameraBuffer.GetHandle();
        cameraInfo.offset = 0;
        cameraInfo.range = sizeof(GPUCamera);

        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = m_LightBuffer.GetHandle();
        lightInfo.offset = 0;
        lightInfo.range = m_LightBuffer.GetSize();

        // Environment map textures
        VkDescriptorImageInfo envMapInfo{};
        VkDescriptorImageInfo envMarginalInfo{};
        VkDescriptorImageInfo envConditionalInfo{};
        
        if (m_EnvMap && m_EnvMap->IsLoaded()) {
            envMapInfo.sampler = m_EnvMap->GetSampler();
            envMapInfo.imageView = m_EnvMap->GetEnvView();
            envMapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            
            envMarginalInfo.sampler = m_EnvMap->GetSampler();
            envMarginalInfo.imageView = m_EnvMap->GetMarginalCDFView();
            envMarginalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            
            envConditionalInfo.sampler = m_EnvMap->GetSampler();
            envConditionalInfo.imageView = m_EnvMap->GetConditionalCDFView();
            envConditionalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        VkWriteDescriptorSet writes[13] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].pNext = &asWrite;
        writes[0].dstSet = m_DescriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_DescriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &imageInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_DescriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &vertexInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_DescriptorSet;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &indexInfo;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = m_DescriptorSet;
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &materialInfo;

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = m_DescriptorSet;
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[5].pBufferInfo = &cameraInfo;

        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = m_DescriptorSet;
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pBufferInfo = &primMatInfo;

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = m_DescriptorSet;
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[7].pImageInfo = &albedoInfo;

        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = m_DescriptorSet;
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[8].pImageInfo = &normalInfo;

        writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet = m_DescriptorSet;
        writes[9].dstBinding = 9;
        writes[9].descriptorCount = 1;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[9].pBufferInfo = &lightInfo;

        uint32_t writeCount = 10;
        
        // Environment map writes - only add if we have valid views
        if (m_EnvMap && m_EnvMap->IsLoaded()) {
            writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[10].dstSet = m_DescriptorSet;
            writes[10].dstBinding = 10;
            writes[10].descriptorCount = 1;
            writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[10].pImageInfo = &envMapInfo;
            
            writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[11].dstSet = m_DescriptorSet;
            writes[11].dstBinding = 11;
            writes[11].descriptorCount = 1;
            writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[11].pImageInfo = &envMarginalInfo;
            
            writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[12].dstSet = m_DescriptorSet;
            writes[12].dstBinding = 12;
            writes[12].descriptorCount = 1;
            writes[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[12].pImageInfo = &envConditionalInfo;
            
            writeCount = 13;
        }

        vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
        m_DescriptorsDirty = false;
    }
    
    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout,
                             0, 1, &m_DescriptorSet, 0, nullptr);
    
    // Push constants
    RTPushConstants pc{};
    pc.frameIndex = m_FrameIndex;
    pc.sampleIndex = settings.accumulatedSamples;
    pc.maxBounces = settings.maxBounces;
    pc.clampValue = settings.clampIndirect;
    pc.lightCount = m_LightCount;
    pc.envIntensity = settings.envIntensity;
    pc.envRotation = settings.envRotation;
    pc.useEnvMap = (m_EnvMap && m_EnvMap->IsLoaded() && settings.useEnvMap) ? 1 : 0;
    
    vkCmdPushConstants(cmd, m_PipelineLayout, 
                        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
                        0, sizeof(RTPushConstants), &pc);
    
    // Trace rays
    vkCmdTraceRaysKHR(cmd, &m_RaygenRegion, &m_MissRegion, &m_HitRegion, &m_CallableRegion,
                       width, height, 1);
    
    // Barrier
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 1, &barrier, 0, nullptr, 0, nullptr);
    
    m_FrameIndex++;
}

void TracerRayKHR::ResetAccumulation() {
    m_FrameIndex = 0;
    
    if (m_AccumulationImage.GetHandle() != VK_NULL_HANDLE) {
        VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
        
        VkClearColorValue clearColor = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        
        vkCmdClearColorImage(cmd, m_AccumulationImage.GetHandle(), VK_IMAGE_LAYOUT_GENERAL,
                              &clearColor, 1, &range);
        
        if (m_AlbedoImage.GetHandle() != VK_NULL_HANDLE) {
            vkCmdClearColorImage(cmd, m_AlbedoImage.GetHandle(), VK_IMAGE_LAYOUT_GENERAL,
                                  &clearColor, 1, &range);
        }
        if (m_NormalImage.GetHandle() != VK_NULL_HANDLE) {
            vkCmdClearColorImage(cmd, m_NormalImage.GetHandle(), VK_IMAGE_LAYOUT_GENERAL,
                                  &clearColor, 1, &range);
        }
        
        m_Device->EndSingleTimeCommands(cmd);
    }
    
    LUCENT_CORE_DEBUG("TracerRayKHR: Accumulation reset");
}

void TracerRayKHR::SetEnvironmentMap(EnvironmentMap* envMap) {
    m_EnvMap = envMap;
    m_DescriptorsDirty = true;
}

} // namespace lucent::gfx


