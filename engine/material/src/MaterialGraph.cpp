#include "lucent/material/MaterialGraph.h"
#include "lucent/core/Log.h"
#include <algorithm>

namespace lucent::material {

MaterialGraph::MaterialGraph() {
}

void MaterialGraph::Clear() {
    m_Nodes.clear();
    m_Pins.clear();
    m_Links.clear();
    m_TextureSlots.clear();
    m_OutputNodeId = INVALID_NODE_ID;
    m_NextId = 1;
}

uint64_t MaterialGraph::AllocateId() {
    // Single monotonic ID stream for nodes, pins and links.
    // imgui-node-editor examples increment one uniqueId across all objects:
    // https://github.com/thedmd/imgui-node-editor
    const uint64_t id = m_NextId++;
    if (id == 0) {
        // Keep 0 reserved as INVALID.
        LUCENT_CORE_ERROR("MaterialGraph ID overflow (generated 0). Resetting allocator.");
        m_NextId = 1;
        return m_NextId++;
    }
    return id;
}

void MaterialGraph::CreateDefault() {
    Clear();
    
    // Create PBR output node
    m_OutputNodeId = CreateNode(NodeType::PBROutput, glm::vec2(400.0f, 200.0f));
    
    // Create a default color constant
    NodeID colorNode = CreateNode(NodeType::ConstVec3, glm::vec2(100.0f, 100.0f));
    if (auto* node = GetNode(colorNode)) {
        node->parameter = glm::vec3(0.8f, 0.8f, 0.8f);
    }
    
    // Connect color to base color input
    if (auto* outputNode = GetNode(m_OutputNodeId)) {
        if (!outputNode->inputPins.empty()) {
            // Find the base color input (first input)
            PinID baseColorInput = outputNode->inputPins[0];
            
            // Find the output pin of the color node
            if (auto* color = GetNode(colorNode)) {
                if (!color->outputPins.empty()) {
                    CreateLink(color->outputPins[0], baseColorInput);
                }
            }
        }
    }
    
    LUCENT_CORE_DEBUG("Created default material graph");
}

NodeID MaterialGraph::CreateNode(NodeType type, const glm::vec2& position) {
    NodeID id = AllocateId();
    
    MaterialNode node;
    node.id = id;
    node.type = type;
    node.name = GetNodeTypeName(type);
    node.position = position;
    
    // Set default parameter based on type
    switch (type) {
        case NodeType::ConstFloat:
            node.parameter = 0.0f;
            break;
        case NodeType::ConstVec2:
            node.parameter = glm::vec2(0.0f);
            break;
        case NodeType::ConstVec3:
            node.parameter = glm::vec3(0.5f, 0.5f, 0.5f);
            break;
        case NodeType::ConstVec4:
            node.parameter = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
        case NodeType::Texture2D:
        case NodeType::NormalMap:
            node.parameter = std::string(""); // Texture path
            break;
        case NodeType::Noise:
            // x=scale, y=detail(octaves), z=roughness, w=distortion
            node.parameter = glm::vec4(5.0f, 4.0f, 0.5f, 0.0f);
            break;
        case NodeType::ColorRamp:
            // Not used (ramp stored in node.parameter as a string blob for now)
            // Format: "RAMP:t,r,g,b;..." (alpha not supported by ImGradient)
            node.parameter = std::string("RAMP:0.0,0.0,0.0,0.0;1.0,1.0,1.0,1.0"); // two stops
            break;
        case NodeType::Fresnel:
            // power
            node.parameter = 5.0f;
            break;
        default:
            node.parameter = 0.0f;
            break;
    }
    
    m_Nodes[id] = node;
    SetupNodePins(m_Nodes[id]);
    
    return id;
}

void MaterialGraph::SetupNodePins(MaterialNode& node) {
    auto addInput = [&](const std::string& name, PinType type, PinValue defaultVal = 0.0f) {
        PinID pinId = CreatePin(node.id, name, type, PinDirection::Input, defaultVal);
        node.inputPins.push_back(pinId);
    };
    
    auto addOutput = [&](const std::string& name, PinType type) {
        PinID pinId = CreatePin(node.id, name, type, PinDirection::Output);
        node.outputPins.push_back(pinId);
    };
    
    switch (node.type) {
        // Input nodes
        case NodeType::UV:
            addOutput("UV", PinType::Vec2);
            break;
        case NodeType::VertexColor:
            addOutput("Color", PinType::Vec4);
            break;
        case NodeType::Time:
            addOutput("Time", PinType::Float);
            break;
            
        // Constants
        case NodeType::ConstFloat:
            addOutput("Value", PinType::Float);
            break;
        case NodeType::ConstVec2:
            addOutput("Value", PinType::Vec2);
            break;
        case NodeType::ConstVec3:
            addOutput("Value", PinType::Vec3);
            break;
        case NodeType::ConstVec4:
            addOutput("Value", PinType::Vec4);
            break;
            
        // Textures
        case NodeType::Texture2D:
            addInput("UV", PinType::Vec2, glm::vec2(0.0f));
            addOutput("RGB", PinType::Vec3);
            addOutput("R", PinType::Float);
            addOutput("G", PinType::Float);
            addOutput("B", PinType::Float);
            addOutput("A", PinType::Float);
            break;
        case NodeType::NormalMap:
            addInput("UV", PinType::Vec2, glm::vec2(0.0f));
            addInput("Strength", PinType::Float, 1.0f);
            addOutput("Normal", PinType::Vec3);
            break;

        // Procedural
        case NodeType::Noise:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f));
            addInput("Scale", PinType::Float, 5.0f);
            addInput("Detail", PinType::Float, 4.0f);
            addInput("Roughness", PinType::Float, 0.5f);
            addInput("Distortion", PinType::Float, 0.0f);
            addOutput("Value", PinType::Float);
            addOutput("Color", PinType::Vec3);
            break;

        case NodeType::ColorRamp:
            addInput("Factor", PinType::Float, 0.0f);
            addOutput("Color", PinType::Vec3);
            addOutput("Alpha", PinType::Float);
            break;
            
        // Math
        case NodeType::Add:
        case NodeType::Subtract:
        case NodeType::Multiply:
        case NodeType::Divide:
            addInput("A", PinType::Vec3, glm::vec3(0.0f));
            addInput("B", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Power:
            addInput("Base", PinType::Float, 0.0f);
            addInput("Exp", PinType::Float, 1.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Lerp:
            addInput("A", PinType::Vec3, glm::vec3(0.0f));
            addInput("B", PinType::Vec3, glm::vec3(1.0f));
            addInput("T", PinType::Float, 0.5f);
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Remap:
            addInput("Value", PinType::Float, 0.0f);
            addInput("In Min", PinType::Float, 0.0f);
            addInput("In Max", PinType::Float, 1.0f);
            addInput("Out Min", PinType::Float, 0.0f);
            addInput("Out Max", PinType::Float, 1.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Step:
            addInput("Edge", PinType::Float, 0.5f);
            addInput("X", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Smoothstep:
            addInput("Edge0", PinType::Float, 0.0f);
            addInput("Edge1", PinType::Float, 1.0f);
            addInput("X", PinType::Float, 0.5f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Sin:
            addInput("X", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Cos:
            addInput("X", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Clamp:
            addInput("Value", PinType::Float, 0.0f);
            addInput("Min", PinType::Float, 0.0f);
            addInput("Max", PinType::Float, 1.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::OneMinus:
            addInput("Value", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Abs:
            addInput("Value", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;

        // Shading helpers
        case NodeType::Fresnel:
            addInput("Power", PinType::Float, 5.0f);
            addOutput("F", PinType::Float);
            break;
            
        // Vector ops
        case NodeType::Dot:
            addInput("A", PinType::Vec3, glm::vec3(0.0f));
            addInput("B", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Normalize:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f, 1.0f, 0.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Length:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Result", PinType::Float);
            break;
            
        // Separate/Combine
        case NodeType::SeparateVec3:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f));
            addOutput("R", PinType::Float);
            addOutput("G", PinType::Float);
            addOutput("B", PinType::Float);
            break;
        case NodeType::SeparateVec4:
            addInput("Vector", PinType::Vec4, glm::vec4(0.0f));
            addOutput("R", PinType::Float);
            addOutput("G", PinType::Float);
            addOutput("B", PinType::Float);
            addOutput("A", PinType::Float);
            break;
        case NodeType::CombineVec3:
            addInput("R", PinType::Float, 0.0f);
            addInput("G", PinType::Float, 0.0f);
            addInput("B", PinType::Float, 0.0f);
            addOutput("Vector", PinType::Vec3);
            break;
        case NodeType::CombineVec4:
            addInput("R", PinType::Float, 0.0f);
            addInput("G", PinType::Float, 0.0f);
            addInput("B", PinType::Float, 0.0f);
            addInput("A", PinType::Float, 1.0f);
            addOutput("Vector", PinType::Vec4);
            break;
            
        // PBR Output
        case NodeType::PBROutput:
            addInput("Base Color", PinType::Vec3, glm::vec3(0.8f, 0.8f, 0.8f));
            addInput("Metallic", PinType::Float, 0.0f);
            addInput("Roughness", PinType::Float, 0.5f);
            addInput("Normal", PinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f));
            addInput("Emissive", PinType::Vec3, glm::vec3(0.0f));
            addInput("Alpha", PinType::Float, 1.0f);
            break;
    }
}

void MaterialGraph::DeleteNode(NodeID nodeId) {
    auto it = m_Nodes.find(nodeId);
    if (it == m_Nodes.end()) return;
    
    // Don't allow deleting the output node
    if (nodeId == m_OutputNodeId) {
        LUCENT_CORE_WARN("Cannot delete the output node");
        return;
    }
    
    MaterialNode& node = it->second;
    
    // Delete all pins and their links
    for (PinID pinId : node.inputPins) {
        // Find and delete links connected to this pin
        std::vector<LinkID> linksToDelete;
        for (const auto& [linkId, link] : m_Links) {
            if (link.startPinId == pinId || link.endPinId == pinId) {
                linksToDelete.push_back(linkId);
            }
        }
        for (LinkID linkId : linksToDelete) {
            m_Links.erase(linkId);
        }
        m_Pins.erase(pinId);
    }
    
    for (PinID pinId : node.outputPins) {
        std::vector<LinkID> linksToDelete;
        for (const auto& [linkId, link] : m_Links) {
            if (link.startPinId == pinId || link.endPinId == pinId) {
                linksToDelete.push_back(linkId);
            }
        }
        for (LinkID linkId : linksToDelete) {
            m_Links.erase(linkId);
        }
        m_Pins.erase(pinId);
    }
    
    m_Nodes.erase(nodeId);
}

MaterialNode* MaterialGraph::GetNode(NodeID nodeId) {
    auto it = m_Nodes.find(nodeId);
    return it != m_Nodes.end() ? &it->second : nullptr;
}

const MaterialNode* MaterialGraph::GetNode(NodeID nodeId) const {
    auto it = m_Nodes.find(nodeId);
    return it != m_Nodes.end() ? &it->second : nullptr;
}

PinID MaterialGraph::CreatePin(NodeID nodeId, const std::string& name, PinType type, 
                                PinDirection direction, PinValue defaultValue) {
    PinID id = AllocateId();
    
    MaterialPin pin;
    pin.id = id;
    pin.nodeId = nodeId;
    pin.name = name;
    pin.type = type;
    pin.direction = direction;
    pin.defaultValue = defaultValue;
    
    m_Pins[id] = pin;
    return id;
}

MaterialPin* MaterialGraph::GetPin(PinID pinId) {
    auto it = m_Pins.find(pinId);
    return it != m_Pins.end() ? &it->second : nullptr;
}

const MaterialPin* MaterialGraph::GetPin(PinID pinId) const {
    auto it = m_Pins.find(pinId);
    return it != m_Pins.end() ? &it->second : nullptr;
}

NodeID MaterialGraph::GetPinNodeId(PinID pinId) const {
    auto it = m_Pins.find(pinId);
    return it != m_Pins.end() ? it->second.nodeId : INVALID_NODE_ID;
}

LinkID MaterialGraph::CreateLink(PinID startPinId, PinID endPinId) {
    if (!CanCreateLink(startPinId, endPinId)) {
        return INVALID_LINK_ID;
    }
    
    // Remove any existing link to the end pin (inputs can only have one connection)
    LinkID existingLink = FindLinkByEndPin(endPinId);
    if (existingLink != INVALID_LINK_ID) {
        DeleteLink(existingLink);
    }
    
    LinkID id = AllocateId();
    
    MaterialLink link;
    link.id = id;
    link.startPinId = startPinId;
    link.endPinId = endPinId;
    
    m_Links[id] = link;
    return id;
}

void MaterialGraph::DeleteLink(LinkID linkId) {
    m_Links.erase(linkId);
}

bool MaterialGraph::CanCreateLink(PinID startPinId, PinID endPinId) const {
    const MaterialPin* startPin = GetPin(startPinId);
    const MaterialPin* endPin = GetPin(endPinId);
    
    if (!startPin || !endPin) return false;
    
    // Start must be output, end must be input
    if (startPin->direction != PinDirection::Output) return false;
    if (endPin->direction != PinDirection::Input) return false;
    
    // Can't link to self
    if (startPin->nodeId == endPin->nodeId) return false;
    
    // Type compatibility (allow some implicit conversions)
    // For now, allow float<->vec3 conversions (broadcast/extract)
    if (startPin->type == PinType::Sampler2D || endPin->type == PinType::Sampler2D) {
        return false; // Samplers can't be linked
    }
    
    return true;
}

const MaterialLink* MaterialGraph::GetLink(LinkID linkId) const {
    auto it = m_Links.find(linkId);
    return it != m_Links.end() ? &it->second : nullptr;
}

LinkID MaterialGraph::FindLinkByEndPin(PinID endPinId) const {
    for (const auto& [id, link] : m_Links) {
        if (link.endPinId == endPinId) {
            return id;
        }
    }
    return INVALID_LINK_ID;
}

int MaterialGraph::AddTextureSlot(const std::string& path, bool sRGB) {
    int index = static_cast<int>(m_TextureSlots.size());
    TextureSlot slot;
    slot.path = path;
    slot.sRGB = sRGB;
    slot.bindingIndex = index;
    m_TextureSlots.push_back(slot);
    return index;
}

void MaterialGraph::SetTextureSlot(int index, const std::string& path, bool sRGB) {
    if (index >= 0 && index < static_cast<int>(m_TextureSlots.size())) {
        m_TextureSlots[index].path = path;
        m_TextureSlots[index].sRGB = sRGB;
    }
}

uint64_t MaterialGraph::ComputeHash() const {
    // Simple FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    
    auto hashCombine = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    
    // Hash nodes
    for (const auto& [id, node] : m_Nodes) {
        hashCombine(id);
        hashCombine(static_cast<uint64_t>(node.type));
        
        // Hash parameter based on type
        if (std::holds_alternative<float>(node.parameter)) {
            union { float f; uint32_t u; } conv;
            conv.f = std::get<float>(node.parameter);
            hashCombine(conv.u);
        } else if (std::holds_alternative<glm::vec3>(node.parameter)) {
            const auto& v = std::get<glm::vec3>(node.parameter);
            union { float f; uint32_t u; } conv;
            conv.f = v.x; hashCombine(conv.u);
            conv.f = v.y; hashCombine(conv.u);
            conv.f = v.z; hashCombine(conv.u);
        } else if (std::holds_alternative<std::string>(node.parameter)) {
            const auto& s = std::get<std::string>(node.parameter);
            for (char c : s) {
                hashCombine(static_cast<uint64_t>(c));
            }
        }
    }
    
    // Hash links
    for (const auto& [id, link] : m_Links) {
        hashCombine(link.startPinId);
        hashCombine(link.endPinId);
    }
    
    // Hash texture slots
    for (const auto& slot : m_TextureSlots) {
        for (char c : slot.path) {
            hashCombine(static_cast<uint64_t>(c));
        }
        hashCombine(slot.sRGB ? 1 : 0);
    }
    
    return hash;
}

} // namespace lucent::material

