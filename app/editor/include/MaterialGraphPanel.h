#pragma once

#include "lucent/material/MaterialGraph.h"
#include "lucent/material/MaterialAsset.h"
#include "lucent/gfx/Image.h"
#include "lucent/assets/Mesh.h"
#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace lucent {

namespace gfx {
    class Device;
}

// Node Editor panel for editing material graphs
class MaterialGraphPanel {
public:
    MaterialGraphPanel() = default;
    ~MaterialGraphPanel();
    
    bool Init(gfx::Device* device);
    void Shutdown();
    
    // Draw the panel
    void Draw();
    
    // Material management
    void SetMaterial(material::MaterialAsset* material);
    material::MaterialAsset* GetMaterial() const { return m_Material; }
    
    // Create new material
    material::MaterialAsset* CreateNewMaterial();
    
    // Panel visibility
    bool IsVisible() const { return m_Visible; }
    void SetVisible(bool visible) { m_Visible = visible; }
    void ToggleVisible() { m_Visible = !m_Visible; }
    
    // Check if panel needs attention (errors)
    bool HasErrors() const { return m_Material && !m_Material->IsValid(); }
    
    // Callback for navigating to assets (set by EditorUI)
    using NavigateToAssetCallback = std::function<void(const std::string& path)>;
    void SetNavigateToAssetCallback(NavigateToAssetCallback callback) { m_NavigateToAsset = callback; }
    
private:
    void DrawToolbar();
    void DrawNodeEditor();
    void DrawNodeCreationMenu();
    void DrawQuickAddPopup();
    void DrawCompileStatus();
    void HandleAutoCompile();
    void RenderMaterialPreviewIfNeeded();
    void RenderMaterialPreview();
    
    // Node drawing helpers
    void DrawNode(const material::MaterialNode& node);
    void DrawPin(const material::MaterialPin& pin, bool isInput);
    ImColor GetPinColor(material::PinType type);
    
    // Popup menus
    void HandleContextMenu();
    void HandleNewNode(material::NodeType type, const ImVec2& position);
    
    // Drag-drop handling
    void HandleDragDrop();
    void CreateNodeFromDrop(const std::string& path, const ImVec2& position);
    
    // Link validation
    bool CanCreateLink(ax::NodeEditor::PinId startPin, ax::NodeEditor::PinId endPin);
    
private:
    gfx::Device* m_Device = nullptr;
    material::MaterialAsset* m_Material = nullptr;
    
    ax::NodeEditor::EditorContext* m_NodeEditorContext = nullptr;
    
    bool m_Visible = false;
    bool m_ShowCreateMenu = false;
    ImVec2 m_CreateMenuPosition = { 0, 0 };
    
    // Pending link
    ax::NodeEditor::PinId m_NewLinkPin = 0;
    
    // UI state
    bool m_FirstFrame = true;
    std::string m_SearchFilter;
    
    // Quick-add search popup (Tab)
    bool m_ShowQuickAddPopup = false;
    ImVec2 m_QuickAddPosition = { 0, 0 };
    char m_QuickAddSearchBuffer[128] = {};
    int m_QuickAddSelectedIndex = 0;
    bool m_QuickAddFocusInput = false;
    
    // Compile status animation
    float m_CompileAnimTimer = 0.0f;
    
    // Auto compile (debounced)
    bool m_AutoCompile = true;
    bool m_WasDirty = false;
    float m_DirtySinceTime = 0.0f;
    
    // Material preview (offscreen)
    bool m_ShowPreview = true;
    bool m_PreviewDirty = true;
    uint64_t m_PreviewGraphHash = 0;
    uint32_t m_PreviewSize = 256;
    gfx::Image m_PreviewColor;
    gfx::Image m_PreviewDepth;
    VkSampler m_PreviewSampler = VK_NULL_HANDLE;
    VkRenderPass m_PreviewRenderPass = VK_NULL_HANDLE;
    VkFramebuffer m_PreviewFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet m_PreviewImGuiTex = VK_NULL_HANDLE;
    std::unique_ptr<assets::Mesh> m_PreviewSphere;
    
    // Asset navigation callback
    NavigateToAssetCallback m_NavigateToAsset;
    
    // Deferred color picker (popups can't open inside node editor)
    bool m_PendingColorEdit = false;
    material::NodeID m_PendingColorNodeId = 0;
    float m_PendingColor[3] = { 0.0f, 0.0f, 0.0f };
    
    // ColorRamp deferred editing
    bool m_PendingRampColorEdit = false;
    material::NodeID m_PendingRampNodeId = 0;
    int m_PendingRampStopIndex = -1;
    float m_PendingRampColor[3] = { 0.0f, 0.0f, 0.0f };
    
    // Undo support - track "before" values when editing starts
    material::NodeID m_EditingNodeId = 0;
    float m_BeforeFloat = 0.0f;
    glm::vec3 m_BeforeVec3{0.0f};
    bool m_IsEditingFloat = false;
    bool m_IsEditingVec3 = false;
    
    // Clipboard for copy/paste
    struct ClipboardNode {
        material::NodeType type;
        material::PinValue parameter;
        glm::vec2 position;
        int originalIdx; // index in clipboard for link remapping
    };
    struct ClipboardLink {
        int srcNodeIdx;  // index into clipboard nodes
        int srcPinIdx;   // output pin index on that node
        int dstNodeIdx;  // index into clipboard nodes
        int dstPinIdx;   // input pin index on that node
    };
    std::vector<ClipboardNode> m_ClipboardNodes;
    std::vector<ClipboardLink> m_ClipboardLinks;
    glm::vec2 m_ClipboardCenter{0.0f}; // center of copied selection (for offset on paste)
    
    // Clipboard methods
    void CopySelection();
    void PasteClipboard();
    void DuplicateSelection();
};

} // namespace lucent

