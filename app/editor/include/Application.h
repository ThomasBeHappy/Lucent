#pragma once

#include "lucent/core/Core.h"
#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Renderer.h"
#include "lucent/gfx/EnvironmentMap.h"
#include "lucent/scene/Scene.h"
#include "lucent/scene/EditorCamera.h"
#include "lucent/assets/Mesh.h"
#include "EditorUI.h"
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace lucent {

struct ApplicationConfig {
    const char* title = "Lucent Editor";
    uint32_t width = 1280;
    uint32_t height = 720;
    bool vsync = true;
    bool enableValidation = true;
};

class Application : public NonCopyable {
public:
    Application() = default;
    ~Application();
    
    bool Init(const ApplicationConfig& config);
    void Run();
    void Shutdown();
    
    // Getters
    GLFWwindow* GetWindow() const { return m_Window; }
    gfx::VulkanContext* GetVulkanContext() { return &m_VulkanContext; }
    gfx::Device* GetDevice() { return &m_Device; }
    gfx::Renderer* GetRenderer() { return &m_Renderer; }
    scene::Scene* GetScene() { return &m_Scene; }
    scene::EditorCamera* GetEditorCamera() { return &m_EditorCamera; }
    
    bool IsRunning() const { return m_Running; }
    float GetDeltaTime() const { return m_DeltaTime; }
    
private:
    bool InitWindow(const ApplicationConfig& config);
    void InitScene();
    void ProcessInput();
    void RenderFrame();
    void RenderSceneToViewport(VkCommandBuffer cmd);
    
    static void FramebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    
private:
    GLFWwindow* m_Window = nullptr;
    ApplicationConfig m_Config;
    
    gfx::VulkanContext m_VulkanContext;
    gfx::Device m_Device;
    gfx::Renderer m_Renderer;
    EditorUI m_EditorUI;
    
    // Scene
    scene::Scene m_Scene;
    scene::EditorCamera m_EditorCamera;
    scene::Entity m_SelectedEntity;
    
    bool m_Running = false;
    bool m_Minimized = false;
    float m_DeltaTime = 0.0f;
    double m_LastFrameTime = 0.0;
    
    uint32_t m_FrameCount = 0;
    double m_FpsTimer = 0.0;
    
    bool m_ViewportTextureReady = false;
    
    // Input state
    double m_LastMouseX = 0.0;
    double m_LastMouseY = 0.0;
    bool m_FirstMouse = true;
    
    // Primitive meshes
    std::unordered_map<scene::MeshRendererComponent::PrimitiveType, std::unique_ptr<assets::Mesh>> m_PrimitiveMeshes;
    
    // Editable mesh GPU buffers (entity ID -> GPU mesh)
    std::unordered_map<scene::EntityID, std::unique_ptr<assets::Mesh>> m_EditableMeshGPU;
    void UpdateEditableMeshGPU(scene::Entity entity);
    
    // Shadow mapping
    bool m_ShadowsEnabled = true;
    float m_ShadowBias = 0.005f;
    glm::mat4 m_LightViewProj{1.0f};
    
    void CreatePrimitiveMeshes();
    void RenderMeshes(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void UpdateLightMatrix();
    void RenderShadowPass(VkCommandBuffer cmd);
    
    // Traced mode support
    void UpdateTracerScene();
    void UpdateTracerLightsOnly();
    void RenderTracedPath(VkCommandBuffer cmd);
    void RenderRayTracedPath(VkCommandBuffer cmd);
    void BuildTracerSceneData(std::vector<gfx::BVHBuilder::Triangle>& triangles,
                              std::vector<gfx::GPUMaterial>& materials,
                              std::vector<gfx::GPULight>& lights,
                              std::vector<gfx::GPUVolume>& volumes);
    void StartFinalRenderFromMainCamera();
    bool m_TracerSceneDirty = true;
    std::vector<gfx::GPULight> m_LastTracerLights;
    
    // Environment mapping (HDRI)
    gfx::EnvironmentMap m_EnvironmentMap;
    void InitEnvironmentMap();
};

} // namespace lucent
