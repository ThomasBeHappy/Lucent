#pragma once

#include "lucent/gfx/Device.h"
#include <vector>
#include <string>

namespace lucent::gfx {

struct ShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    const char* entryPoint = "main";
};

class PipelineBuilder : public NonCopyable {
public:
    PipelineBuilder() = default;
    
    // Shader stages
    PipelineBuilder& AddShaderStage(VkShaderStageFlagBits stage, VkShaderModule module, const char* entry = "main");
    
    // Vertex input
    PipelineBuilder& SetVertexInput(const VkPipelineVertexInputStateCreateInfo& info);
    PipelineBuilder& SetVertexInput(
        const std::vector<VkVertexInputBindingDescription>& bindings,
        const std::vector<VkVertexInputAttributeDescription>& attributes);
    PipelineBuilder& SetInputAssembly(VkPrimitiveTopology topology, bool primitiveRestart = false);
    
    // Viewport/scissor (dynamic by default)
    PipelineBuilder& SetViewport(float width, float height);
    PipelineBuilder& SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h);
    
    // Rasterization
    PipelineBuilder& SetRasterizer(VkPolygonMode polyMode, VkCullModeFlags cullMode, VkFrontFace frontFace);
    PipelineBuilder& SetMultisample(VkSampleCountFlagBits samples);
    PipelineBuilder& SetDepthStencil(bool depthTest, bool depthWrite, VkCompareOp compareOp);
    
    // Color blending
    PipelineBuilder& SetColorBlendAttachment(bool enable, VkBlendFactor srcColor = VK_BLEND_FACTOR_SRC_ALPHA,
                                             VkBlendFactor dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    
    // Dynamic rendering (Vulkan 1.3) - used when render pass is not set
    PipelineBuilder& SetColorAttachmentFormat(VkFormat format);
    PipelineBuilder& SetDepthAttachmentFormat(VkFormat format);
    
    // Legacy render pass (Vulkan 1.1/1.2) - used instead of dynamic rendering
    PipelineBuilder& SetRenderPass(VkRenderPass renderPass, uint32_t subpass = 0);
    
    // Pipeline layout
    PipelineBuilder& SetLayout(VkPipelineLayout layout);
    
    // Build - uses dynamic rendering if renderPass is not set, legacy otherwise
    VkPipeline Build(VkDevice device);
    void Clear();
    
    // Utility: load shader module from SPIR-V
    static VkShaderModule LoadShaderModule(VkDevice device, const std::string& path);
    static VkShaderModule CreateShaderModule(VkDevice device, const std::vector<uint32_t>& code);
    
private:
    void SetDefaults();
    
private:
    std::vector<VkPipelineShaderStageCreateInfo> m_ShaderStages;
    
    VkPipelineVertexInputStateCreateInfo m_VertexInput{};
    std::vector<VkVertexInputBindingDescription> m_VertexBindings;
    std::vector<VkVertexInputAttributeDescription> m_VertexAttributes;
    VkPipelineInputAssemblyStateCreateInfo m_InputAssembly{};
    VkViewport m_Viewport{};
    VkRect2D m_Scissor{};
    VkPipelineRasterizationStateCreateInfo m_Rasterizer{};
    VkPipelineMultisampleStateCreateInfo m_Multisample{};
    VkPipelineDepthStencilStateCreateInfo m_DepthStencil{};
    VkPipelineColorBlendAttachmentState m_ColorBlendAttachment{};
    
    VkPipelineLayout m_Layout = VK_NULL_HANDLE;
    
    VkFormat m_ColorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormat m_DepthFormat = VK_FORMAT_UNDEFINED;
    
    // Legacy render pass support
    VkRenderPass m_RenderPass = VK_NULL_HANDLE;
    uint32_t m_Subpass = 0;
    
    bool m_DefaultsSet = false;
};

} // namespace lucent::gfx

