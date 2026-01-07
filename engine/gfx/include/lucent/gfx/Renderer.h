#pragma once

#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Swapchain.h"
#include "lucent/gfx/Image.h"
#include "lucent/gfx/DescriptorAllocator.h"
#include "lucent/gfx/PipelineBuilder.h"
#include "lucent/gfx/RenderCapabilities.h"
#include "lucent/gfx/RenderSettings.h"
#include "lucent/gfx/TracerCompute.h"
#include "lucent/gfx/TracerRayKHR.h"
#include "lucent/gfx/FinalRender.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>

namespace lucent::gfx {

constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

struct FrameData {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;
};

struct RendererConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
};

class Renderer : public NonCopyable {
public:
    Renderer() = default;
    ~Renderer();
    
    bool Init(VulkanContext* context, Device* device, const RendererConfig& config);
    void Shutdown();
    
    // Frame operations
    bool BeginFrame();
    void EndFrame();
    
    // Resize handling
    void OnResize(uint32_t width, uint32_t height);
    
    // Getters
    VkCommandBuffer GetCurrentCommandBuffer() const { return m_Frames[m_CurrentFrame].commandBuffer; }
    uint32_t GetCurrentFrameIndex() const { return m_CurrentFrame; }
    uint32_t GetCurrentImageIndex() const { return m_CurrentImageIndex; }
    
    Swapchain* GetSwapchain() { return &m_Swapchain; }
    Image* GetOffscreenImage() { return &m_OffscreenColor; }
    Image* GetDepthImage() { return &m_OffscreenDepth; }
    Device* GetDevice() { return m_Device; }
    VulkanContext* GetContext() { return m_Context; }
    
    VkDescriptorSet GetCompositeDescriptorSet() const { return m_CompositeDescriptorSet; }
    VkPipeline GetCompositePipeline() const { return m_CompositePipeline; }
    VkPipelineLayout GetCompositePipelineLayout() const { return m_CompositePipelineLayout; }
    
    VkPipeline GetTrianglePipeline() const { return m_TrianglePipeline; }
    VkPipelineLayout GetTrianglePipelineLayout() const { return m_TrianglePipelineLayout; }
    VkSampler GetOffscreenSampler() const { return m_OffscreenSampler; }
    
    VkPipeline GetGridPipeline() const { return m_GridPipeline; }
    VkPipelineLayout GetGridPipelineLayout() const { return m_GridPipelineLayout; }
    
    VkPipeline GetMeshPipeline() const { return m_MeshPipeline; }
    VkPipeline GetMeshWireframePipeline() const { return m_MeshWireframePipeline; }
    VkPipelineLayout GetMeshPipelineLayout() const { return m_MeshPipelineLayout; }
    
    VkPipeline GetSkyboxPipeline() const { return m_SkyboxPipeline; }
    VkPipelineLayout GetSkyboxPipelineLayout() const { return m_SkyboxPipelineLayout; }
    
    // Render capabilities and mode
    const RenderCapabilities& GetCapabilities() const { return m_Capabilities; }
    RenderMode GetRenderMode() const { return m_RenderMode; }
    void SetRenderMode(RenderMode mode);
    bool IsRenderModeAvailable(RenderMode mode) const { return m_Capabilities.IsModeAvailable(mode); }
    
    // Render settings
    RenderSettings& GetSettings() { return m_Settings; }
    const RenderSettings& GetSettings() const { return m_Settings; }
    
    // Tracer access
    TracerCompute* GetTracerCompute() { return m_TracerCompute.get(); }
    TracerRayKHR* GetTracerRayKHR() { return m_TracerRayKHR.get(); }
    FinalRender* GetFinalRender() { return m_FinalRender.get(); }
    
    // Post-processing
    VkPipeline GetPostFXPipeline() const { return m_PostFXPipeline; }
    VkPipelineLayout GetPostFXPipelineLayout() const { return m_PostFXPipelineLayout; }
    VkDescriptorSet GetPostFXDescriptorSet() const { return m_PostFXDescriptorSet; }
    
    // Render pass abstraction - works with both Vulkan 1.3 and 1.1/1.2
    void BeginOffscreenPass(VkCommandBuffer cmd, const glm::vec4& clearColor = glm::vec4(0.02f, 0.02f, 0.03f, 1.0f));
    void EndOffscreenPass(VkCommandBuffer cmd);
    void BeginSwapchainPass(VkCommandBuffer cmd, const glm::vec4& clearColor = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f));
    void EndSwapchainPass(VkCommandBuffer cmd);
    
    // Image transition abstraction
    void TransitionSwapchainToRenderTarget(VkCommandBuffer cmd);
    void TransitionSwapchainToPresent(VkCommandBuffer cmd);
    
    // Check if we're using Vulkan 1.3 features
    bool UseDynamicRendering() const;
    
    // Legacy render passes (for Vulkan 1.1/1.2 fallback)
    VkRenderPass GetOffscreenRenderPass() const { return m_OffscreenRenderPass; }
    VkRenderPass GetSwapchainRenderPass() const { return m_SwapchainRenderPass; }
    
    // Shadow mapping
    void BeginShadowPass(VkCommandBuffer cmd);
    void EndShadowPass(VkCommandBuffer cmd);
    VkDescriptorSet GetShadowDescriptorSet() const { return m_ShadowDescriptorSet; }
    VkPipeline GetShadowPipeline() const { return m_ShadowPipeline; }
    VkPipelineLayout GetShadowPipelineLayout() const { return m_ShadowPipelineLayout; }
    Image* GetShadowMap() { return &m_ShadowMap; }
    VkSampler GetShadowSampler() const { return m_ShadowSampler; }
    
private:
    bool CreateFrameResources();
    bool CreateOffscreenResources();
    bool CreatePipelines();
    bool CreateSampler();
    bool CreateRenderPasses();
    bool CreateFramebuffers();
    bool CreateShadowResources();
    void DestroyFrameResources();
    void DestroyOffscreenResources();
    void DestroyPipelines();
    void DestroyRenderPasses();
    void DestroyFramebuffers();
    void DestroyShadowResources();
    
    void RecreateSwapchain();
    
private:
    VulkanContext* m_Context = nullptr;
    Device* m_Device = nullptr;
    
    Swapchain m_Swapchain;
    RendererConfig m_Config;
    
    // Per-frame resources
    FrameData m_Frames[MAX_FRAMES_IN_FLIGHT];
    uint32_t m_CurrentFrame = 0;
    uint32_t m_CurrentImageIndex = 0;
    
    // Offscreen rendering
    Image m_OffscreenColor;
    Image m_OffscreenDepth;
    VkSampler m_OffscreenSampler = VK_NULL_HANDLE;
    
    // Composite pipeline (offscreen -> swapchain)
    VkDescriptorSetLayout m_CompositeDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_CompositeDescriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_CompositePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_CompositePipeline = VK_NULL_HANDLE;
    
    // Triangle pipeline (for testing)
    VkPipelineLayout m_TrianglePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_TrianglePipeline = VK_NULL_HANDLE;
    
    // Grid pipeline
    VkPipelineLayout m_GridPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_GridPipeline = VK_NULL_HANDLE;
    VkShaderModule m_GridVertShader = VK_NULL_HANDLE;
    VkShaderModule m_GridFragShader = VK_NULL_HANDLE;
    
    // Mesh pipeline
    VkDescriptorSetLayout m_MeshDescriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_MeshPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_MeshPipeline = VK_NULL_HANDLE;
    VkPipeline m_MeshWireframePipeline = VK_NULL_HANDLE;
    VkShaderModule m_MeshVertShader = VK_NULL_HANDLE;
    VkShaderModule m_MeshFragShader = VK_NULL_HANDLE;
    
    // Skybox pipeline
    VkPipelineLayout m_SkyboxPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_SkyboxPipeline = VK_NULL_HANDLE;
    VkShaderModule m_SkyboxVertShader = VK_NULL_HANDLE;
    VkShaderModule m_SkyboxFragShader = VK_NULL_HANDLE;
    
    // PostFX pipeline
    VkDescriptorSetLayout m_PostFXDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_PostFXDescriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout m_PostFXPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_PostFXPipeline = VK_NULL_HANDLE;
    VkShaderModule m_PostFXVertShader = VK_NULL_HANDLE;
    VkShaderModule m_PostFXFragShader = VK_NULL_HANDLE;
    
    // Descriptor allocator
    DescriptorAllocator m_DescriptorAllocator;
    
    // Shader modules
    VkShaderModule m_TriangleVertShader = VK_NULL_HANDLE;
    VkShaderModule m_TriangleFragShader = VK_NULL_HANDLE;
    VkShaderModule m_CompositeVertShader = VK_NULL_HANDLE;
    VkShaderModule m_CompositeFragShader = VK_NULL_HANDLE;
    
    bool m_FrameStarted = false;
    bool m_NeedsResize = false;
    uint32_t m_PendingWidth = 0;
    uint32_t m_PendingHeight = 0;
    
    // Render capabilities and current mode
    RenderCapabilities m_Capabilities;
    RenderMode m_RenderMode = RenderMode::Simple;
    RenderSettings m_Settings;
    
    // Compute tracer (for Traced mode)
    std::unique_ptr<TracerCompute> m_TracerCompute;
    
    // KHR ray tracer (for RayTraced mode)
    std::unique_ptr<TracerRayKHR> m_TracerRayKHR;
    
    // Final render (for image export)
    std::unique_ptr<FinalRender> m_FinalRender;
    
    // Legacy render pass support (Vulkan 1.1/1.2 fallback)
    VkRenderPass m_OffscreenRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_SwapchainRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_OffscreenFramebuffer = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_SwapchainFramebuffers;
    
    // Per-swapchain-image semaphores (to avoid semaphore reuse before present completes)
    std::vector<VkSemaphore> m_ImageRenderFinishedSemaphores;
    
    // Shadow mapping
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;
    Image m_ShadowMap;
    VkSampler m_ShadowSampler = VK_NULL_HANDLE;
    VkRenderPass m_ShadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_ShadowFramebuffer = VK_NULL_HANDLE;
    VkPipeline m_ShadowPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_ShadowPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_ShadowVertShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ShadowDescriptorLayout = VK_NULL_HANDLE; // Points to m_MeshDescriptorLayout
    VkDescriptorSet m_ShadowDescriptorSet = VK_NULL_HANDLE;
};

} // namespace lucent::gfx

