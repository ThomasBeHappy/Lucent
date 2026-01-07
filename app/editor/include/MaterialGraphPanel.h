#pragma once

#include "lucent/material/MaterialGraph.h"
#include "lucent/material/MaterialAsset.h"
#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <string>
#include <unordered_map>

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
    
private:
    void DrawToolbar();
    void DrawNodeEditor();
    void DrawNodeCreationMenu();
    void DrawCompileStatus();
    
    // Node drawing helpers
    void DrawNode(const material::MaterialNode& node);
    void DrawPin(const material::MaterialPin& pin, bool isInput);
    ImColor GetPinColor(material::PinType type);
    
    // Popup menus
    void HandleContextMenu();
    void HandleNewNode(material::NodeType type, const ImVec2& position);
    
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
    
    // Compile status animation
    float m_CompileAnimTimer = 0.0f;
};

} // namespace lucent

