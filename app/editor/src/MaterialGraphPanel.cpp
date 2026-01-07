#include "MaterialGraphPanel.h"
#include "lucent/material/MaterialAsset.h"
#include "lucent/core/Log.h"
#include <imgui-node-editor/imgui_node_editor.h>
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace ed = ax::NodeEditor;

namespace lucent {

MaterialGraphPanel::~MaterialGraphPanel() {
    Shutdown();
}

bool MaterialGraphPanel::Init(gfx::Device* device) {
    m_Device = device;
    
    // Create node editor context
    ed::Config config;
    config.SettingsFile = "material_node_editor.json";
    m_NodeEditorContext = ed::CreateEditor(&config);
    
    LUCENT_CORE_INFO("Material graph panel initialized");
    return true;
}

void MaterialGraphPanel::Shutdown() {
    if (m_NodeEditorContext) {
        ed::DestroyEditor(m_NodeEditorContext);
        m_NodeEditorContext = nullptr;
    }
}

void MaterialGraphPanel::SetMaterial(material::MaterialAsset* material) {
    m_Material = material;
    m_FirstFrame = true;
}

material::MaterialAsset* MaterialGraphPanel::CreateNewMaterial() {
    auto& manager = material::MaterialAssetManager::Get();
    auto* mat = manager.CreateMaterial("New Material");
    if (mat) {
        mat->Recompile();
        SetMaterial(mat);
    }
    return mat;
}

void MaterialGraphPanel::Draw() {
    if (!m_Visible) return;
    
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    
    if (ImGui::Begin("Material Graph", &m_Visible, ImGuiWindowFlags_MenuBar)) {
        DrawToolbar();
        DrawNodeEditor();
        DrawCompileStatus();
    }
    ImGui::End();
}

void MaterialGraphPanel::DrawToolbar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Material", "Ctrl+N")) {
                CreateNewMaterial();
            }
            if (ImGui::MenuItem("Open Material...", "Ctrl+O")) {
                // TODO: Open file dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Material != nullptr)) {
                if (m_Material && !m_Material->GetFilePath().empty()) {
                    material::MaterialAssetManager::Get().SaveMaterial(m_Material, m_Material->GetFilePath());
                }
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_Material != nullptr)) {
                // TODO: Save file dialog
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Selected", "Delete")) {
                // TODO: Delete selected nodes
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Add")) {
            DrawNodeCreationMenu();
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }
    
    // Toolbar buttons
    if (m_Material) {
        // Material name
        ImGui::Text("Material: %s", m_Material->GetGraph().GetName().c_str());
        ImGui::SameLine();
        
        // Compile button
        if (ImGui::Button("Compile")) {
            m_Material->Recompile();
            m_CompileAnimTimer = 1.0f;
        }
        
        ImGui::SameLine();
        
        // Status indicator
        if (m_Material->IsValid()) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "[OK]");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[ERROR]");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", m_Material->GetCompileError().c_str());
            }
        }
        
        if (m_Material->IsDirty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "(unsaved)");
        }
    } else {
        ImGui::Text("No material selected");
        ImGui::SameLine();
        if (ImGui::Button("Create New")) {
            CreateNewMaterial();
        }
    }
    
    ImGui::Separator();
}

void MaterialGraphPanel::DrawNodeEditor() {
    if (!m_Material) {
        ImGui::TextDisabled("Select or create a material to edit");
        return;
    }
    
    // Check if we're dragging a compatible payload BEFORE drawing anything
    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    bool isDraggingAsset = activePayload != nullptr &&
        (activePayload->IsDataType("TEXTURE_PATH") ||
         activePayload->IsDataType("ASSET_PATH"));
    
    bool dropped = false;
    std::string droppedPath;
    ImVec2 dropPos;
    
    // Only create drop zone when actually dragging something compatible
    if (isDraggingAsset) {
        ImVec2 editorSize = ImGui::GetContentRegionAvail();
        
        // Create a visible hint that we can drop here
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 0.3f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.9f, 0.5f));
        ImGui::Button("##GraphDropZone", editorSize);
        ImGui::PopStyleColor(2);
        
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TEXTURE_PATH")) {
                droppedPath = std::string(static_cast<const char*>(payload->Data));
                dropPos = ImGui::GetMousePos();
                dropped = true;
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                droppedPath = std::string(static_cast<const char*>(payload->Data));
                dropPos = ImGui::GetMousePos();
                dropped = true;
            }
            ImGui::EndDragDropTarget();
        }
        
        // Reset cursor to draw node editor in same position
        ImVec2 cursorPos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y - editorSize.y));
    }
    
    ed::SetCurrentEditor(m_NodeEditorContext);
    
    ed::Begin("MaterialNodeEditor", ImVec2(0.0f, 0.0f));
    
    // Process the drop now that we have the node editor context
    if (dropped && !droppedPath.empty()) {
        ImVec2 canvasPos = ed::ScreenToCanvas(dropPos);
        CreateNodeFromDrop(droppedPath, canvasPos);
    }
    
    material::MaterialGraph& graph = m_Material->GetGraph();
    
    // Draw all nodes
    for (const auto& [nodeId, node] : graph.GetNodes()) {
        DrawNode(node);
    }
    
    // Draw all links
    for (const auto& [linkId, link] : graph.GetLinks()) {
        ed::Link(ed::LinkId(linkId), ed::PinId(link.startPinId), ed::PinId(link.endPinId));
    }
    
    // Handle link creation
    if (ed::BeginCreate()) {
        ed::PinId startPinId, endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            if (startPinId && endPinId) {
                // Get actual pin IDs
                material::PinID start = static_cast<material::PinID>(startPinId.Get());
                material::PinID end = static_cast<material::PinID>(endPinId.Get());
                
                // Ensure start is output and end is input
                const material::MaterialPin* startPin = graph.GetPin(start);
                const material::MaterialPin* endPin = graph.GetPin(end);
                
                if (startPin && endPin) {
                    // Swap if needed
                    if (startPin->direction == material::PinDirection::Input) {
                        std::swap(start, end);
                    }
                    
                    if (graph.CanCreateLink(start, end)) {
                        if (ed::AcceptNewItem(ImColor(0.2f, 0.8f, 0.2f, 1.0f), 2.0f)) {
                            graph.CreateLink(start, end);
                            m_Material->MarkDirty();
                        }
                    } else {
                        ed::RejectNewItem(ImColor(0.9f, 0.2f, 0.2f, 1.0f), 2.0f);
                    }
                }
            }
        }
    }
    ed::EndCreate();
    
    // Handle link deletion
    if (ed::BeginDelete()) {
        ed::LinkId linkId;
        while (ed::QueryDeletedLink(&linkId)) {
            if (ed::AcceptDeletedItem()) {
                graph.DeleteLink(static_cast<material::LinkID>(linkId.Get()));
                m_Material->MarkDirty();
            }
        }
        
        ed::NodeId nodeId;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (ed::AcceptDeletedItem()) {
                graph.DeleteNode(static_cast<material::NodeID>(nodeId.Get()));
                m_Material->MarkDirty();
            }
        }
    }
    ed::EndDelete();
    
    // Handle context menu
    HandleContextMenu();
    
    ed::End();
    
    // Handle deferred color pickers (must be outside node editor)
    // ConstVec3 color picker
    if (m_PendingColorEdit) {
        ImGui::OpenPopup("ColorPickerPopup");
        m_PendingColorEdit = false;
    }
    
    if (ImGui::BeginPopup("ColorPickerPopup")) {
        if (ImGui::ColorPicker3("##picker", m_PendingColor, ImGuiColorEditFlags_PickerHueWheel)) {
            // Update the node
            auto& colorGraph = m_Material->GetGraph();
            const auto* colorNode = colorGraph.GetNode(m_PendingColorNodeId);
            if (colorNode) {
                auto* mutableNode = const_cast<material::MaterialNode*>(colorNode);
                mutableNode->parameter = glm::vec3(m_PendingColor[0], m_PendingColor[1], m_PendingColor[2]);
                m_Material->MarkDirty();
            }
        }
        ImGui::EndPopup();
    }
    
    // ColorRamp stop color picker
    if (m_PendingRampColorEdit) {
        ImGui::OpenPopup("RampColorPickerPopup");
        m_PendingRampColorEdit = false;
    }
    
    if (ImGui::BeginPopup("RampColorPickerPopup")) {
        if (ImGui::ColorPicker3("##ramppicker", m_PendingRampColor, ImGuiColorEditFlags_PickerHueWheel)) {
            // Update the color ramp node stop by modifying the blob
            auto& rampGraph = m_Material->GetGraph();
            const auto* rampNode = rampGraph.GetNode(m_PendingRampNodeId);
            if (rampNode && m_PendingRampStopIndex >= 0) {
                // Parse existing blob
                std::string blob = std::holds_alternative<std::string>(rampNode->parameter) ? 
                    std::get<std::string>(rampNode->parameter) : "RAMP:0.0,0.0,0.0,0.0;1.0,1.0,1.0,1.0";
                
                struct Stop { float t, r, g, b; };
                std::vector<Stop> stops;
                const std::string prefix = "RAMP:";
                size_t start = (blob.rfind(prefix, 0) == 0) ? prefix.size() : 0;
                while (start < blob.size()) {
                    size_t end = blob.find(';', start);
                    std::string token = blob.substr(start, end == std::string::npos ? std::string::npos : (end - start));
                    if (!token.empty()) {
                        Stop s;
                        if (sscanf_s(token.c_str(), "%f,%f,%f,%f", &s.t, &s.r, &s.g, &s.b) == 4) {
                            stops.push_back(s);
                        }
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                
                // Update the specific stop
                if (m_PendingRampStopIndex < (int)stops.size()) {
                    stops[m_PendingRampStopIndex].r = m_PendingRampColor[0];
                    stops[m_PendingRampStopIndex].g = m_PendingRampColor[1];
                    stops[m_PendingRampStopIndex].b = m_PendingRampColor[2];
                    
                    // Rebuild blob
                    std::string newBlob = "RAMP:";
                    for (size_t i = 0; i < stops.size(); ++i) {
                        if (i > 0) newBlob += ";";
                        char buf[64];
                        snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f", 
                            stops[i].t, stops[i].r, stops[i].g, stops[i].b);
                        newBlob += buf;
                    }
                    
                    auto* mutableNode = const_cast<material::MaterialNode*>(rampNode);
                    mutableNode->parameter = newBlob;
                    m_Material->MarkDirty();
                }
            }
        }
        ImGui::EndPopup();
    }
    
    ed::SetCurrentEditor(nullptr);
}

void MaterialGraphPanel::DrawNode(const material::MaterialNode& node) {
    // Push unique ID for entire node to avoid conflicts
    ImGui::PushID(static_cast<int>(node.id));
    
    ed::BeginNode(ed::NodeId(node.id));
    
    // Node title
    ImGui::Text("%s", node.name.c_str());
    ImGui::Dummy(ImVec2(100.0f, 0.0f)); // Min width
    
    // Draw inputs and outputs
    ImGui::BeginGroup();
    
    const auto& graph = m_Material->GetGraph();
    
    // Input pins on left
    for (material::PinID pinId : node.inputPins) {
        const material::MaterialPin* pin = graph.GetPin(pinId);
        if (pin) {
            DrawPin(*pin, true);
        }
    }
    
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    
    // Output pins on right
    for (material::PinID pinId : node.outputPins) {
        const material::MaterialPin* pin = graph.GetPin(pinId);
        if (pin) {
            DrawPin(*pin, false);
        }
    }
    
    ImGui::EndGroup();
    
    // Node-specific parameters
    switch (node.type) {
        case material::NodeType::ConstFloat: {
            float value = std::holds_alternative<float>(node.parameter) ? 
                          std::get<float>(node.parameter) : 0.0f;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("##floatval", &value, 0.01f)) {
                auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                mutableNode->parameter = value;
                m_Material->MarkDirty();
            }
            break;
        }
        case material::NodeType::ConstVec3: {
            glm::vec3 value = std::holds_alternative<glm::vec3>(node.parameter) ?
                              std::get<glm::vec3>(node.parameter) : glm::vec3(0.0f);
            
            // Draw a color button - clicking it will open a popup outside node editor
            ImVec4 col(value.x, value.y, value.z, 1.0f);
            if (ImGui::ColorButton("##colorbtn", col, ImGuiColorEditFlags_NoTooltip, ImVec2(80, 20))) {
                m_PendingColorEdit = true;
                m_PendingColorNodeId = node.id;
                m_PendingColor[0] = value.x;
                m_PendingColor[1] = value.y;
                m_PendingColor[2] = value.z;
            }
            break;
        }
        case material::NodeType::Texture2D:
        case material::NodeType::NormalMap: {
            std::string path = std::holds_alternative<std::string>(node.parameter) ?
                               std::get<std::string>(node.parameter) : "";
            
            // Extract just the filename for display
            std::string displayName = path.empty() ? "(no texture)" : path;
            size_t lastSlash = displayName.find_last_of("/\\");
            if (lastSlash != std::string::npos) {
                displayName = displayName.substr(lastSlash + 1);
            }
            
            // Truncate if too long
            if (displayName.length() > 18) {
                displayName = displayName.substr(0, 15) + "...";
            }
            
            // Show as clickable link
            if (!path.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                if (ImGui::Selectable(displayName.c_str(), false, ImGuiSelectableFlags_None, ImVec2(130.0f, 0.0f))) {
                    // Navigate to the asset in content browser
                    if (m_NavigateToAsset) {
                        m_NavigateToAsset(path);
                    }
                }
                ImGui::PopStyleColor();
                
                // Tooltip with full path
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Click to locate: %s", path.c_str());
                }
            } else {
                ImGui::TextDisabled("%s", displayName.c_str());
            }
            
            // Drag-drop target on the node itself to change texture
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TEXTURE_PATH")) {
                    std::string newPath(static_cast<const char*>(payload->Data));
                    auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                    mutableNode->parameter = newPath;
                    m_Material->MarkDirty();
                    LUCENT_CORE_INFO("Updated texture node: {}", newPath);
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string newPath(static_cast<const char*>(payload->Data));
                    // Check if it's a texture
                    std::string ext = newPath.substr(newPath.find_last_of('.'));
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr" || ext == ".tga") {
                        auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                        mutableNode->parameter = newPath;
                        m_Material->MarkDirty();
                        LUCENT_CORE_INFO("Updated texture node: {}", newPath);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            break;
        }
        case material::NodeType::Noise: {
            glm::vec4 p = std::holds_alternative<glm::vec4>(node.parameter)
                ? std::get<glm::vec4>(node.parameter)
                : glm::vec4(5.0f, 4.0f, 0.5f, 0.0f);

            // p.x = scale, p.y = detail, p.z = roughness, p.w = distortion
            ImGui::SetNextItemWidth(170.0f);
            bool changed = false;
            changed |= ImGui::DragFloat("Scale##noise", &p.x, 0.1f, 0.0f, 100.0f, "%.2f");
            changed |= ImGui::DragFloat("Detail##noise", &p.y, 0.1f, 1.0f, 12.0f, "%.1f");
            changed |= ImGui::SliderFloat("Roughness##noise", &p.z, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::DragFloat("Distort##noise", &p.w, 0.05f, 0.0f, 10.0f, "%.2f");

            if (changed) {
                auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                mutableNode->parameter = p;
                m_Material->MarkDirty();
            }
            break;
        }
        case material::NodeType::ColorRamp: {
            // ColorRamp (custom UI). Store stops as: "RAMP:t,r,g,b;..."
            // NOTE: We intentionally do NOT use ImGradient::Edit() from ImGuizmo because the vcpkg
            // build calls ImDrawList::AddRect() with legacy corner flag values and triggers an ImGui assert.
            std::string blob = std::holds_alternative<std::string>(node.parameter) ? std::get<std::string>(node.parameter) : std::string();
            if (blob.empty()) blob = "RAMP:0.0,0.0,0.0,0.0;1.0,1.0,1.0,1.0";

            struct Stop { float t; ImVec4 c; }; // rgb in xyz, w unused
            std::vector<Stop> stops;
            stops.reserve(8);

            try {
                const std::string prefix = "RAMP:";
                size_t start = (blob.rfind(prefix, 0) == 0) ? prefix.size() : 0;
                while (start < blob.size()) {
                    size_t end = blob.find(';', start);
                    std::string token = blob.substr(start, end == std::string::npos ? std::string::npos : (end - start));
                    if (!token.empty()) {
                        float t = 0, r = 1, g = 1, b = 1;
                        // t,r,g,b
                        const int n = sscanf_s(token.c_str(), "%f,%f,%f,%f", &t, &r, &g, &b);
                        if (n == 4) {
                            t = std::clamp(t, 0.0f, 1.0f);
                            stops.push_back({ t, ImVec4(r, g, b, 1.0f) });
                        }
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            } catch (...) {
                // ignore parse errors; we fall back below
            }

            if (stops.size() < 2) {
                stops.clear();
                stops.push_back({ 0.0f, ImVec4(0.0f, 0.0f, 0.0f, 1.0f) });
                stops.push_back({ 1.0f, ImVec4(1.0f, 1.0f, 1.0f, 1.0f) });
            }

            static std::unordered_map<uint64_t, int> s_RampSelection;
            int& selection = s_RampSelection[node.id];
            static std::unordered_map<uint64_t, bool> s_RampDragging;
            bool& dragging = s_RampDragging[node.id];

            // Sort
            std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
            if (selection < 0 || selection >= (int)stops.size()) selection = 0;

            auto eval = [&](float t) -> ImVec4 {
                t = std::clamp(t, 0.0f, 1.0f);
                if (t <= stops.front().t) return stops.front().c;
                if (t >= stops.back().t) return stops.back().c;
                for (size_t i = 0; i + 1 < stops.size(); ++i) {
                    if (t >= stops[i].t && t <= stops[i + 1].t) {
                        float denom = std::max(stops[i + 1].t - stops[i].t, 1e-6f);
                        float u = (t - stops[i].t) / denom;
                        u = std::clamp(u, 0.0f, 1.0f);
                        return ImVec4(
                            (1.0f - u) * stops[i].c.x + u * stops[i + 1].c.x,
                            (1.0f - u) * stops[i].c.y + u * stops[i + 1].c.y,
                            (1.0f - u) * stops[i].c.z + u * stops[i + 1].c.z,
                            1.0f
                        );
                    }
                }
                return stops.back().c;
            };

            bool changed = false;
            ImGui::TextDisabled("Ramp");
            ImVec2 barSize(220.0f, 18.0f);
            ImVec2 barPos = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw background
            const float rounding = 4.0f;
            dl->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), IM_COL32(25, 25, 30, 255), rounding);

            // Draw gradient segments
            for (size_t i = 0; i + 1 < stops.size(); ++i) {
                float x0 = barPos.x + stops[i].t * barSize.x;
                float x1 = barPos.x + stops[i + 1].t * barSize.x;
                ImU32 c0 = ImColor(stops[i].c.x, stops[i].c.y, stops[i].c.z);
                ImU32 c1 = ImColor(stops[i + 1].c.x, stops[i + 1].c.y, stops[i + 1].c.z);
                dl->AddRectFilledMultiColor(
                    ImVec2(x0, barPos.y),
                    ImVec2(x1, barPos.y + barSize.y),
                    c0, c1, c1, c0
                );
            }

            // Interaction box
            ImGui::InvisibleButton("##ramp_bar", barSize);
            const bool hoveredBar = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;

            // Double click bar to add stop
            if (hoveredBar && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                float t = std::clamp((mouse.x - barPos.x) / barSize.x, 0.0f, 1.0f);
                ImVec4 c = eval(t);
                stops.push_back({ t, c });
                std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
                // select closest new stop
                int best = 0; float bestD = 1e9f;
                for (int i = 0; i < (int)stops.size(); ++i) {
                    float d = std::fabs(stops[i].t - t);
                    if (d < bestD) { bestD = d; best = i; }
                }
                selection = best;
                changed = true;
            }

            // Draw stops
            const float markerW = 10.0f;
            const float markerH = 8.0f;
            int hoveredStop = -1;
            for (int i = 0; i < (int)stops.size(); ++i) {
                float x = barPos.x + stops[i].t * barSize.x;
                ImVec2 p0(x, barPos.y + barSize.y + 2.0f);
                ImVec2 p1(x - markerW * 0.5f, barPos.y + barSize.y + 2.0f + markerH);
                ImVec2 p2(x + markerW * 0.5f, barPos.y + barSize.y + 2.0f + markerH);

                ImU32 fill = ImColor(stops[i].c.x, stops[i].c.y, stops[i].c.z);
                ImU32 border = (i == selection) ? IM_COL32(220, 220, 220, 255) : IM_COL32(120, 120, 120, 255);
                dl->AddTriangleFilled(p0, p1, p2, fill);
                dl->AddTriangle(p0, p1, p2, border, (i == selection) ? 2.0f : 1.0f);

                // Hit test
                ImRect r(ImVec2(p1.x, p0.y), ImVec2(p2.x, p2.y));
                if (r.Contains(mouse)) hoveredStop = i;
            }

            // Click to select + drag to move
            if (hoveredStop >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selection = hoveredStop;
                dragging = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
            if (dragging && selection >= 0 && selection < (int)stops.size()) {
                float t = std::clamp((mouse.x - barPos.x) / barSize.x, 0.0f, 1.0f);
                // Keep endpoints pinned (Blender-like default)
                if (selection != 0 && selection != (int)stops.size() - 1) {
                    stops[selection].t = t;
                    std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
                    // re-find selected stop by nearest color+pos isn't stable; pick nearest position
                    int best = 0; float bestD = 1e9f;
                    for (int i = 0; i < (int)stops.size(); ++i) {
                        float d = std::fabs(stops[i].t - t);
                        if (d < bestD) { bestD = d; best = i; }
                    }
                    selection = best;
                    changed = true;
                }
            }

            // Right click stop to delete (not endpoints)
            if (hoveredStop > 0 && hoveredStop < (int)stops.size() - 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                stops.erase(stops.begin() + hoveredStop);
                selection = std::clamp(selection, 0, (int)stops.size() - 1);
                changed = true;
            }

            // Selected stop controls
            if (selection >= 0 && selection < (int)stops.size()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Selected Stop");
                
                // Draw color button - clicking opens popup outside node editor
                ImVec4 stopCol = stops[selection].c;
                if (ImGui::ColorButton("##rampcolbtn", stopCol, ImGuiColorEditFlags_NoTooltip, ImVec2(60, 16))) {
                    m_PendingRampColorEdit = true;
                    m_PendingRampNodeId = node.id;
                    m_PendingRampStopIndex = selection;
                    m_PendingRampColor[0] = stopCol.x;
                    m_PendingRampColor[1] = stopCol.y;
                    m_PendingRampColor[2] = stopCol.z;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(click to edit)");
                
                float t = stops[selection].t;
                if (selection == 0 || selection == (int)stops.size() - 1) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::SliderFloat("Pos##ramp", &t, 0.0f, 1.0f, "%.3f")) {
                    stops[selection].t = std::clamp(t, 0.0f, 1.0f);
                    std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
                    changed = true;
                }
                if (selection == 0 || selection == (int)stops.size() - 1) {
                    ImGui::EndDisabled();
                }
            }

            if (changed) {
                std::sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.t < b.t; });
                std::string out = "RAMP:";
                for (size_t i = 0; i < stops.size(); ++i) {
                    char buf[128]{};
                    sprintf_s(buf, "%g,%g,%g,%g", stops[i].t, stops[i].c.x, stops[i].c.y, stops[i].c.z);
                    out += buf;
                    if (i + 1 < stops.size()) out += ";";
                }

                auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                mutableNode->parameter = out;
                m_Material->MarkDirty();
            }
            break;
        }
        case material::NodeType::Fresnel: {
            float power = std::holds_alternative<float>(node.parameter) ? std::get<float>(node.parameter) : 5.0f;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat("Power##fresnel", &power, 0.1f, 0.1f, 20.0f, "%.2f")) {
                auto* mutableNode = const_cast<material::MaterialNode*>(&node);
                mutableNode->parameter = power;
                m_Material->MarkDirty();
            }
            break;
        }
        default:
            break;
    }
    
    ed::EndNode();
    
    ImGui::PopID();
}

void MaterialGraphPanel::DrawPin(const material::MaterialPin& pin, bool isInput) {
    // Push unique ID for this pin
    ImGui::PushID(static_cast<int>(pin.id));
    
    ImColor color = GetPinColor(pin.type);
    
    // Pin icon size
    const float pinIconSize = 12.0f;
    
    if (isInput) {
        // Input: icon on left, label on right
        ed::BeginPin(ed::PinId(pin.id), ed::PinKind::Input);
        ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
        ed::PinPivotSize(ImVec2(0, 0));
        
        // Draw a small colored circle as the pin icon
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddCircleFilled(
            ImVec2(cursorPos.x + pinIconSize * 0.5f, cursorPos.y + pinIconSize * 0.5f),
            pinIconSize * 0.4f,
            color
        );
        ImGui::Dummy(ImVec2(pinIconSize, pinIconSize));
        
        ed::EndPin();
        
        // Draw label outside the pin (not part of hitbox)
        ImGui::SameLine();
        ImGui::Text("%s", pin.name.c_str());
    } else {
        // Output: label on left, icon on right
        ImGui::Text("%s", pin.name.c_str());
        ImGui::SameLine();
        
        ed::BeginPin(ed::PinId(pin.id), ed::PinKind::Output);
        ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
        ed::PinPivotSize(ImVec2(0, 0));
        
        // Draw a small colored circle as the pin icon
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddCircleFilled(
            ImVec2(cursorPos.x + pinIconSize * 0.5f, cursorPos.y + pinIconSize * 0.5f),
            pinIconSize * 0.4f,
            color
        );
        ImGui::Dummy(ImVec2(pinIconSize, pinIconSize));
        
        ed::EndPin();
    }
    
    ImGui::PopID();
}

ImColor MaterialGraphPanel::GetPinColor(material::PinType type) {
    switch (type) {
        case material::PinType::Float:    return ImColor(0.5f, 0.8f, 0.5f); // Green
        case material::PinType::Vec2:     return ImColor(0.5f, 0.7f, 0.9f); // Light blue
        case material::PinType::Vec3:     return ImColor(0.9f, 0.8f, 0.2f); // Yellow
        case material::PinType::Vec4:     return ImColor(0.9f, 0.5f, 0.2f); // Orange
        case material::PinType::Sampler2D:return ImColor(0.8f, 0.3f, 0.8f); // Purple
    }
    return ImColor(0.7f, 0.7f, 0.7f);
}

void MaterialGraphPanel::HandleContextMenu() {
    ed::Suspend();
    
    if (ed::ShowBackgroundContextMenu()) {
        ImGui::OpenPopup("CreateNodeMenu");
        // Store canvas position when menu opens (while editor context is active)
        m_CreateMenuPosition = ed::ScreenToCanvas(ImGui::GetMousePos());
    }
    
    if (ImGui::BeginPopup("CreateNodeMenu")) {
        DrawNodeCreationMenu();
        ImGui::EndPopup();
    }
    
    ed::Resume();
}

void MaterialGraphPanel::DrawNodeCreationMenu() {
    if (!m_Material) return;
    
    material::MaterialGraph& graph = m_Material->GetGraph();
    
    // Use cached canvas position (set when context menu opens)
    ImVec2 canvasPos = m_CreateMenuPosition;
    
    auto addNodeMenuItem = [&](const char* name, material::NodeType type) {
        if (ImGui::MenuItem(name)) {
            graph.CreateNode(type, glm::vec2(canvasPos.x, canvasPos.y));
            m_Material->MarkDirty();
        }
    };
    
    if (ImGui::BeginMenu("Input")) {
        addNodeMenuItem("UV", material::NodeType::UV);
        addNodeMenuItem("Vertex Color", material::NodeType::VertexColor);
        addNodeMenuItem("Time", material::NodeType::Time);
        ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Constants")) {
        addNodeMenuItem("Float", material::NodeType::ConstFloat);
        addNodeMenuItem("Vector2", material::NodeType::ConstVec2);
        addNodeMenuItem("Vector3 / Color", material::NodeType::ConstVec3);
        addNodeMenuItem("Vector4", material::NodeType::ConstVec4);
        ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Texture")) {
        addNodeMenuItem("Texture2D", material::NodeType::Texture2D);
        addNodeMenuItem("Normal Map", material::NodeType::NormalMap);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Procedural")) {
        addNodeMenuItem("Noise", material::NodeType::Noise);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Color")) {
        addNodeMenuItem("Color Ramp", material::NodeType::ColorRamp);
        ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Math")) {
        addNodeMenuItem("Add", material::NodeType::Add);
        addNodeMenuItem("Subtract", material::NodeType::Subtract);
        addNodeMenuItem("Multiply", material::NodeType::Multiply);
        addNodeMenuItem("Divide", material::NodeType::Divide);
        addNodeMenuItem("Power", material::NodeType::Power);
        addNodeMenuItem("Lerp", material::NodeType::Lerp);
        addNodeMenuItem("Remap", material::NodeType::Remap);
        addNodeMenuItem("Step", material::NodeType::Step);
        addNodeMenuItem("Smoothstep", material::NodeType::Smoothstep);
        addNodeMenuItem("Sine", material::NodeType::Sin);
        addNodeMenuItem("Cosine", material::NodeType::Cos);
        addNodeMenuItem("Clamp", material::NodeType::Clamp);
        addNodeMenuItem("One Minus", material::NodeType::OneMinus);
        addNodeMenuItem("Abs", material::NodeType::Abs);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Shading")) {
        addNodeMenuItem("Fresnel", material::NodeType::Fresnel);
        ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Vector")) {
        addNodeMenuItem("Dot Product", material::NodeType::Dot);
        addNodeMenuItem("Normalize", material::NodeType::Normalize);
        addNodeMenuItem("Length", material::NodeType::Length);
        ImGui::EndMenu();
    }
    
    if (ImGui::BeginMenu("Convert")) {
        addNodeMenuItem("Separate RGB", material::NodeType::SeparateVec3);
        addNodeMenuItem("Separate RGBA", material::NodeType::SeparateVec4);
        addNodeMenuItem("Combine RGB", material::NodeType::CombineVec3);
        addNodeMenuItem("Combine RGBA", material::NodeType::CombineVec4);
        ImGui::EndMenu();
    }
}

void MaterialGraphPanel::DrawCompileStatus() {
    if (!m_Material) return;
    
    // Animate compile flash
    if (m_CompileAnimTimer > 0.0f) {
        m_CompileAnimTimer -= ImGui::GetIO().DeltaTime * 2.0f;
    }
    
    // Show errors in a separate panel
    if (!m_Material->IsValid() && !m_Material->GetCompileError().empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "Compile Error:");
        
        // Scrollable error text
        ImGui::BeginChild("ErrorLog", ImVec2(0, 100), true);
        ImGui::TextWrapped("%s", m_Material->GetCompileError().c_str());
        ImGui::EndChild();
    }
}

bool MaterialGraphPanel::CanCreateLink(ed::PinId startPin, ed::PinId endPin) {
    if (!m_Material) return false;
    
    material::PinID start = static_cast<material::PinID>(startPin.Get());
    material::PinID end = static_cast<material::PinID>(endPin.Get());
    
    return m_Material->GetGraph().CanCreateLink(start, end);
}

void MaterialGraphPanel::HandleDragDrop() {
    // Suspend node editor to handle ImGui drag-drop
    ed::Suspend();
    
    // Check if we're dragging something over the node editor
    // Use the window itself as the drop target
    if (ImGui::BeginDragDropTarget()) {
        // Accept texture files
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TEXTURE_PATH")) {
            std::string path(static_cast<const char*>(payload->Data));
            
            // Get drop position in canvas coordinates
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 canvasPos = ed::ScreenToCanvas(mousePos);
            
            CreateNodeFromDrop(path, canvasPos);
        }
        
        // Accept generic asset paths (we'll filter by extension)
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string path(static_cast<const char*>(payload->Data));
            
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 canvasPos = ed::ScreenToCanvas(mousePos);
            
            CreateNodeFromDrop(path, canvasPos);
        }
        
        ImGui::EndDragDropTarget();
    }
    
    ed::Resume();
}

void MaterialGraphPanel::CreateNodeFromDrop(const std::string& path, const ImVec2& position) {
    if (!m_Material) return;
    
    material::MaterialGraph& graph = m_Material->GetGraph();
    
    // Determine file type by extension
    std::string ext = path.substr(path.find_last_of('.'));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".hdr" || ext == ".tga" || ext == ".bmp") {
        // Create a Texture2D node
        material::NodeID nodeId = graph.CreateNode(material::NodeType::Texture2D, glm::vec2(position.x, position.y));
        
        // Set the texture path as the parameter
        auto* node = graph.GetNode(nodeId);
        if (node) {
            node->parameter = path;
            LUCENT_CORE_INFO("Created Texture2D node from drop: {}", path);
        }
        
        m_Material->MarkDirty();
    }
    // Add more file type handling here as needed (e.g., .lmat for sub-materials)
}

} // namespace lucent

