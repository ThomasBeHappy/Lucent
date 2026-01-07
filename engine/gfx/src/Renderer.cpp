#include "lucent/gfx/Renderer.h"
#include "lucent/gfx/DebugUtils.h"

namespace lucent::gfx {

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Init(VulkanContext* context, Device* device, const RendererConfig& config) {
    m_Context = context;
    m_Device = device;
    m_Config = config;
    
    // Initialize debug utils
    DebugUtils::Init(context->GetInstance());
    
    // Create swapchain
    SwapchainConfig swapConfig{};
    swapConfig.width = config.width;
    swapConfig.height = config.height;
    swapConfig.vsync = config.vsync;
    
    if (!m_Swapchain.Init(context, swapConfig)) {
        LUCENT_CORE_ERROR("Failed to create swapchain");
        return false;
    }
    
    // Initialize descriptor allocator
    if (!m_DescriptorAllocator.Init(device, 100)) {
        LUCENT_CORE_ERROR("Failed to initialize descriptor allocator");
        return false;
    }
    
    if (!CreateFrameResources()) {
        LUCENT_CORE_ERROR("Failed to create frame resources");
        return false;
    }
    
    if (!CreateOffscreenResources()) {
        LUCENT_CORE_ERROR("Failed to create offscreen resources");
        return false;
    }
    
    if (!CreateSampler()) {
        LUCENT_CORE_ERROR("Failed to create sampler");
        return false;
    }
    
    // Create render passes for Vulkan 1.1/1.2 fallback
    if (!UseDynamicRendering()) {
        if (!CreateRenderPasses()) {
            LUCENT_CORE_ERROR("Failed to create render passes");
            return false;
        }
        if (!CreateFramebuffers()) {
            LUCENT_CORE_ERROR("Failed to create framebuffers");
            return false;
        }
    }
    
    if (!CreatePipelines()) {
        LUCENT_CORE_ERROR("Failed to create pipelines");
        return false;
    }
    
    LUCENT_CORE_INFO("Renderer initialized");
    return true;
}

void Renderer::Shutdown() {
    if (!m_Context) return;
    
    m_Context->WaitIdle();
    
    DestroyPipelines();
    DestroyFramebuffers();
    DestroyRenderPasses();
    DestroyOffscreenResources();
    DestroyFrameResources();
    
    m_DescriptorAllocator.Shutdown();
    m_Swapchain.Shutdown();
    
    m_Context = nullptr;
    m_Device = nullptr;
}

bool Renderer::BeginFrame() {
    if (m_NeedsResize) {
        RecreateSwapchain();
        m_NeedsResize = false;
    }
    
    VkDevice device = m_Context->GetDevice();
    FrameData& frame = m_Frames[m_CurrentFrame];
    
    // Wait for previous frame to finish
    vkWaitForFences(device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
    
    // Acquire next swapchain image
    if (!m_Swapchain.AcquireNextImage(frame.imageAvailableSemaphore, m_CurrentImageIndex)) {
        if (m_Swapchain.NeedsRecreate()) {
            RecreateSwapchain();
            return false; // Skip this frame
        }
        LUCENT_CORE_ERROR("Failed to acquire swapchain image");
        return false;
    }
    
    // Reset fence only after we know we'll submit work
    vkResetFences(device, 1, &frame.inFlightFence);
    
    // Reset and begin command buffer
    vkResetCommandBuffer(frame.commandBuffer, 0);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to begin command buffer");
        return false;
    }
    
    m_FrameStarted = true;
    return true;
}

void Renderer::EndFrame() {
    if (!m_FrameStarted) return;
    
    FrameData& frame = m_Frames[m_CurrentFrame];
    VkCommandBuffer cmd = frame.commandBuffer;
    
    // End command buffer
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to end command buffer");
        return;
    }
    
    // Submit
    VkSemaphore waitSemaphores[] = { frame.imageAvailableSemaphore };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signalSemaphores[] = { frame.renderFinishedSemaphore };
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    
    if (vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to submit command buffer");
        return;
    }
    
    // Present
    if (!m_Swapchain.Present(frame.renderFinishedSemaphore, m_CurrentImageIndex)) {
        if (m_Swapchain.NeedsRecreate()) {
            m_NeedsResize = true;
        }
    }
    
    m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    m_FrameStarted = false;
}

void Renderer::OnResize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    
    m_PendingWidth = width;
    m_PendingHeight = height;
    m_NeedsResize = true;
}

bool Renderer::CreateFrameResources() {
    VkDevice device = m_Context->GetDevice();
    
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        FrameData& frame = m_Frames[i];
        
        // Command pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_Context->GetQueueFamilies().graphics;
        
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS) {
            return false;
        }
        
        // Command buffer
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        
        if (vkAllocateCommandBuffers(device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS) {
            return false;
        }
        
        // Semaphores
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.imageAvailableSemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frame.renderFinishedSemaphore) != VK_SUCCESS) {
            return false;
        }
        
        // Fence
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        
        if (vkCreateFence(device, &fenceInfo, nullptr, &frame.inFlightFence) != VK_SUCCESS) {
            return false;
        }
        
        // Debug names
        std::string name = "Frame" + std::to_string(i);
        DebugUtils::SetObjectName(device, frame.commandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER, 
            (name + "_CommandBuffer").c_str());
    }
    
    return true;
}

bool Renderer::CreateOffscreenResources() {
    VkExtent2D extent = m_Swapchain.GetExtent();
    
    // HDR color buffer (RGBA16F)
    ImageDesc colorDesc{};
    colorDesc.width = extent.width;
    colorDesc.height = extent.height;
    colorDesc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorDesc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    colorDesc.debugName = "OffscreenColor";
    
    if (!m_OffscreenColor.Init(m_Device, colorDesc)) {
        return false;
    }
    
    // Depth buffer
    ImageDesc depthDesc{};
    depthDesc.width = extent.width;
    depthDesc.height = extent.height;
    depthDesc.format = VK_FORMAT_D32_SFLOAT;
    depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthDesc.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthDesc.debugName = "OffscreenDepth";
    
    if (!m_OffscreenDepth.Init(m_Device, depthDesc)) {
        return false;
    }
    
    // Transition images to correct layouts
    m_Device->ImmediateSubmit([this](VkCommandBuffer cmd) {
        m_OffscreenColor.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        m_OffscreenDepth.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    });
    
    return true;
}

bool Renderer::CreateSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 1.0f;
    
    if (vkCreateSampler(m_Context->GetDevice(), &samplerInfo, nullptr, &m_OffscreenSampler) != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool Renderer::CreatePipelines() {
    VkDevice device = m_Context->GetDevice();
    
    // Load shader modules
    m_TriangleVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/triangle.vert.spv");
    m_TriangleFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/triangle.frag.spv");
    m_CompositeVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/composite.vert.spv");
    m_CompositeFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/composite.frag.spv");
    
    if (!m_TriangleVertShader || !m_TriangleFragShader || !m_CompositeVertShader || !m_CompositeFragShader) {
        LUCENT_CORE_ERROR("Failed to load shaders");
        return false;
    }
    
    // Create triangle pipeline layout (empty)
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_TrianglePipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create triangle pipeline
    PipelineBuilder triangleBuilder;
    triangleBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_TriangleVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_TriangleFragShader)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .SetLayout(m_TrianglePipelineLayout);
    
    m_TrianglePipeline = triangleBuilder.Build(device);
    if (!m_TrianglePipeline) {
        return false;
    }
    
    // Load grid shaders
    m_GridVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/grid.vert.spv");
    m_GridFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/grid.frag.spv");
    
    if (!m_GridVertShader || !m_GridFragShader) {
        LUCENT_CORE_ERROR("Failed to load grid shaders");
        return false;
    }
    
    // Create grid pipeline layout with push constants for view-projection matrix
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(float) * 16; // mat4
    
    VkPipelineLayoutCreateInfo gridLayoutInfo{};
    gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    gridLayoutInfo.pushConstantRangeCount = 1;
    gridLayoutInfo.pPushConstantRanges = &pushConstant;
    
    if (vkCreatePipelineLayout(device, &gridLayoutInfo, nullptr, &m_GridPipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create grid pipeline
    PipelineBuilder gridBuilder;
    gridBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_GridVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_GridFragShader)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .SetLayout(m_GridPipelineLayout);
    
    m_GridPipeline = gridBuilder.Build(device);
    if (!m_GridPipeline) {
        return false;
    }
    
    // Load mesh shaders
    m_MeshVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/mesh.vert.spv");
    m_MeshFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/mesh.frag.spv");
    
    if (!m_MeshVertShader || !m_MeshFragShader) {
        LUCENT_CORE_ERROR("Failed to load mesh shaders");
        return false;
    }
    
    // Create mesh pipeline layout with push constants for model + viewProj matrices
    // Push constants: 2 mat4 + 4 vec4 = 192 bytes
    VkPushConstantRange meshPushConstant{};
    meshPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    meshPushConstant.offset = 0;
    meshPushConstant.size = sizeof(float) * 48; // 2 mat4 + 4 vec4 = 192 bytes
    
    VkPipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshLayoutInfo.pushConstantRangeCount = 1;
    meshLayoutInfo.pPushConstantRanges = &meshPushConstant;
    
    if (vkCreatePipelineLayout(device, &meshLayoutInfo, nullptr, &m_MeshPipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create mesh pipeline with vertex input
    // Standard mesh vertex format: position, normal, uv, tangent
    std::vector<VkVertexInputBindingDescription> meshBindings = {
        { 0, sizeof(float) * 12, VK_VERTEX_INPUT_RATE_VERTEX } // 3+3+2+4 floats
    };
    std::vector<VkVertexInputAttributeDescription> meshAttributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },                  // position
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 },  // normal
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6 },     // uv
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8 } // tangent
    };
    
    PipelineBuilder meshBuilder;
    meshBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_MeshVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_MeshFragShader)
        .SetVertexInput(meshBindings, meshAttributes)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .SetRasterizer(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .SetLayout(m_MeshPipelineLayout);
    
    m_MeshPipeline = meshBuilder.Build(device);
    if (!m_MeshPipeline) {
        return false;
    }
    
    // Create wireframe variant of mesh pipeline (same vertex input, different rasterizer)
    PipelineBuilder wireframeBuilder;
    wireframeBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_MeshVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_MeshFragShader)
        .SetVertexInput(meshBindings, meshAttributes)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .SetRasterizer(VK_POLYGON_MODE_LINE, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .SetLayout(m_MeshPipelineLayout);
    
    m_MeshWireframePipeline = wireframeBuilder.Build(device);
    if (!m_MeshWireframePipeline) {
        LUCENT_CORE_WARN("Failed to create wireframe pipeline");
        // Not fatal, continue without wireframe support
    }
    
    // Load skybox shaders
    m_SkyboxVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/skybox.vert.spv");
    m_SkyboxFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/skybox.frag.spv");
    
    if (!m_SkyboxVertShader || !m_SkyboxFragShader) {
        LUCENT_CORE_ERROR("Failed to load skybox shaders");
        return false;
    }
    
    // Create skybox pipeline layout with push constant for viewProj matrix
    VkPushConstantRange skyboxPushConstant{};
    skyboxPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    skyboxPushConstant.offset = 0;
    skyboxPushConstant.size = sizeof(float) * 16; // mat4
    
    VkPipelineLayoutCreateInfo skyboxLayoutInfo{};
    skyboxLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    skyboxLayoutInfo.pushConstantRangeCount = 1;
    skyboxLayoutInfo.pPushConstantRanges = &skyboxPushConstant;
    
    if (vkCreatePipelineLayout(device, &skyboxLayoutInfo, nullptr, &m_SkyboxPipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create skybox pipeline (no vertex input, no depth write, draw behind everything)
    PipelineBuilder skyboxBuilder;
    skyboxBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_SkyboxVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_SkyboxFragShader)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, false, VK_COMPARE_OP_LESS_OR_EQUAL) // Depth test but no write
        .SetLayout(m_SkyboxPipelineLayout);
    
    m_SkyboxPipeline = skyboxBuilder.Build(device);
    if (!m_SkyboxPipeline) {
        return false;
    }
    
    // Create composite descriptor set layout
    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_CompositeDescriptorLayout = layoutBuilder.Build(device);
    
    if (!m_CompositeDescriptorLayout) {
        return false;
    }
    
    // Create composite pipeline layout
    VkPipelineLayoutCreateInfo compositeLayoutInfo{};
    compositeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    compositeLayoutInfo.setLayoutCount = 1;
    compositeLayoutInfo.pSetLayouts = &m_CompositeDescriptorLayout;
    
    if (vkCreatePipelineLayout(device, &compositeLayoutInfo, nullptr, &m_CompositePipelineLayout) != VK_SUCCESS) {
        return false;
    }
    
    // Create composite pipeline
    PipelineBuilder compositeBuilder;
    compositeBuilder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_CompositeVertShader)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_CompositeFragShader)
        .SetColorAttachmentFormat(m_Swapchain.GetFormat())
        .SetLayout(m_CompositePipelineLayout);
    
    m_CompositePipeline = compositeBuilder.Build(device);
    if (!m_CompositePipeline) {
        return false;
    }
    
    // Allocate and update composite descriptor set
    m_CompositeDescriptorSet = m_DescriptorAllocator.Allocate(m_CompositeDescriptorLayout);
    if (!m_CompositeDescriptorSet) {
        return false;
    }
    
    DescriptorWriter writer;
    writer.WriteImage(0, m_OffscreenColor.GetView(), m_OffscreenSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writer.UpdateSet(device, m_CompositeDescriptorSet);
    
    LUCENT_CORE_DEBUG("Pipelines created");
    return true;
}

void Renderer::DestroyFrameResources() {
    VkDevice device = m_Context->GetDevice();
    
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        FrameData& frame = m_Frames[i];
        
        if (frame.inFlightFence != VK_NULL_HANDLE) {
            vkDestroyFence(device, frame.inFlightFence, nullptr);
        }
        if (frame.renderFinishedSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.renderFinishedSemaphore, nullptr);
        }
        if (frame.imageAvailableSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, frame.imageAvailableSemaphore, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, frame.commandPool, nullptr);
        }
        
        frame = {};
    }
}

void Renderer::DestroyOffscreenResources() {
    m_OffscreenColor.Shutdown();
    m_OffscreenDepth.Shutdown();
    
    if (m_OffscreenSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_Context->GetDevice(), m_OffscreenSampler, nullptr);
        m_OffscreenSampler = VK_NULL_HANDLE;
    }
}

void Renderer::DestroyPipelines() {
    VkDevice device = m_Context->GetDevice();
    
    if (m_TrianglePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_TrianglePipeline, nullptr);
        m_TrianglePipeline = VK_NULL_HANDLE;
    }
    if (m_TrianglePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_TrianglePipelineLayout, nullptr);
        m_TrianglePipelineLayout = VK_NULL_HANDLE;
    }
    
    if (m_CompositePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_CompositePipeline, nullptr);
        m_CompositePipeline = VK_NULL_HANDLE;
    }
    if (m_CompositePipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_CompositePipelineLayout, nullptr);
        m_CompositePipelineLayout = VK_NULL_HANDLE;
    }
    if (m_CompositeDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_CompositeDescriptorLayout, nullptr);
        m_CompositeDescriptorLayout = VK_NULL_HANDLE;
    }
    
    // Destroy shader modules
    if (m_TriangleVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_TriangleVertShader, nullptr);
        m_TriangleVertShader = VK_NULL_HANDLE;
    }
    if (m_TriangleFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_TriangleFragShader, nullptr);
        m_TriangleFragShader = VK_NULL_HANDLE;
    }
    if (m_CompositeVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_CompositeVertShader, nullptr);
        m_CompositeVertShader = VK_NULL_HANDLE;
    }
    if (m_CompositeFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_CompositeFragShader, nullptr);
        m_CompositeFragShader = VK_NULL_HANDLE;
    }
    
    // Grid pipeline
    if (m_GridPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_GridPipeline, nullptr);
        m_GridPipeline = VK_NULL_HANDLE;
    }
    if (m_GridPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_GridPipelineLayout, nullptr);
        m_GridPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_GridVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_GridVertShader, nullptr);
        m_GridVertShader = VK_NULL_HANDLE;
    }
    if (m_GridFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_GridFragShader, nullptr);
        m_GridFragShader = VK_NULL_HANDLE;
    }
    
    // Mesh pipeline
    if (m_MeshPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_MeshPipeline, nullptr);
        m_MeshPipeline = VK_NULL_HANDLE;
    }
    if (m_MeshWireframePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_MeshWireframePipeline, nullptr);
        m_MeshWireframePipeline = VK_NULL_HANDLE;
    }
    if (m_MeshPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_MeshPipelineLayout, nullptr);
        m_MeshPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_MeshVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_MeshVertShader, nullptr);
        m_MeshVertShader = VK_NULL_HANDLE;
    }
    if (m_MeshFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_MeshFragShader, nullptr);
        m_MeshFragShader = VK_NULL_HANDLE;
    }
    
    // Skybox pipeline
    if (m_SkyboxPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_SkyboxPipeline, nullptr);
        m_SkyboxPipeline = VK_NULL_HANDLE;
    }
    if (m_SkyboxPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_SkyboxPipelineLayout, nullptr);
        m_SkyboxPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_SkyboxVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_SkyboxVertShader, nullptr);
        m_SkyboxVertShader = VK_NULL_HANDLE;
    }
    if (m_SkyboxFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_SkyboxFragShader, nullptr);
        m_SkyboxFragShader = VK_NULL_HANDLE;
    }
}

void Renderer::RecreateSwapchain() {
    m_Context->WaitIdle();
    
    uint32_t width = m_PendingWidth > 0 ? m_PendingWidth : m_Swapchain.GetExtent().width;
    uint32_t height = m_PendingHeight > 0 ? m_PendingHeight : m_Swapchain.GetExtent().height;
    
    if (width == 0 || height == 0) return;
    
    // Recreate swapchain
    if (!m_Swapchain.Recreate(width, height)) {
        LUCENT_CORE_ERROR("Failed to recreate swapchain");
        return;
    }
    
    // Recreate offscreen resources
    DestroyOffscreenResources();
    CreateOffscreenResources();
    CreateSampler();
    
    // Update descriptor set
    DescriptorWriter writer;
    writer.WriteImage(0, m_OffscreenColor.GetView(), m_OffscreenSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writer.UpdateSet(m_Context->GetDevice(), m_CompositeDescriptorSet);
    
    m_PendingWidth = 0;
    m_PendingHeight = 0;
    
    LUCENT_CORE_INFO("Swapchain recreated: {}x{}", width, height);
}

} // namespace lucent::gfx

