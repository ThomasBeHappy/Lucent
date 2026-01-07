#include "lucent/gfx/TracerCompute.h"
#include "lucent/gfx/PipelineBuilder.h"
#include "lucent/core/Log.h"
#include <algorithm>

namespace lucent::gfx {

// ============================================================================
// BVHBuilder Implementation
// ============================================================================

void BVHBuilder::Build(const std::vector<Triangle>& triangles) {
    if (triangles.empty()) return;
    
    m_Triangles = triangles;
    m_TriangleIndices.resize(triangles.size());
    for (size_t i = 0; i < triangles.size(); i++) {
        m_TriangleIndices[i] = static_cast<uint32_t>(i);
    }
    
    // Reserve space for nodes (worst case: 2N - 1 nodes)
    m_Nodes.clear();
    m_Nodes.reserve(2 * triangles.size());
    
    // Create root node
    BVHNode root{};
    root.leftFirst = 0;
    root.count = static_cast<uint32_t>(triangles.size());
    
    // Compute root bounds
    root.aabbMin = glm::vec3(FLT_MAX);
    root.aabbMax = glm::vec3(-FLT_MAX);
    for (const auto& tri : triangles) {
        root.aabbMin = glm::min(root.aabbMin, glm::min(tri.v0, glm::min(tri.v1, tri.v2)));
        root.aabbMax = glm::max(root.aabbMax, glm::max(tri.v0, glm::max(tri.v1, tri.v2)));
    }
    
    m_Nodes.push_back(root);
    
    // Build recursively
    BuildRecursive(0, 0, static_cast<uint32_t>(triangles.size()));
    
    LUCENT_CORE_DEBUG("BVH built: {} nodes, {} triangles", m_Nodes.size(), triangles.size());
}

void BVHBuilder::BuildRecursive(uint32_t nodeIdx, uint32_t start, uint32_t end) {
    BVHNode& node = m_Nodes[nodeIdx];
    uint32_t count = end - start;
    
    // Leaf threshold
    if (count <= 4) {
        node.leftFirst = start;
        node.count = count;
        return;
    }
    
    // Find best split (simple midpoint split for now)
    glm::vec3 extent = node.aabbMax - node.aabbMin;
    int axis = 0;
    if (extent.y > extent.x) axis = 1;
    if (extent.z > extent[axis]) axis = 2;
    
    float splitPos = (node.aabbMin[axis] + node.aabbMax[axis]) * 0.5f;
    
    // Partition triangles
    uint32_t mid = start;
    for (uint32_t i = start; i < end; i++) {
        const Triangle& tri = m_Triangles[m_TriangleIndices[i]];
        glm::vec3 centroid = (tri.v0 + tri.v1 + tri.v2) / 3.0f;
        if (centroid[axis] < splitPos) {
            std::swap(m_TriangleIndices[i], m_TriangleIndices[mid]);
            mid++;
        }
    }
    
    // Handle degenerate splits
    if (mid == start || mid == end) {
        mid = start + count / 2;
    }
    
    // Create child nodes
    uint32_t leftIdx = static_cast<uint32_t>(m_Nodes.size());
    m_Nodes.push_back({});
    uint32_t rightIdx = static_cast<uint32_t>(m_Nodes.size());
    m_Nodes.push_back({});
    
    // Update parent as internal node
    node.leftFirst = leftIdx;
    node.count = 0;
    
    // Compute child bounds
    BVHNode& left = m_Nodes[leftIdx];
    BVHNode& right = m_Nodes[rightIdx];
    
    left.aabbMin = glm::vec3(FLT_MAX);
    left.aabbMax = glm::vec3(-FLT_MAX);
    for (uint32_t i = start; i < mid; i++) {
        const Triangle& tri = m_Triangles[m_TriangleIndices[i]];
        left.aabbMin = glm::min(left.aabbMin, glm::min(tri.v0, glm::min(tri.v1, tri.v2)));
        left.aabbMax = glm::max(left.aabbMax, glm::max(tri.v0, glm::max(tri.v1, tri.v2)));
    }
    
    right.aabbMin = glm::vec3(FLT_MAX);
    right.aabbMax = glm::vec3(-FLT_MAX);
    for (uint32_t i = mid; i < end; i++) {
        const Triangle& tri = m_Triangles[m_TriangleIndices[i]];
        right.aabbMin = glm::min(right.aabbMin, glm::min(tri.v0, glm::min(tri.v1, tri.v2)));
        right.aabbMax = glm::max(right.aabbMax, glm::max(tri.v0, glm::max(tri.v1, tri.v2)));
    }
    
    // Recurse
    BuildRecursive(leftIdx, start, mid);
    BuildRecursive(rightIdx, mid, end);
}

// ============================================================================
// TracerCompute Implementation
// ============================================================================

TracerCompute::~TracerCompute() {
    Shutdown();
}

bool TracerCompute::Init(VulkanContext* context, Device* device) {
    m_Context = context;
    m_Device = device;
    
    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 },  // accumImage + albedoImage + normalImage
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 }, // triangles + bvh + instances + materials + lights
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }
    };
    
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    
    if (vkCreateDescriptorPool(context->GetDevice(), &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create tracer descriptor pool");
        return false;
    }
    
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding bindings[] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },      // accumImage
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },     // triangles
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },     // bvhNodes
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },     // instances
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },     // materials
        { 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },     // camera
        { 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },      // albedoImage
        { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },      // normalImage
        { 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }      // lights
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 9;
    layoutInfo.pBindings = bindings;
    
    if (vkCreateDescriptorSetLayout(context->GetDevice(), &layoutInfo, nullptr, &m_DescriptorLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create tracer descriptor set layout");
        return false;
    }
    
    // Create camera UBO
    BufferDesc cameraBufferDesc{};
    cameraBufferDesc.size = sizeof(GPUCamera);
    cameraBufferDesc.usage = BufferUsage::Uniform;
    cameraBufferDesc.hostVisible = true;
    cameraBufferDesc.debugName = "TracerCameraUBO";
    m_CameraBuffer.Init(device, cameraBufferDesc);
    
    if (!CreateComputePipeline()) {
        LUCENT_CORE_ERROR("Failed to create tracer compute pipeline");
        return false;
    }
    
    LUCENT_CORE_INFO("TracerCompute initialized");
    return true;
}

void TracerCompute::Shutdown() {
    VkDevice device = m_Context ? m_Context->GetDevice() : VK_NULL_HANDLE;
    if (!device) return;
    
    m_Context->WaitIdle();
    
    // Destroy scene buffers
    m_SceneGPU.triangleBuffer.Shutdown();
    m_SceneGPU.bvhNodeBuffer.Shutdown();
    m_SceneGPU.instanceBuffer.Shutdown();
    m_SceneGPU.materialBuffer.Shutdown();
    
    m_AccumulationImage.Shutdown();
    m_AlbedoImage.Shutdown();
    m_NormalImage.Shutdown();
    m_CameraBuffer.Shutdown();
    
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_Pipeline, nullptr);
    }
    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
    }
    if (m_ComputeShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_ComputeShader, nullptr);
    }
    if (m_DescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_DescriptorLayout, nullptr);
    }
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
    }
    
    m_Ready = false;
}

bool TracerCompute::CreateComputePipeline() {
    VkDevice device = m_Context->GetDevice();
    
    // Load compute shader
    m_ComputeShader = PipelineBuilder::LoadShaderModule(device, "shaders/traced.comp.spv");
    if (!m_ComputeShader) {
        LUCENT_CORE_ERROR("Failed to load traced.comp shader");
        return false;
    }
    
    // Create pipeline layout
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(TracerPushConstants);
    
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_DescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = m_PipelineLayout;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = m_ComputeShader;
    pipelineInfo.stage.pName = "main";
    
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline) != VK_SUCCESS) {
        return false;
    }
    
    LUCENT_CORE_DEBUG("TracerCompute pipeline created");
    return true;
}

bool TracerCompute::CreateDescriptorSets() {
    VkDevice device = m_Context->GetDevice();
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_DescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_DescriptorLayout;
    
    if (vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to allocate tracer descriptor set");
        return false;
    }
    
    return true;
}

bool TracerCompute::CreateAccumulationImage(uint32_t width, uint32_t height) {
    if (width == m_AccumWidth && height == m_AccumHeight && m_AccumulationImage.GetHandle() != VK_NULL_HANDLE) {
        return true;
    }
    
    m_AccumulationImage.Shutdown();
    m_AlbedoImage.Shutdown();
    m_NormalImage.Shutdown();
    
    ImageDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = VK_FORMAT_R32G32B32A32_SFLOAT;  // HDR accumulation
    desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    desc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    desc.debugName = "TracerAccumulationImage";
    
    if (!m_AccumulationImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("Failed to create tracer accumulation image");
        return false;
    }
    
    // Create AOV images for denoiser
    desc.debugName = "TracerAlbedoImage";
    if (!m_AlbedoImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("Failed to create tracer albedo image");
        return false;
    }
    
    desc.debugName = "TracerNormalImage";
    if (!m_NormalImage.Init(m_Device, desc)) {
        LUCENT_CORE_ERROR("Failed to create tracer normal image");
        return false;
    }
    
    // Transition to general layout for compute
    VkCommandBuffer cmd = m_Device->BeginSingleTimeCommands();
    m_AccumulationImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_AlbedoImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_NormalImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_Device->EndSingleTimeCommands(cmd);
    
    m_AccumWidth = width;
    m_AccumHeight = height;
    m_DescriptorsDirty = true;  // Accumulation image changed, need to update descriptors
    
    LUCENT_CORE_DEBUG("TracerCompute accumulation + AOV images created: {}x{}", width, height);
    return true;
}

void TracerCompute::UpdateDescriptors() {
    VkDevice device = m_Context->GetDevice();
    
    // Accumulation image
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = m_AccumulationImage.GetView();
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    
    // AOV images for denoiser
    VkDescriptorImageInfo albedoInfo{};
    albedoInfo.imageView = m_AlbedoImage.GetView();
    albedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    
    VkDescriptorImageInfo normalInfo{};
    normalInfo.imageView = m_NormalImage.GetView();
    normalInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    
    // Scene buffers (use dummy if not ready)
    VkDescriptorBufferInfo triangleInfo{};
    triangleInfo.buffer = m_SceneGPU.triangleBuffer.GetHandle();
    triangleInfo.offset = 0;
    triangleInfo.range = m_SceneGPU.triangleBuffer.GetSize();
    
    VkDescriptorBufferInfo bvhInfo{};
    bvhInfo.buffer = m_SceneGPU.bvhNodeBuffer.GetHandle();
    bvhInfo.offset = 0;
    bvhInfo.range = m_SceneGPU.bvhNodeBuffer.GetSize();
    
    VkDescriptorBufferInfo instanceInfo{};
    instanceInfo.buffer = m_SceneGPU.instanceBuffer.GetHandle();
    instanceInfo.offset = 0;
    instanceInfo.range = m_SceneGPU.instanceBuffer.GetSize();
    
    VkDescriptorBufferInfo materialInfo{};
    materialInfo.buffer = m_SceneGPU.materialBuffer.GetHandle();
    materialInfo.offset = 0;
    materialInfo.range = m_SceneGPU.materialBuffer.GetSize();
    
    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = m_CameraBuffer.GetHandle();
    cameraInfo.offset = 0;
    cameraInfo.range = sizeof(GPUCamera);
    
    VkDescriptorBufferInfo lightInfo{};
    lightInfo.buffer = m_SceneGPU.lightBuffer.GetHandle();
    lightInfo.offset = 0;
    lightInfo.range = m_SceneGPU.lightBuffer.GetSize();
    
    VkWriteDescriptorSet writes[9] = {};
    
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_DescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imageInfo;
    
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_DescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &triangleInfo;
    
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_DescriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &bvhInfo;
    
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_DescriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &instanceInfo;
    
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
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[6].pImageInfo = &albedoInfo;
    
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_DescriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &normalInfo;
    
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_DescriptorSet;
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &lightInfo;
    
    vkUpdateDescriptorSets(device, 9, writes, 0, nullptr);
}

void TracerCompute::UpdateScene(const std::vector<BVHBuilder::Triangle>& inputTriangles,
                                 const std::vector<GPUMaterial>& inputMaterials,
                                 const std::vector<GPULight>& inputLights) {
    std::vector<BVHBuilder::Triangle> triangles = inputTriangles;
    std::vector<GPUMaterial> materials = inputMaterials;
    
    // Ensure we have at least a default material
    if (materials.empty()) {
        GPUMaterial defaultMat{};
        defaultMat.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        defaultMat.emissive = glm::vec4(0.0f);
        defaultMat.metallic = 0.0f;
        defaultMat.roughness = 0.5f;
        defaultMat.ior = 1.5f;
        defaultMat.flags = 0;
        materials.push_back(defaultMat);
    }
    
    if (triangles.empty()) {
        // Add a dummy triangle to prevent empty buffer issues
        BVHBuilder::Triangle dummy{};
        dummy.v0 = glm::vec3(0, -1000, 0);
        dummy.v1 = glm::vec3(1, -1000, 0);
        dummy.v2 = glm::vec3(0, -1000, 1);
        dummy.materialId = 0;
        triangles.push_back(dummy);
    }
    
    // Build BVH
    BVHBuilder builder;
    builder.Build(triangles);
    
    // Pack triangle data for GPU (3 vec4s per triangle)
    std::vector<glm::vec4> packedTriangles;
    for (size_t i = 0; i < triangles.size(); i++) {
        const auto& tri = triangles[builder.GetTriangleIndices()[i]];
        
        // vec4(v0.xyz, materialId)
        packedTriangles.push_back(glm::vec4(tri.v0, glm::uintBitsToFloat(tri.materialId)));
        // vec4(v1.xyz, pad)
        packedTriangles.push_back(glm::vec4(tri.v1, 0.0f));
        // vec4(v2.xyz, pad)
        packedTriangles.push_back(glm::vec4(tri.v2, 0.0f));
    }
    
    // Pack BVH nodes (2 vec4s per node)
    const auto& nodes = builder.GetNodes();
    std::vector<glm::vec4> packedNodes;
    for (const auto& node : nodes) {
        packedNodes.push_back(glm::vec4(node.aabbMin, glm::uintBitsToFloat(node.leftFirst)));
        packedNodes.push_back(glm::vec4(node.aabbMax, glm::uintBitsToFloat(node.count)));
    }
    
    // Pack materials (3 vec4s per material)
    std::vector<glm::vec4> packedMaterials;
    for (const auto& mat : materials) {
        packedMaterials.push_back(mat.baseColor);
        packedMaterials.push_back(mat.emissive);
        packedMaterials.push_back(glm::vec4(mat.metallic, mat.roughness, mat.ior, glm::uintBitsToFloat(mat.flags)));
    }
    
    // Create/resize GPU buffers
    size_t triSize = packedTriangles.size() * sizeof(glm::vec4);
    size_t bvhSize = packedNodes.size() * sizeof(glm::vec4);
    size_t matSize = packedMaterials.size() * sizeof(glm::vec4);
    size_t instSize = sizeof(glm::mat4); // Dummy instance buffer
    size_t lightSize = std::max(inputLights.size(), size_t(1)) * sizeof(GPULight);
    
    m_SceneGPU.triangleBuffer.Shutdown();
    m_SceneGPU.bvhNodeBuffer.Shutdown();
    m_SceneGPU.instanceBuffer.Shutdown();
    m_SceneGPU.materialBuffer.Shutdown();
    m_SceneGPU.lightBuffer.Shutdown();
    
    BufferDesc triDesc{};
    triDesc.size = triSize;
    triDesc.usage = BufferUsage::Storage;
    triDesc.hostVisible = true;
    triDesc.debugName = "TracerTriangles";
    m_SceneGPU.triangleBuffer.Init(m_Device, triDesc);
    
    BufferDesc bvhDesc{};
    bvhDesc.size = bvhSize;
    bvhDesc.usage = BufferUsage::Storage;
    bvhDesc.hostVisible = true;
    bvhDesc.debugName = "TracerBVH";
    m_SceneGPU.bvhNodeBuffer.Init(m_Device, bvhDesc);
    
    BufferDesc instDesc{};
    instDesc.size = instSize;
    instDesc.usage = BufferUsage::Storage;
    instDesc.hostVisible = true;
    instDesc.debugName = "TracerInstances";
    m_SceneGPU.instanceBuffer.Init(m_Device, instDesc);
    
    BufferDesc matDesc{};
    matDesc.size = matSize;
    matDesc.usage = BufferUsage::Storage;
    matDesc.hostVisible = true;
    matDesc.debugName = "TracerMaterials";
    m_SceneGPU.materialBuffer.Init(m_Device, matDesc);
    
    BufferDesc lightDesc{};
    lightDesc.size = lightSize;
    lightDesc.usage = BufferUsage::Storage;
    lightDesc.hostVisible = true;
    lightDesc.debugName = "TracerLights";
    m_SceneGPU.lightBuffer.Init(m_Device, lightDesc);
    
    // Upload data
    m_SceneGPU.triangleBuffer.Upload(packedTriangles.data(), triSize);
    m_SceneGPU.bvhNodeBuffer.Upload(packedNodes.data(), bvhSize);
    m_SceneGPU.materialBuffer.Upload(packedMaterials.data(), matSize);
    
    glm::mat4 identity(1.0f);
    m_SceneGPU.instanceBuffer.Upload(&identity, sizeof(glm::mat4));
    
    // Upload lights (or a dummy light if empty)
    if (!inputLights.empty()) {
        m_SceneGPU.lightBuffer.Upload(inputLights.data(), inputLights.size() * sizeof(GPULight));
        m_SceneGPU.lightCount = static_cast<uint32_t>(inputLights.size());
    } else {
        // Default directional light (sun)
        GPULight defaultLight{};
        defaultLight.position = glm::vec3(0.0f); // Not used for directional
        defaultLight.type = static_cast<uint32_t>(GPULightType::Directional);
        defaultLight.color = glm::vec3(1.0f, 0.98f, 0.95f);
        defaultLight.intensity = 2.5f;
        defaultLight.direction = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
        defaultLight.range = 0.0f;
        m_SceneGPU.lightBuffer.Upload(&defaultLight, sizeof(GPULight));
        m_SceneGPU.lightCount = 1;
    }
    
    m_SceneGPU.triangleCount = static_cast<uint32_t>(triangles.size());
    m_SceneGPU.bvhNodeCount = static_cast<uint32_t>(nodes.size());
    m_SceneGPU.materialCount = static_cast<uint32_t>(materials.size());
    m_SceneGPU.instanceCount = 1;
    m_SceneGPU.valid = true;
    
    m_SceneDirty = false;
    m_DescriptorsDirty = true;  // Scene buffers changed, need to update descriptors
    
    LUCENT_CORE_INFO("TracerCompute scene updated: {} triangles, {} BVH nodes, {} materials, {} lights",
        m_SceneGPU.triangleCount, m_SceneGPU.bvhNodeCount, m_SceneGPU.materialCount, m_SceneGPU.lightCount);
}

void TracerCompute::Trace(VkCommandBuffer cmd, 
                           const GPUCamera& camera,
                           const RenderSettings& settings,
                           Image* outputImage) {
    if (!m_SceneGPU.valid) return;
    
    uint32_t width = outputImage->GetWidth();
    uint32_t height = outputImage->GetHeight();
    
    // Ensure accumulation image is correct size
    if (!CreateAccumulationImage(width, height)) {
        return;
    }
    
    // Create descriptor set if needed
    if (m_DescriptorSet == VK_NULL_HANDLE) {
        if (!CreateDescriptorSets()) {
            return;
        }
        m_DescriptorsDirty = true;
    }
    
    // Update camera UBO (buffer contents, not descriptor)
    m_CameraBuffer.Upload(&camera, sizeof(GPUCamera));
    
    // Only update descriptors when they actually changed (scene updated, image resized)
    // Updating every frame causes validation errors when descriptor is still in use
    if (m_DescriptorsDirty) {
        UpdateDescriptors();
        m_DescriptorsDirty = false;
    }
    
    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 
        0, 1, &m_DescriptorSet, 0, nullptr);
    
    // Push constants
    TracerPushConstants pc{};
    pc.frameIndex = m_FrameIndex;
    pc.sampleIndex = settings.accumulatedSamples;
    pc.maxBounces = settings.maxBounces;
    pc.clampValue = settings.clampIndirect;
    
    vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 
        0, sizeof(TracerPushConstants), &pc);
    
    // Dispatch
    uint32_t groupX = (width + 7) / 8;
    uint32_t groupY = (height + 7) / 8;
    vkCmdDispatch(cmd, groupX, groupY, 1);
    
    // Memory barrier for accumulation image
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    
    m_FrameIndex++;
    m_Ready = true;
}

void TracerCompute::ResetAccumulation() {
    m_FrameIndex = 0;
    
    // Clear accumulation and AOV images if they exist
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
    
    LUCENT_CORE_DEBUG("TracerCompute accumulation reset");
}

} // namespace lucent::gfx

