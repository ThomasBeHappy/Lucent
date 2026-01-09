#pragma once

#include "lucent/core/Core.h"
#include "lucent/scene/Scene.h"
#include "lucent/scene/EditorCamera.h"
#include "lucent/mesh/EditableMesh.h"
#include "lucent/mesh/MeshOps.h"
#include "MaterialGraphPanel.h"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <optional>

struct GLFWwindow;

namespace lucent {

namespace gfx {
    class VulkanContext;
    class Device;
    class Renderer;
}

// Gizmo operation type
enum class GizmoOperation {
    Translate,
    Rotate,
    Scale
};

// Gizmo mode (local vs world space)
enum class GizmoMode {
    Local,
    World
};

// Viewport render mode
enum class RenderMode {
    Shaded,       // Full PBR lighting
    Solid,        // Flat shading (no specular)
    Wireframe     // Wireframe overlay
};

// Editor interaction mode
enum class EditorMode {
    Object,       // Object selection and manipulation
    Edit          // Mesh editing (vertex/edge/face)
};

// Mesh selection mode (in Edit mode)
enum class MeshSelectMode {
    Vertex,       // Select vertices (key: 1)
    Edge,         // Select edges (key: 2)
    Face          // Select faces (key: 3)
};

// Interactive transform type (Blender-style G/R/S)
enum class InteractiveTransformType {
    None,
    Grab,         // G key - translate
    Rotate,       // R key - rotate (future)
    Scale         // S key - scale (future)
};

// Axis constraint for interactive transforms
enum class AxisConstraint {
    None,         // Free transform
    X,            // Lock to X axis
    Y,            // Lock to Y axis
    Z             // Lock to Z axis
};

class EditorUI : public NonCopyable {
public:
    EditorUI() = default;
    ~EditorUI();
    
    bool Init(GLFWwindow* window, gfx::VulkanContext* context, gfx::Device* device, gfx::Renderer* renderer);
    void Shutdown();
    
    // Frame operations
    void BeginFrame();
    void EndFrame();
    void Render(VkCommandBuffer cmd);
    void RenderPlatformWindows();
    
    // Scene and selection binding
    void SetScene(scene::Scene* scene) { m_Scene = scene; }
    void SetEditorCamera(scene::EditorCamera* camera) { m_EditorCamera = camera; }
    
    // Selection (multi-select)
    void SetSelectedEntity(scene::Entity entity);
    scene::Entity GetSelectedEntity() const;
    bool IsSelected(scene::Entity entity) const;
    void AddToSelection(scene::Entity entity);
    void RemoveFromSelection(scene::Entity entity);
    void ToggleSelection(scene::Entity entity);
    void ClearSelection();
    void SelectAll();
    const std::vector<scene::EntityID>& GetSelectedEntities() const { return m_SelectedEntities; }
    size_t GetSelectionCount() const { return m_SelectedEntities.size(); }
    
    // Viewport texture for ImGui rendering
    void SetViewportTexture(VkImageView view, VkSampler sampler);
    
    // Render preview window
    void SetRenderPreviewTexture(VkImageView view, VkSampler sampler);
    void DrawRenderPreviewWindow(bool* pOpen);
    
    // Layout
    void SaveLayout();
    void LoadLayout();
    
    // Accessors
    bool IsViewportHovered() const { return m_ViewportHovered; }
    bool IsViewportFocused() const { return m_ViewportFocused; }
    ImVec2 GetViewportSize() const { return m_ViewportSize; }
    ImVec2 GetViewportPosition() const { return m_ViewportPosition; }
    
    // Gizmo state
    GizmoOperation GetGizmoOperation() const { return m_GizmoOperation; }
    void SetGizmoOperation(GizmoOperation op) { m_GizmoOperation = op; }
    GizmoMode GetGizmoMode() const { return m_GizmoMode; }
    void SetGizmoMode(GizmoMode mode) { m_GizmoMode = mode; }
    bool IsUsingGizmo() const { return m_UsingGizmo; }
    
    // Scene dirty flag (for tracer BVH update)
    bool ConsumeSceneDirty() { 
        bool dirty = m_SceneDirty; 
        m_SceneDirty = false; 
        return dirty; 
    }
    
    // Render mode
    RenderMode GetRenderMode() const { return m_RenderMode; }
    void SetRenderMode(RenderMode mode) { m_RenderMode = mode; }
    
    // PostFX settings
    float GetExposure() const { return m_Exposure; }
    int GetTonemapMode() const { return m_TonemapMode; }
    float GetGamma() const { return m_Gamma; }
    
    // Snapping
    float GetTranslateSnap() const { return m_TranslateSnap; }
    float GetRotateSnap() const { return m_RotateSnap; }
    float GetScaleSnap() const { return m_ScaleSnap; }
    
    // Material graph panel
    MaterialGraphPanel& GetMaterialGraphPanel() { return m_MaterialGraphPanel; }
    void ShowMaterialGraphPanel() { m_MaterialGraphPanel.SetVisible(true); }
    
    // Render preview window
    void ShowRenderPreview(bool show) { m_ShowRenderPreview = show; if (show) m_RenderPreviewJustOpened = true; }
    bool IsRenderPreviewVisible() const { return m_ShowRenderPreview; }
    
    // Edit Mode
    EditorMode GetEditorMode() const { return m_EditorMode; }
    void SetEditorMode(EditorMode mode);
    void ToggleEditorMode(); // Switch between Object and Edit mode (Tab key)
    
    MeshSelectMode GetMeshSelectMode() const { return m_MeshSelectMode; }
    void SetMeshSelectMode(MeshSelectMode mode);
    
    // Returns true if an entity is being edited
    bool IsInEditMode() const { return m_EditorMode == EditorMode::Edit; }
    scene::Entity GetEditedEntity() const;
    
private:
    void SetupStyle();
    void SetupFonts();
    void DrawDockspace();
    void DrawViewportPanel();
    void DrawGizmo();
    void DrawOutlinerPanel();
    void DrawInspectorPanel();
    void DrawContentBrowserPanel();
    void DrawConsolePanel();
    void DrawRenderPropertiesPanel();
    void ApplySceneEnvironment();
    
    void DrawEntityNode(scene::Entity entity);
    void DrawComponentsPanel(scene::Entity entity);
    void DrawModals();
    void HandleGlobalShortcuts();
    
    // Picking
    void HandleViewportClick();
    scene::Entity PickEntity(const glm::vec2& mousePos);
    bool RayIntersectsAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir, 
                           const glm::vec3& aabbMin, const glm::vec3& aabbMax, float& tOut);
    bool RayIntersectsSphere(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                             const glm::vec3& center, float radius, float& tOut);
    
    // Edit Mode picking and overlay
    void HandleEditModeClick();
    mesh::VertexID PickVertex(const glm::vec2& mousePos, float radius = 10.0f);
    mesh::EdgeID PickEdge(const glm::vec2& mousePos, float radius = 5.0f);
    mesh::FaceID PickFace(const glm::vec2& mousePos);
    void DrawEditModeOverlay();
    glm::vec3 WorldToScreen(const glm::vec3& worldPos);

    // Scene indicators (lights/cameras)
    void DrawEntityIndicators();
    
    // Interactive Transform (Blender-style G/R/S)
    void StartInteractiveTransform(InteractiveTransformType type);
    void UpdateInteractiveTransform();
    void ConfirmInteractiveTransform();
    void CancelInteractiveTransform();
    bool IsInInteractiveTransform() const { return m_InteractiveTransform != InteractiveTransformType::None; }
    void DrawInteractiveTransformHUD();
    
    // Asset navigation
    void NavigateToAsset(const std::string& path);
    void OpenMaterialInEditor(const std::string& path);
    
    // Material assignment
    void HandleMaterialDrop(const std::string& materialPath);
    
private:
    GLFWwindow* m_Window = nullptr;
    gfx::VulkanContext* m_Context = nullptr;
    gfx::Device* m_Device = nullptr;
    gfx::Renderer* m_Renderer = nullptr;
    
    // Scene reference
    scene::Scene* m_Scene = nullptr;
    scene::EditorCamera* m_EditorCamera = nullptr;
    
    // Multi-selection
    std::vector<scene::EntityID> m_SelectedEntities;
    
    // Clipboard for entity copy/paste
    struct ClipboardEntity {
        std::string name;
        scene::TransformComponent transform;
        std::optional<scene::CameraComponent> camera;
        std::optional<scene::LightComponent> light;
        std::optional<scene::MeshRendererComponent> meshRenderer;
    };
    std::vector<ClipboardEntity> m_Clipboard;
    
    VkDescriptorPool m_ImGuiPool = VK_NULL_HANDLE;
    
    // Viewport texture
    VkDescriptorSet m_ViewportDescriptor = VK_NULL_HANDLE;
    VkDescriptorSet m_RenderPreviewDescriptor = VK_NULL_HANDLE;

    // Indicators
    bool m_ShowIndicators = true;
    bool m_IndicatorsSelectedOnly = false; // lights
    bool m_ShowLightIndicators = true;
    bool m_ShowCameraIndicators = true;
    bool m_CameraIndicatorsSelectedOnly = true;
    ImVec2 m_ViewportSize = { 0, 0 };
    ImVec2 m_ViewportPosition = { 0, 0 };
    bool m_ViewportHovered = false;
    bool m_ViewportFocused = false;
    
    // Gizmo
    GizmoOperation m_GizmoOperation = GizmoOperation::Translate;
    GizmoMode m_GizmoMode = GizmoMode::Local;
    bool m_UsingGizmo = false;
    bool m_WasUsingGizmo = false; // Track gizmo state changes
    
    // Gizmo undo state
    glm::vec3 m_GizmoStartPosition{0.0f};
    glm::vec3 m_GizmoStartRotation{0.0f};
    glm::vec3 m_GizmoStartScale{1.0f};
    
    // Snapping
    bool m_SnapEnabled = false;
    float m_TranslateSnap = 0.5f;
    float m_RotateSnap = 15.0f;
    float m_ScaleSnap = 0.1f;
    
    // Render mode
    RenderMode m_RenderMode = RenderMode::Shaded;
    
    // Edit Mode
    EditorMode m_EditorMode = EditorMode::Object;
    MeshSelectMode m_MeshSelectMode = MeshSelectMode::Vertex;
    scene::EntityID m_EditedEntityID = UINT32_MAX;  // Entity being edited in Edit Mode
    
    // Interactive Transform (Blender-style G/R/S)
    InteractiveTransformType m_InteractiveTransform = InteractiveTransformType::None;
    AxisConstraint m_AxisConstraint = AxisConstraint::None;
    glm::vec2 m_TransformStartMousePos{0.0f};
    glm::vec3 m_TransformStartValue{0.0f};  // Starting position/rotation/scale
    glm::vec3 m_TransformStartRotation{0.0f};
    glm::vec3 m_TransformStartScale{1.0f};
    std::vector<glm::vec3> m_TransformStartPositions;  // For Edit mode: original vertex positions
    std::vector<mesh::VertexID> m_TransformVertexIDs;  // Vertex IDs being transformed
    glm::vec3 m_TransformPivotLocal{0.0f};
    float m_TransformSensitivity = 0.01f;
    
    // Numeric input during interactive transforms (e.g. G X 1 Enter)
    std::string m_TransformNumeric;
    
    // PostFX settings
    float m_Exposure = 1.0f;
    int m_TonemapMode = 2; // ACES by default
    float m_Gamma = 2.2f;
    
    // Layout file path
    std::string m_LayoutPath = "imgui.ini";
    
    // Panel visibility
    bool m_ShowViewport = true;
    bool m_ShowOutliner = true;
    bool m_ShowInspector = true;
    bool m_ShowContentBrowser = true;
    bool m_ShowConsole = true;
    bool m_ShowRenderProperties = false;
    bool m_ShowRenderPreview = false;
    bool m_RenderPreviewJustOpened = false;
    
    bool m_FirstFrame = true;
    
    // Scene file management
    std::string m_CurrentScenePath;
    bool m_SceneDirty = false;
    
    // Modals
    bool m_ShowAboutModal = false;
    bool m_ShowShortcutsModal = false;
    bool m_ShowPreferencesModal = false;
    
    // Content browser state
    std::filesystem::path m_ContentBrowserPath;
    std::string m_ContentBrowserSearch;
    bool m_IconFontLoaded = false;
    
    // Material graph panel
    MaterialGraphPanel m_MaterialGraphPanel;
};

} // namespace lucent
