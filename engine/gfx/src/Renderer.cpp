#include "lucent/gfx/Renderer.h"
#include "lucent/gfx/DebugUtils.h"
#include <array>

namespace lucent::gfx {

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Init(VulkanContext* context, Device* device, const RendererConfig& config) {
    m_Context = context;
    m_Device = device;
    m_Config = config;
    
    // Build render capabilities from device features
    m_Capabilities = RenderCapabilities::FromDeviceFeatures(
        context->GetDeviceFeatures(), 
        VK_API_VERSION_1_3  // We request 1.3, actual support may be lower
    );
    
    // Select best available render mode
    m_RenderMode = m_Capabilities.GetBestMode();
    LUCENT_CORE_INFO("Initial render mode: {}", RenderModeName(m_RenderMode));
    
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
    
    // Create per-swapchain-image semaphores to avoid semaphore reuse before present completes
    m_ImageRenderFinishedSemaphores.resize(m_Swapchain.GetImageCount());
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < m_ImageRenderFinishedSemaphores.size(); i++) {
        if (vkCreateSemaphore(context->GetDevice(), &semaphoreInfo, nullptr, &m_ImageRenderFinishedSemaphores[i]) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create per-image semaphore");
            return false;
        }
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
    
    if (!CreateShadowResources()) {
        LUCENT_CORE_ERROR("Failed to create shadow resources");
        return false;
    }
    
    // Initialize compute tracer if Traced mode is available
    if (m_Capabilities.tracedAvailable) {
        m_TracerCompute = std::make_unique<TracerCompute>();
        if (!m_TracerCompute->Init(m_Context, m_Device)) {
            LUCENT_CORE_WARN("Failed to initialize compute tracer, Traced mode disabled");
            m_TracerCompute.reset();
            m_Capabilities.tracedAvailable = false;
            if (m_RenderMode == RenderMode::Traced) {
                m_RenderMode = RenderMode::Simple;
            }
        }
    }
    
    // Initialize KHR ray tracer if RayTraced mode is available
    if (m_Capabilities.rayTracedAvailable) {
        m_TracerRayKHR = std::make_unique<TracerRayKHR>();
        if (!m_TracerRayKHR->Init(m_Context, m_Device)) {
            LUCENT_CORE_WARN("Failed to initialize KHR ray tracer, RayTraced mode disabled");
            m_TracerRayKHR.reset();
            m_Capabilities.rayTracedAvailable = false;
            if (m_RenderMode == RenderMode::RayTraced) {
                m_RenderMode = m_Capabilities.tracedAvailable ? RenderMode::Traced : RenderMode::Simple;
            }
        }
    }
    
    // Initialize final render (always available if any tracer is available)
    if (m_Capabilities.tracedAvailable || m_Capabilities.rayTracedAvailable) {
        m_FinalRender = std::make_unique<FinalRender>();
        if (!m_FinalRender->Init(this)) {
            LUCENT_CORE_WARN("Failed to initialize final render");
            m_FinalRender.reset();
        }
    }
    
    LUCENT_CORE_INFO("Renderer initialized");
    return true;
}

void Renderer::Shutdown() {
    if (!m_Context) return;
    
    m_Context->WaitIdle();
    
    // Shutdown tracers
    if (m_TracerCompute) {
        m_TracerCompute->Shutdown();
        m_TracerCompute.reset();
    }
    
    if (m_TracerRayKHR) {
        m_TracerRayKHR->Shutdown();
        m_TracerRayKHR.reset();
    }
    
    if (m_FinalRender) {
        m_FinalRender->Shutdown();
        m_FinalRender.reset();
    }
    
    DestroyShadowResources();
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
    
    // Submit - use per-image semaphore for present to avoid reuse before present completes
    VkSemaphore waitSemaphores[] = { frame.imageAvailableSemaphore };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore renderFinishedSemaphore = m_ImageRenderFinishedSemaphores[m_CurrentImageIndex];
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinishedSemaphore;
    
    if (vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, frame.inFlightFence) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to submit command buffer");
        return;
    }
    
    // Present
    if (!m_Swapchain.Present(renderFinishedSemaphore, m_CurrentImageIndex)) {
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
    colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | 
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    // Note: Offscreen color starts in SHADER_READ_ONLY_OPTIMAL so BeginOffscreenPass can transition it properly
    m_Device->ImmediateSubmit([this](VkCommandBuffer cmd) {
        m_OffscreenColor.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    if (!UseDynamicRendering()) {
        triangleBuilder.SetRenderPass(m_OffscreenRenderPass);
    }
    
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
    if (!UseDynamicRendering()) {
        gridBuilder.SetRenderPass(m_OffscreenRenderPass);
    }
    
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
    
    // Create mesh pipeline layout with push constants for model + viewProj + lightVP matrices
    // Push constants: 3 mat4 + 4 vec4 = 256 bytes
    VkPushConstantRange meshPushConstant{};
    meshPushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    meshPushConstant.offset = 0;
    meshPushConstant.size = sizeof(float) * 64; // 3 mat4 + 4 vec4 = 256 bytes
    
    // Create descriptor set layout for shadow map (set 0, binding 0)
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 0;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    shadowLayoutInfo.bindingCount = 1;
    shadowLayoutInfo.pBindings = &shadowBinding;
    
    if (vkCreateDescriptorSetLayout(device, &shadowLayoutInfo, nullptr, &m_MeshDescriptorLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create mesh descriptor layout");
        return false;
    }
    
    VkPipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshLayoutInfo.setLayoutCount = 1;
    meshLayoutInfo.pSetLayouts = &m_MeshDescriptorLayout;
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
    if (!UseDynamicRendering()) {
        meshBuilder.SetRenderPass(m_OffscreenRenderPass);
    }
    
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
    if (!UseDynamicRendering()) {
        wireframeBuilder.SetRenderPass(m_OffscreenRenderPass);
    }
    
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
    if (!UseDynamicRendering()) {
        skyboxBuilder.SetRenderPass(m_OffscreenRenderPass);
    }
    
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
    
    // Create composite pipeline layout with push constants for PostFX settings
    VkPushConstantRange compositePushConstant{};
    compositePushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    compositePushConstant.offset = 0;
    compositePushConstant.size = sizeof(float) * 4; // exposure, tonemapMode, gamma, unused
    
    VkPipelineLayoutCreateInfo compositeLayoutInfo{};
    compositeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    compositeLayoutInfo.setLayoutCount = 1;
    compositeLayoutInfo.pSetLayouts = &m_CompositeDescriptorLayout;
    compositeLayoutInfo.pushConstantRangeCount = 1;
    compositeLayoutInfo.pPushConstantRanges = &compositePushConstant;
    
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
    if (!UseDynamicRendering()) {
        compositeBuilder.SetRenderPass(m_SwapchainRenderPass);
    }
    
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
    
    // Create PostFX pipeline
    m_PostFXVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/postfx.vert.spv");
    m_PostFXFragShader = PipelineBuilder::LoadShaderModule(device, "shaders/postfx.frag.spv");
    
    if (!m_PostFXVertShader || !m_PostFXFragShader) {
        LUCENT_CORE_WARN("PostFX shaders not found, post-processing disabled");
    } else {
        // PostFX descriptor layout (same as composite - just samples HDR image)
        DescriptorLayoutBuilder postfxLayoutBuilder;
        postfxLayoutBuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
        m_PostFXDescriptorLayout = postfxLayoutBuilder.Build(device);
        
        // PostFX pipeline layout with push constants for settings
        VkPushConstantRange postfxPushConstant{};
        postfxPushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        postfxPushConstant.offset = 0;
        postfxPushConstant.size = sizeof(float) * 4; // exposure, tonemapMode, gamma, unused
        
        VkPipelineLayoutCreateInfo postfxLayoutInfo{};
        postfxLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        postfxLayoutInfo.setLayoutCount = 1;
        postfxLayoutInfo.pSetLayouts = &m_PostFXDescriptorLayout;
        postfxLayoutInfo.pushConstantRangeCount = 1;
        postfxLayoutInfo.pPushConstantRanges = &postfxPushConstant;
        
        vkCreatePipelineLayout(device, &postfxLayoutInfo, nullptr, &m_PostFXPipelineLayout);
        
        // Build PostFX pipeline
        PipelineBuilder postfxBuilder;
        postfxBuilder
            .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_PostFXVertShader)
            .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_PostFXFragShader)
            .SetInputAssembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .SetColorAttachmentFormat(m_Swapchain.GetFormat())
            .SetDepthStencil(false, false, VK_COMPARE_OP_ALWAYS)
            .SetLayout(m_PostFXPipelineLayout);
        if (!UseDynamicRendering()) {
            postfxBuilder.SetRenderPass(m_SwapchainRenderPass);
        }
        
        m_PostFXPipeline = postfxBuilder.Build(device);
        
        // Allocate PostFX descriptor set
        m_PostFXDescriptorSet = m_DescriptorAllocator.Allocate(m_PostFXDescriptorLayout);
        DescriptorWriter postfxWriter;
        postfxWriter.WriteImage(0, m_OffscreenColor.GetView(), m_OffscreenSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        postfxWriter.UpdateSet(device, m_PostFXDescriptorSet);
        
        LUCENT_CORE_INFO("PostFX pipeline created");
    }
    
    LUCENT_CORE_DEBUG("Pipelines created");
    return true;
}

void Renderer::DestroyFrameResources() {
    VkDevice device = m_Context->GetDevice();
    
    // Destroy per-image semaphores
    for (auto semaphore : m_ImageRenderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
    m_ImageRenderFinishedSemaphores.clear();
    
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
    if (m_MeshDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_MeshDescriptorLayout, nullptr);
        m_MeshDescriptorLayout = VK_NULL_HANDLE;
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
    
    // PostFX cleanup
    if (m_PostFXPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_PostFXPipeline, nullptr);
        m_PostFXPipeline = VK_NULL_HANDLE;
    }
    if (m_PostFXPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_PostFXPipelineLayout, nullptr);
        m_PostFXPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_PostFXDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_PostFXDescriptorLayout, nullptr);
        m_PostFXDescriptorLayout = VK_NULL_HANDLE;
    }
    if (m_PostFXVertShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_PostFXVertShader, nullptr);
        m_PostFXVertShader = VK_NULL_HANDLE;
    }
    if (m_PostFXFragShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_PostFXFragShader, nullptr);
        m_PostFXFragShader = VK_NULL_HANDLE;
    }
}

void Renderer::RecreateSwapchain() {
    m_Context->WaitIdle();
    
    uint32_t width = m_PendingWidth > 0 ? m_PendingWidth : m_Swapchain.GetExtent().width;
    uint32_t height = m_PendingHeight > 0 ? m_PendingHeight : m_Swapchain.GetExtent().height;
    
    if (width == 0 || height == 0) return;
    
    // Destroy old per-image semaphores
    for (auto semaphore : m_ImageRenderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_Context->GetDevice(), semaphore, nullptr);
        }
    }
    m_ImageRenderFinishedSemaphores.clear();
    
    // Recreate swapchain
    if (!m_Swapchain.Recreate(width, height)) {
        LUCENT_CORE_ERROR("Failed to recreate swapchain");
        return;
    }
    
    // Recreate per-image semaphores
    m_ImageRenderFinishedSemaphores.resize(m_Swapchain.GetImageCount());
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < m_ImageRenderFinishedSemaphores.size(); i++) {
        vkCreateSemaphore(m_Context->GetDevice(), &semaphoreInfo, nullptr, &m_ImageRenderFinishedSemaphores[i]);
    }
    
    // Recreate offscreen resources
    DestroyOffscreenResources();
    CreateOffscreenResources();
    CreateSampler();
    
    // Recreate framebuffers if using legacy path
    if (!UseDynamicRendering()) {
        DestroyFramebuffers();
        CreateFramebuffers();
    }
    
    // Update descriptor set
    DescriptorWriter writer;
    writer.WriteImage(0, m_OffscreenColor.GetView(), m_OffscreenSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writer.UpdateSet(m_Context->GetDevice(), m_CompositeDescriptorSet);
    
    m_PendingWidth = 0;
    m_PendingHeight = 0;
    
    LUCENT_CORE_INFO("Swapchain recreated: {}x{}", width, height);
}

bool Renderer::UseDynamicRendering() const {
    return m_Context->GetDeviceFeatures().dynamicRendering;
}

void Renderer::SetRenderMode(RenderMode mode) {
    if (!m_Capabilities.IsModeAvailable(mode)) {
        LUCENT_CORE_WARN("Render mode {} not available, keeping current mode {}", 
            RenderModeName(mode), RenderModeName(m_RenderMode));
        return;
    }
    
    if (m_RenderMode != mode) {
        LUCENT_CORE_INFO("Render mode changed: {} -> {}", 
            RenderModeName(m_RenderMode), RenderModeName(mode));
        m_RenderMode = mode;
        // TODO: Reset accumulation buffers when switching to/from traced modes
    }
}

bool Renderer::CreateRenderPasses() {
    VkDevice device = m_Context->GetDevice();
    
    // Offscreen render pass (HDR color + depth)
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        
        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        
        std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_OffscreenRenderPass) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create offscreen render pass");
            return false;
        }
    }
    
    // Swapchain render pass (color only, for ImGui)
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_Swapchain.GetFormat();
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        
        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_SwapchainRenderPass) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create swapchain render pass");
            return false;
        }
    }
    
    LUCENT_CORE_INFO("Legacy render passes created");
    return true;
}

bool Renderer::CreateFramebuffers() {
    VkDevice device = m_Context->GetDevice();
    VkExtent2D extent = m_Swapchain.GetExtent();
    
    // Offscreen framebuffer
    {
        std::array<VkImageView, 2> attachments = {
            m_OffscreenColor.GetView(),
            m_OffscreenDepth.GetView()
        };
        
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_OffscreenRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;
        
        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_OffscreenFramebuffer) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create offscreen framebuffer");
            return false;
        }
    }
    
    // Swapchain framebuffers
    {
        m_SwapchainFramebuffers.resize(m_Swapchain.GetImageCount());
        
        for (size_t i = 0; i < m_Swapchain.GetImageCount(); i++) {
            VkImageView attachment = m_Swapchain.GetImageView(static_cast<uint32_t>(i));
            
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_SwapchainRenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &attachment;
            framebufferInfo.width = extent.width;
            framebufferInfo.height = extent.height;
            framebufferInfo.layers = 1;
            
            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &m_SwapchainFramebuffers[i]) != VK_SUCCESS) {
                LUCENT_CORE_ERROR("Failed to create swapchain framebuffer {}", i);
                return false;
            }
        }
    }
    
    LUCENT_CORE_INFO("Legacy framebuffers created");
    return true;
}

void Renderer::DestroyRenderPasses() {
    VkDevice device = m_Context->GetDevice();
    
    if (m_OffscreenRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_OffscreenRenderPass, nullptr);
        m_OffscreenRenderPass = VK_NULL_HANDLE;
    }
    if (m_SwapchainRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_SwapchainRenderPass, nullptr);
        m_SwapchainRenderPass = VK_NULL_HANDLE;
    }
}

void Renderer::DestroyFramebuffers() {
    VkDevice device = m_Context->GetDevice();
    
    if (m_OffscreenFramebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_OffscreenFramebuffer, nullptr);
        m_OffscreenFramebuffer = VK_NULL_HANDLE;
    }
    
    for (auto framebuffer : m_SwapchainFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    m_SwapchainFramebuffers.clear();
}

void Renderer::BeginOffscreenPass(VkCommandBuffer cmd, const glm::vec4& clearColor) {
    VkExtent2D extent = m_Swapchain.GetExtent();
    
    if (UseDynamicRendering()) {
        // Vulkan 1.3 path
        m_OffscreenColor.TransitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_OffscreenColor.GetView();
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = { { clearColor.r, clearColor.g, clearColor.b, clearColor.a } };
        
        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = m_OffscreenDepth.GetView();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };
        
        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = extent;
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;
        renderInfo.pDepthAttachment = &depthAttachment;
        
        vkCmdBeginRendering(cmd, &renderInfo);
    } else {
        // Vulkan 1.1/1.2 fallback
        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = { { clearColor.r, clearColor.g, clearColor.b, clearColor.a } };
        clearValues[1].depthStencil = { 1.0f, 0 };
        
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_OffscreenRenderPass;
        renderPassInfo.framebuffer = m_OffscreenFramebuffer;
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = extent;
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();
        
        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    
    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::EndOffscreenPass(VkCommandBuffer cmd) {
    if (UseDynamicRendering()) {
        vkCmdEndRendering(cmd);
        m_OffscreenColor.TransitionLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        vkCmdEndRenderPass(cmd);
        // Render pass handles the transition to SHADER_READ_ONLY_OPTIMAL
    }
}

void Renderer::TransitionSwapchainToRenderTarget(VkCommandBuffer cmd) {
    if (UseDynamicRendering()) {
        // Vulkan 1.3 path
        VkImage swapchainImage = m_Swapchain.GetImage(m_CurrentImageIndex);
        
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchainImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        
        VkDependencyInfo depInfo{};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &barrier;
        
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
    // Legacy path: render pass handles the transition
}

void Renderer::BeginSwapchainPass(VkCommandBuffer cmd, const glm::vec4& clearColor) {
    VkExtent2D extent = m_Swapchain.GetExtent();
    
    if (UseDynamicRendering()) {
        // Vulkan 1.3 path
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = m_Swapchain.GetImageView(m_CurrentImageIndex);
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = { { clearColor.r, clearColor.g, clearColor.b, clearColor.a } };
        
        VkRenderingInfo renderInfo{};
        renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        renderInfo.renderArea.offset = { 0, 0 };
        renderInfo.renderArea.extent = extent;
        renderInfo.layerCount = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments = &colorAttachment;
        
        vkCmdBeginRendering(cmd, &renderInfo);
    } else {
        // Vulkan 1.1/1.2 fallback
        VkClearValue clearValue{};
        clearValue.color = { { clearColor.r, clearColor.g, clearColor.b, clearColor.a } };
        
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_SwapchainRenderPass;
        renderPassInfo.framebuffer = m_SwapchainFramebuffers[m_CurrentImageIndex];
        renderPassInfo.renderArea.offset = { 0, 0 };
        renderPassInfo.renderArea.extent = extent;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;
        
        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    
    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::EndSwapchainPass(VkCommandBuffer cmd) {
    if (UseDynamicRendering()) {
        vkCmdEndRendering(cmd);
    } else {
        vkCmdEndRenderPass(cmd);
    }
}

void Renderer::TransitionSwapchainToPresent(VkCommandBuffer cmd) {
    if (UseDynamicRendering()) {
        // Vulkan 1.3 path
        VkImage swapchainImage = m_Swapchain.GetImage(m_CurrentImageIndex);
        
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchainImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        
        VkDependencyInfo depInfo{};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers = &barrier;
        
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
    // Legacy path: render pass handles the transition to PRESENT_SRC_KHR
}

bool Renderer::CreateShadowResources() {
    VkDevice device = m_Context->GetDevice();
    
    // Create shadow map depth image
    ImageDesc shadowDesc{};
    shadowDesc.width = SHADOW_MAP_SIZE;
    shadowDesc.height = SHADOW_MAP_SIZE;
    shadowDesc.format = VK_FORMAT_D32_SFLOAT;
    shadowDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    shadowDesc.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    shadowDesc.debugName = "ShadowMap";
    
    if (!m_ShadowMap.Init(m_Device, shadowDesc)) {
        LUCENT_CORE_ERROR("Failed to create shadow map image");
        return false;
    }
    
    // Transition to depth attachment
    m_Device->ImmediateSubmit([this](VkCommandBuffer cmd) {
        m_ShadowMap.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    });
    
    // Create shadow sampler with comparison and border
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_FALSE; // Using regular sampling for PCF
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    
    if (vkCreateSampler(device, &samplerInfo, nullptr, &m_ShadowSampler) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create shadow sampler");
        return false;
    }
    
    // Create shadow render pass (depth-only)
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;
    
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &depthAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    
    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &m_ShadowRenderPass) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create shadow render pass");
        return false;
    }
    
    // Create shadow framebuffer
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_ShadowRenderPass;
    fbInfo.attachmentCount = 1;
    VkImageView shadowView = m_ShadowMap.GetView();
    fbInfo.pAttachments = &shadowView;
    fbInfo.width = SHADOW_MAP_SIZE;
    fbInfo.height = SHADOW_MAP_SIZE;
    fbInfo.layers = 1;
    
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &m_ShadowFramebuffer) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create shadow framebuffer");
        return false;
    }
    
    // Use the mesh descriptor layout (already has shadow map binding at set 0, binding 0)
    // Allocate descriptor set for the shadow map
    m_ShadowDescriptorSet = m_DescriptorAllocator.Allocate(m_MeshDescriptorLayout);
    if (m_ShadowDescriptorSet == VK_NULL_HANDLE) {
        LUCENT_CORE_ERROR("Failed to allocate shadow descriptor set");
        return false;
    }
    
    // Update descriptor set
    DescriptorWriter writer;
    writer.WriteImage(0, m_ShadowMap.GetView(), m_ShadowSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writer.UpdateSet(device, m_ShadowDescriptorSet);
    
    // Load shadow shader
    m_ShadowVertShader = PipelineBuilder::LoadShaderModule(device, "shaders/shadow_depth.vert.spv");
    if (m_ShadowVertShader == VK_NULL_HANDLE) {
        LUCENT_CORE_ERROR("Failed to load shadow vertex shader");
        return false;
    }
    
    // Create shadow pipeline layout (just push constants)
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(glm::mat4) * 2; // model + lightViewProj
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;
    
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_ShadowPipelineLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create shadow pipeline layout");
        return false;
    }
    
    // Create shadow pipeline
    PipelineBuilder builder;
    builder.AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_ShadowVertShader);
    // No fragment shader needed for depth-only pass
    
    // Vertex input for mesh
    std::vector<VkVertexInputBindingDescription> bindings = {
        {0, sizeof(float) * 12, VK_VERTEX_INPUT_RATE_VERTEX} // pos + normal + uv + tangent
    };
    std::vector<VkVertexInputAttributeDescription> attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, // position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3}, // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6}, // uv
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8} // tangent
    };
    builder.SetVertexInput(bindings, attributes);
    builder.SetInputAssembly(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    builder.SetRasterizer(VK_POLYGON_MODE_FILL, VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    builder.SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL);
    builder.SetMultisample(VK_SAMPLE_COUNT_1_BIT);
    builder.SetLayout(m_ShadowPipelineLayout);
    builder.SetRenderPass(m_ShadowRenderPass);
    builder.SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT);
    
    m_ShadowPipeline = builder.Build(device);
    if (m_ShadowPipeline == VK_NULL_HANDLE) {
        LUCENT_CORE_ERROR("Failed to create shadow pipeline");
        return false;
    }
    
    LUCENT_CORE_INFO("Shadow resources created ({}x{})", SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    return true;
}

void Renderer::DestroyShadowResources() {
    VkDevice device = m_Context ? m_Context->GetDevice() : VK_NULL_HANDLE;
    if (device == VK_NULL_HANDLE) return;
    
    if (m_ShadowPipeline) {
        vkDestroyPipeline(device, m_ShadowPipeline, nullptr);
        m_ShadowPipeline = VK_NULL_HANDLE;
    }
    if (m_ShadowPipelineLayout) {
        vkDestroyPipelineLayout(device, m_ShadowPipelineLayout, nullptr);
        m_ShadowPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_ShadowVertShader) {
        vkDestroyShaderModule(device, m_ShadowVertShader, nullptr);
        m_ShadowVertShader = VK_NULL_HANDLE;
    }
    // Descriptor layout is shared with mesh pipeline, don't destroy here
    m_ShadowDescriptorLayout = VK_NULL_HANDLE;
    if (m_ShadowFramebuffer) {
        vkDestroyFramebuffer(device, m_ShadowFramebuffer, nullptr);
        m_ShadowFramebuffer = VK_NULL_HANDLE;
    }
    if (m_ShadowRenderPass) {
        vkDestroyRenderPass(device, m_ShadowRenderPass, nullptr);
        m_ShadowRenderPass = VK_NULL_HANDLE;
    }
    if (m_ShadowSampler) {
        vkDestroySampler(device, m_ShadowSampler, nullptr);
        m_ShadowSampler = VK_NULL_HANDLE;
    }
    m_ShadowMap.Shutdown();
}

void Renderer::BeginShadowPass(VkCommandBuffer cmd) {
    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_ShadowRenderPass;
    beginInfo.framebuffer = m_ShadowFramebuffer;
    beginInfo.renderArea.offset = {0, 0};
    beginInfo.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    beginInfo.clearValueCount = 1;
    beginInfo.pClearValues = &clearValue;
    
    vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.height = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::EndShadowPass(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

} // namespace lucent::gfx

