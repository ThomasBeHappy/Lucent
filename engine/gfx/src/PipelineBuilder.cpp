#include "lucent/gfx/PipelineBuilder.h"
#include <fstream>

namespace lucent::gfx {

void PipelineBuilder::SetDefaults() {
    if (m_DefaultsSet) return;
    
    // Vertex input (empty by default - no vertex buffer)
    m_VertexInput = {};
    m_VertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    
    // Input assembly
    m_InputAssembly = {};
    m_InputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    m_InputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    m_InputAssembly.primitiveRestartEnable = VK_FALSE;
    
    // Viewport and scissor (will be dynamic)
    m_Viewport = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    m_Scissor = { {0, 0}, {1, 1} };
    
    // Rasterizer
    m_Rasterizer = {};
    m_Rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    m_Rasterizer.depthClampEnable = VK_FALSE;
    m_Rasterizer.rasterizerDiscardEnable = VK_FALSE;
    m_Rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    m_Rasterizer.lineWidth = 1.0f;
    m_Rasterizer.cullMode = VK_CULL_MODE_NONE;
    m_Rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    m_Rasterizer.depthBiasEnable = VK_FALSE;
    
    // Multisampling
    m_Multisample = {};
    m_Multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    m_Multisample.sampleShadingEnable = VK_FALSE;
    m_Multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Depth stencil
    m_DepthStencil = {};
    m_DepthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    m_DepthStencil.depthTestEnable = VK_FALSE;
    m_DepthStencil.depthWriteEnable = VK_FALSE;
    m_DepthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    m_DepthStencil.depthBoundsTestEnable = VK_FALSE;
    m_DepthStencil.stencilTestEnable = VK_FALSE;
    
    // Color blend attachment
    m_ColorBlendAttachment = {};
    m_ColorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    m_ColorBlendAttachment.blendEnable = VK_FALSE;
    
    m_DefaultsSet = true;
}

PipelineBuilder& PipelineBuilder::AddShaderStage(VkShaderStageFlagBits stage, VkShaderModule module, const char* entry) {
    SetDefaults();
    
    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = stage;
    shaderStage.module = module;
    shaderStage.pName = entry;
    
    m_ShaderStages.push_back(shaderStage);
    return *this;
}

PipelineBuilder& PipelineBuilder::SetVertexInput(const VkPipelineVertexInputStateCreateInfo& info) {
    SetDefaults();
    m_VertexInput = info;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetVertexInput(
    const std::vector<VkVertexInputBindingDescription>& bindings,
    const std::vector<VkVertexInputAttributeDescription>& attributes) {
    SetDefaults();
    
    // Store copies
    m_VertexBindings = bindings;
    m_VertexAttributes = attributes;
    
    m_VertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(m_VertexBindings.size());
    m_VertexInput.pVertexBindingDescriptions = m_VertexBindings.data();
    m_VertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_VertexAttributes.size());
    m_VertexInput.pVertexAttributeDescriptions = m_VertexAttributes.data();
    
    return *this;
}

PipelineBuilder& PipelineBuilder::SetInputAssembly(VkPrimitiveTopology topology, bool primitiveRestart) {
    SetDefaults();
    m_InputAssembly.topology = topology;
    m_InputAssembly.primitiveRestartEnable = primitiveRestart ? VK_TRUE : VK_FALSE;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetViewport(float width, float height) {
    SetDefaults();
    m_Viewport.width = width;
    m_Viewport.height = height;
    m_Scissor.extent.width = static_cast<uint32_t>(width);
    m_Scissor.extent.height = static_cast<uint32_t>(height);
    return *this;
}

PipelineBuilder& PipelineBuilder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    SetDefaults();
    m_Scissor.offset = { x, y };
    m_Scissor.extent = { w, h };
    return *this;
}

PipelineBuilder& PipelineBuilder::SetRasterizer(VkPolygonMode polyMode, VkCullModeFlags cullMode, VkFrontFace frontFace) {
    SetDefaults();
    m_Rasterizer.polygonMode = polyMode;
    m_Rasterizer.cullMode = cullMode;
    m_Rasterizer.frontFace = frontFace;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetMultisample(VkSampleCountFlagBits samples) {
    SetDefaults();
    m_Multisample.rasterizationSamples = samples;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetDepthStencil(bool depthTest, bool depthWrite, VkCompareOp compareOp) {
    SetDefaults();
    m_DepthStencil.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    m_DepthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    m_DepthStencil.depthCompareOp = compareOp;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetColorBlendAttachment(bool enable, VkBlendFactor srcColor, VkBlendFactor dstColor) {
    SetDefaults();
    m_ColorBlendAttachment.blendEnable = enable ? VK_TRUE : VK_FALSE;
    m_ColorBlendAttachment.srcColorBlendFactor = srcColor;
    m_ColorBlendAttachment.dstColorBlendFactor = dstColor;
    m_ColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    m_ColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    m_ColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    m_ColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetColorAttachmentFormat(VkFormat format) {
    SetDefaults();
    m_ColorFormat = format;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetDepthAttachmentFormat(VkFormat format) {
    SetDefaults();
    m_DepthFormat = format;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetRenderPass(VkRenderPass renderPass, uint32_t subpass) {
    m_RenderPass = renderPass;
    m_Subpass = subpass;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetLayout(VkPipelineLayout layout) {
    m_Layout = layout;
    return *this;
}

VkPipeline PipelineBuilder::Build(VkDevice device) {
    SetDefaults();
    
    // Viewport state (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &m_Viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &m_Scissor;
    
    // Color blending
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &m_ColorBlendAttachment;
    
    // Dynamic state
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    
    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(m_ShaderStages.size());
    pipelineInfo.pStages = m_ShaderStages.data();
    pipelineInfo.pVertexInputState = &m_VertexInput;
    pipelineInfo.pInputAssemblyState = &m_InputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &m_Rasterizer;
    pipelineInfo.pMultisampleState = &m_Multisample;
    pipelineInfo.pDepthStencilState = &m_DepthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_Layout;
    
    // Dynamic rendering (Vulkan 1.3) vs legacy render pass
    VkPipelineRenderingCreateInfo renderingInfo{};
    if (m_RenderPass != VK_NULL_HANDLE) {
        // Legacy path - use render pass
        pipelineInfo.renderPass = m_RenderPass;
        pipelineInfo.subpass = m_Subpass;
    } else {
        // Vulkan 1.3 path - use dynamic rendering
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &m_ColorFormat;
        renderingInfo.depthAttachmentFormat = m_DepthFormat;
        
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.subpass = 0;
    }
    
    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    
    if (result != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create graphics pipeline: {}", static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    
    return pipeline;
}

void PipelineBuilder::Clear() {
    m_ShaderStages.clear();
    m_DefaultsSet = false;
    m_Layout = VK_NULL_HANDLE;
    m_RenderPass = VK_NULL_HANDLE;
    m_Subpass = 0;
}

VkShaderModule PipelineBuilder::LoadShaderModule(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    
    if (!file.is_open()) {
        LUCENT_CORE_ERROR("Failed to open shader file: {}", path);
        return VK_NULL_HANDLE;
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();
    
    return CreateShaderModule(device, buffer);
}

VkShaderModule PipelineBuilder::CreateShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();
    
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create shader module");
        return VK_NULL_HANDLE;
    }
    
    return shaderModule;
}

} // namespace lucent::gfx

