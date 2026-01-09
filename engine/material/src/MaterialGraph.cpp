#include "lucent/material/MaterialGraph.h"
#include "lucent/core/Log.h"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace lucent::material {

namespace {

struct CustomCodeDecl {
    bool isOutput = false; // false = input (includes 'uniform')
    PinType type = PinType::Float;
    std::string name;
};

static bool ParsePinTypeToken(const std::string& tok, PinType& outType) {
    if (tok == "float") { outType = PinType::Float; return true; }
    if (tok == "vec2")  { outType = PinType::Vec2;  return true; }
    if (tok == "vec3")  { outType = PinType::Vec3;  return true; }
    if (tok == "vec4")  { outType = PinType::Vec4;  return true; }
    return false;
}

static std::string TrimLeft(std::string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

static bool ParseCustomCodeDeclLine(const std::string& line, CustomCodeDecl& outDecl) {
    // Accept:
    //   in vec3 Name;
    //   uniform float Strength;
    //   out vec2 UV;
    std::string s = TrimLeft(line);
    if (s.empty()) return false;
    // Skip comments (// ...)
    if (s.rfind("//", 0) == 0) return false;

    bool isOut = false;
    bool isIn = false;
    bool isUniform = false;
    if (s.rfind("out ", 0) == 0) { isOut = true; s = TrimLeft(s.substr(4)); }
    else if (s.rfind("in ", 0) == 0) { isIn = true; s = TrimLeft(s.substr(3)); }
    else if (s.rfind("uniform ", 0) == 0) { isUniform = true; s = TrimLeft(s.substr(8)); }
    else return false;

    std::istringstream iss(s);
    std::string typeTok;
    std::string nameTok;
    if (!(iss >> typeTok >> nameTok)) return false;
    // Strip trailing ';' from name
    if (!nameTok.empty() && nameTok.back() == ';') nameTok.pop_back();
    if (nameTok.empty()) return false;

    PinType pinType{};
    if (!ParsePinTypeToken(typeTok, pinType)) return false;

    outDecl.isOutput = isOut;
    outDecl.type = pinType;
    outDecl.name = nameTok;
    (void)isIn;
    (void)isUniform;
    return true;
}

static std::vector<CustomCodeDecl> ParseCustomCodeDecls(const std::string& code) {
    std::vector<CustomCodeDecl> decls;
    std::istringstream ss(code);
    std::string line;
    while (std::getline(ss, line)) {
        CustomCodeDecl d{};
        if (ParseCustomCodeDeclLine(line, d)) {
            decls.push_back(std::move(d));
        }
    }
    return decls;
}

} // namespace

MaterialGraph::MaterialGraph() {
}

void MaterialGraph::Clear() {
    m_Nodes.clear();
    m_Pins.clear();
    m_Links.clear();
    m_TextureSlots.clear();
    m_OutputNodeId = INVALID_NODE_ID;
    m_VolumeOutputNodeId = INVALID_NODE_ID;
    m_Domain = MaterialDomain::Surface;
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
    // Prevent duplicate output nodes
    if (type == NodeType::PBROutput && m_OutputNodeId != INVALID_NODE_ID) {
        LUCENT_CORE_WARN("Cannot create duplicate PBR Output node");
        return INVALID_NODE_ID;
    }
    if (type == NodeType::VolumetricOutput && m_VolumeOutputNodeId != INVALID_NODE_ID) {
        LUCENT_CORE_WARN("Cannot create duplicate Volumetric Output node");
        return INVALID_NODE_ID;
    }
    
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
        case NodeType::Frame:
            // Format: "FRAME:w,h,r,g,b,a;title"
            node.parameter = std::string("FRAME:300,200,0.2,0.2,0.2,0.5;Comment");
            break;
        case NodeType::CustomCode:
            // Free-form code block stored as a string.
            // Pins are inferred from `in/uniform/out` declarations when rebuilt.
            node.parameter = std::string(
                "// Custom Code (Surface)\n"
                "// - Default pins always exist: `In` (vec3), `Out` (vec3)\n"
                "// - Declare extra pins:\n"
                "//     uniform float Strength;\n"
                "//     in vec3 Color;\n"
                "//     out vec3 Result;\n"
                "\n"
                "Out = In;\n"
            );
            break;
        default:
            node.parameter = 0.0f;
            break;
    }
    
    m_Nodes[id] = node;
    SetupNodePins(m_Nodes[id]);
    
    // Track output nodes
    if (type == NodeType::PBROutput) {
        m_OutputNodeId = id;
    } else if (type == NodeType::VolumetricOutput) {
        m_VolumeOutputNodeId = id;
        // If this is the first output node, switch to volume domain
        if (m_OutputNodeId == INVALID_NODE_ID) {
            m_Domain = MaterialDomain::Volume;
        }
    }
    
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
        case NodeType::WorldPosition:
            addOutput("Position", PinType::Vec3);
            break;
        case NodeType::WorldNormal:
            addOutput("Normal", PinType::Vec3);
            break;
        case NodeType::ViewDirection:
            addOutput("View", PinType::Vec3);
            break;

        case NodeType::CustomCode: {
            // Always provide a default RGB passthrough-like interface.
            addInput("In", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Out", PinType::Vec3);

            const std::string code = std::holds_alternative<std::string>(node.parameter)
                ? std::get<std::string>(node.parameter)
                : std::string();
            const auto decls = ParseCustomCodeDecls(code);

            auto hasName = [&](const std::vector<PinID>& pins, const std::string& name) -> bool {
                for (PinID pid : pins) {
                    const MaterialPin* p = GetPin(pid);
                    if (p && p->name == name) return true;
                }
                return false;
            };

            for (const auto& d : decls) {
                // Avoid duplicates (including the defaults).
                if (d.isOutput) {
                    if (hasName(node.outputPins, d.name)) continue;
                    addOutput(d.name, d.type);
                } else {
                    if (hasName(node.inputPins, d.name)) continue;
                    addInput(d.name, d.type, 0.0f);
                }
            }
            break;
        }
            
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

        case NodeType::Min:
        case NodeType::Max:
            addInput("A", PinType::Vec3, glm::vec3(0.0f));
            addInput("B", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Saturate:
            addInput("Value", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Sqrt:
        case NodeType::Floor:
        case NodeType::Ceil:
        case NodeType::Fract:
        case NodeType::Exp:
        case NodeType::Log:
        case NodeType::Negate:
            addInput("Value", PinType::Float, 0.0f);
            addOutput("Result", PinType::Float);
            break;
        case NodeType::Mod:
            addInput("A", PinType::Float, 0.0f);
            addInput("B", PinType::Float, 1.0f);
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

        case NodeType::Cross:
            addInput("A", PinType::Vec3, glm::vec3(1.0f, 0.0f, 0.0f));
            addInput("B", PinType::Vec3, glm::vec3(0.0f, 1.0f, 0.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Reflect:
            addInput("I", PinType::Vec3, glm::vec3(0.0f, 0.0f, -1.0f));
            addInput("N", PinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f));
            addOutput("Result", PinType::Vec3);
            break;
        case NodeType::Refract:
            addInput("I", PinType::Vec3, glm::vec3(0.0f, 0.0f, -1.0f));
            addInput("N", PinType::Vec3, glm::vec3(0.0f, 0.0f, 1.0f));
            addInput("Eta", PinType::Float, 1.0f / 1.5f);
            addOutput("Result", PinType::Vec3);
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
        case NodeType::SeparateVec2:
            addInput("Vector", PinType::Vec2, glm::vec2(0.0f));
            addOutput("X", PinType::Float);
            addOutput("Y", PinType::Float);
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
        case NodeType::CombineVec2:
            addInput("X", PinType::Float, 0.0f);
            addInput("Y", PinType::Float, 0.0f);
            addOutput("Vector", PinType::Vec2);
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
            
        // Volumetric Output (Blender-like ports)
        case NodeType::VolumetricOutput:
            addInput("Color", PinType::Vec3, glm::vec3(0.8f, 0.8f, 0.8f));      // Scattering color
            addInput("Density", PinType::Float, 1.0f);                           // Volume density
            addInput("Anisotropy", PinType::Float, 0.0f);                        // Phase function g (-1 to 1)
            addInput("Absorption", PinType::Vec3, glm::vec3(0.0f));              // Absorption color
            addInput("Emission", PinType::Vec3, glm::vec3(0.0f));                // Volume emission
            addInput("Emission Strength", PinType::Float, 1.0f);                 // Emission multiplier
            break;
            
        // Utility nodes
        case NodeType::Reroute:
            // Reroute is polymorphic - pin type is determined when connected
            // Default to Vec3 (most common case)
            addInput("In", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Out", PinType::Vec3);
            break;
        case NodeType::Frame:
            // Frame is editor-only, no pins
            // Size and title stored in parameter as "FRAME:w,h,r,g,b,a;title"
            break;
            
        // Type conversion nodes
        case NodeType::FloatToVec3:
            addInput("Value", PinType::Float, 0.0f);
            addOutput("Vector", PinType::Vec3);
            break;
        case NodeType::Vec3ToFloat:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f));
            addOutput("Value", PinType::Float);
            break;
        case NodeType::Vec2ToVec3:
            addInput("Vector", PinType::Vec2, glm::vec2(0.0f));
            addInput("Z", PinType::Float, 0.0f);
            addOutput("Vector", PinType::Vec3);
            break;
        case NodeType::Vec3ToVec4:
            addInput("Vector", PinType::Vec3, glm::vec3(0.0f));
            addInput("A", PinType::Float, 1.0f);
            addOutput("Vector", PinType::Vec4);
            break;
        case NodeType::Vec4ToVec3:
            addInput("Vector", PinType::Vec4, glm::vec4(0.0f));
            addOutput("Vector", PinType::Vec3);
            break;
    }
}

void MaterialGraph::DeleteNode(NodeID nodeId) {
    auto it = m_Nodes.find(nodeId);
    if (it == m_Nodes.end()) return;
    
    // Don't allow deleting output nodes
    if (nodeId == m_OutputNodeId) {
        LUCENT_CORE_WARN("Cannot delete the PBR Output node");
        return;
    }
    if (nodeId == m_VolumeOutputNodeId) {
        LUCENT_CORE_WARN("Cannot delete the Volumetric Output node");
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

void MaterialGraph::DeleteNodePinsAndLinks(MaterialNode& node) {
    // Delete links touching any pin of this node, then delete pins.
    std::vector<PinID> allPins;
    allPins.reserve(node.inputPins.size() + node.outputPins.size());
    allPins.insert(allPins.end(), node.inputPins.begin(), node.inputPins.end());
    allPins.insert(allPins.end(), node.outputPins.begin(), node.outputPins.end());

    // Delete links referencing these pins
    std::vector<LinkID> linksToDelete;
    for (const auto& [linkId, link] : m_Links) {
        for (PinID pid : allPins) {
            if (link.startPinId == pid || link.endPinId == pid) {
                linksToDelete.push_back(linkId);
                break;
            }
        }
    }
    for (LinkID lid : linksToDelete) {
        m_Links.erase(lid);
    }

    // Delete pins
    for (PinID pid : allPins) {
        m_Pins.erase(pid);
    }
    node.inputPins.clear();
    node.outputPins.clear();
}

void MaterialGraph::RebuildNodePins(NodeID nodeId) {
    auto it = m_Nodes.find(nodeId);
    if (it == m_Nodes.end()) return;
    MaterialNode& node = it->second;

    // Don't rebuild output nodes (pin layout is stable)
    if (node.type == NodeType::PBROutput || node.type == NodeType::VolumetricOutput) return;

    if (node.type == NodeType::CustomCode) {
        // Preserve pins (and links) for unchanged declarations where possible.
        struct PinSpec {
            std::string name;
            PinType type;
            PinDirection dir;
        };

        auto buildDesired = [&](std::vector<PinSpec>& ins, std::vector<PinSpec>& outs) {
            ins.clear();
            outs.clear();
            ins.push_back({ "In", PinType::Vec3, PinDirection::Input });
            outs.push_back({ "Out", PinType::Vec3, PinDirection::Output });

            const std::string code = std::holds_alternative<std::string>(node.parameter)
                ? std::get<std::string>(node.parameter)
                : std::string();
            const auto decls = ParseCustomCodeDecls(code);

            auto addUnique = [](std::vector<PinSpec>& v, const PinSpec& s) {
                for (const auto& it : v) {
                    if (it.name == s.name && it.dir == s.dir) return;
                }
                v.push_back(s);
            };

            for (const auto& d : decls) {
                if (d.isOutput) addUnique(outs, { d.name, d.type, PinDirection::Output });
                else addUnique(ins, { d.name, d.type, PinDirection::Input });
            }
        };

        std::vector<PinSpec> desiredIn, desiredOut;
        buildDesired(desiredIn, desiredOut);

        auto deleteLinksForPin = [&](PinID pid) {
            std::vector<LinkID> linksToDelete;
            linksToDelete.reserve(8);
            for (const auto& [linkId, link] : m_Links) {
                if (link.startPinId == pid || link.endPinId == pid) {
                    linksToDelete.push_back(linkId);
                }
            }
            for (LinkID lid : linksToDelete) {
                m_Links.erase(lid);
            }
        };

        // Build lookup of existing pins by name+dir.
        struct ExistingInfo { PinID id; PinType type; };
        std::unordered_map<std::string, ExistingInfo> existingIn;
        std::unordered_map<std::string, ExistingInfo> existingOut;
        existingIn.reserve(node.inputPins.size());
        existingOut.reserve(node.outputPins.size());

        for (PinID pid : node.inputPins) {
            const MaterialPin* p = GetPin(pid);
            if (!p) continue;
            existingIn[p->name] = { pid, p->type };
        }
        for (PinID pid : node.outputPins) {
            const MaterialPin* p = GetPin(pid);
            if (!p) continue;
            existingOut[p->name] = { pid, p->type };
        }

        // Build new pin lists; reuse old PinIDs when unchanged.
        std::vector<PinID> newInputs;
        std::vector<PinID> newOutputs;
        newInputs.reserve(desiredIn.size());
        newOutputs.reserve(desiredOut.size());

        auto ensurePin = [&](const PinSpec& s) -> PinID {
            auto& table = (s.dir == PinDirection::Input) ? existingIn : existingOut;
            auto it2 = table.find(s.name);
            if (it2 != table.end() && it2->second.type == s.type) {
                return it2->second.id; // unchanged; keep id+links
            }

            // If name exists but type changed, delete old pin+links.
            if (it2 != table.end()) {
                deleteLinksForPin(it2->second.id);
                m_Pins.erase(it2->second.id);
                table.erase(it2);
            }

            // Create new pin
            PinValue def = 0.0f;
            if (s.dir == PinDirection::Input) {
                if (s.type == PinType::Vec2) def = glm::vec2(0.0f);
                else if (s.type == PinType::Vec3) def = glm::vec3(0.0f);
                else if (s.type == PinType::Vec4) def = glm::vec4(0.0f);
            }
            return CreatePin(node.id, s.name, s.type, s.dir, def);
        };

        for (const auto& s : desiredIn) newInputs.push_back(ensurePin(s));
        for (const auto& s : desiredOut) newOutputs.push_back(ensurePin(s));

        // Delete any remaining old pins not present in desired schema.
        auto desiredNameSet = [&](const std::vector<PinSpec>& v) {
            std::unordered_map<std::string, bool> set;
            set.reserve(v.size());
            for (const auto& s : v) set[s.name] = true;
            return set;
        };
        const auto desiredInSet = desiredNameSet(desiredIn);
        const auto desiredOutSet = desiredNameSet(desiredOut);

        for (PinID pid : node.inputPins) {
            const MaterialPin* p = GetPin(pid);
            if (!p) continue;
            if (desiredInSet.find(p->name) == desiredInSet.end()) {
                deleteLinksForPin(pid);
                m_Pins.erase(pid);
            }
        }
        for (PinID pid : node.outputPins) {
            const MaterialPin* p = GetPin(pid);
            if (!p) continue;
            if (desiredOutSet.find(p->name) == desiredOutSet.end()) {
                deleteLinksForPin(pid);
                m_Pins.erase(pid);
            }
        }

        node.inputPins = std::move(newInputs);
        node.outputPins = std::move(newOutputs);
        return;
    }

    // Default behavior for other nodes
    DeleteNodePinsAndLinks(node);
    SetupNodePins(node);
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
    
    // Hash nodes (deterministic order)
    std::vector<NodeID> nodeIds;
    nodeIds.reserve(m_Nodes.size());
    for (const auto& [id, _] : m_Nodes) {
        nodeIds.push_back(id);
    }
    std::sort(nodeIds.begin(), nodeIds.end());
    
    for (NodeID id : nodeIds) {
        const auto it = m_Nodes.find(id);
        if (it == m_Nodes.end()) continue;
        const auto& node = it->second;
        
        hashCombine(id);
        hashCombine(static_cast<uint64_t>(node.type));
        
        // Hash parameter based on type
        union { float f; uint32_t u; } conv;
        if (std::holds_alternative<float>(node.parameter)) {
            conv.f = std::get<float>(node.parameter);
            hashCombine(conv.u);
        } else if (std::holds_alternative<glm::vec2>(node.parameter)) {
            const auto& v = std::get<glm::vec2>(node.parameter);
            conv.f = v.x; hashCombine(conv.u);
            conv.f = v.y; hashCombine(conv.u);
        } else if (std::holds_alternative<glm::vec3>(node.parameter)) {
            const auto& v = std::get<glm::vec3>(node.parameter);
            conv.f = v.x; hashCombine(conv.u);
            conv.f = v.y; hashCombine(conv.u);
            conv.f = v.z; hashCombine(conv.u);
        } else if (std::holds_alternative<glm::vec4>(node.parameter)) {
            const auto& v = std::get<glm::vec4>(node.parameter);
            conv.f = v.x; hashCombine(conv.u);
            conv.f = v.y; hashCombine(conv.u);
            conv.f = v.z; hashCombine(conv.u);
            conv.f = v.w; hashCombine(conv.u);
        } else if (std::holds_alternative<std::string>(node.parameter)) {
            const auto& s = std::get<std::string>(node.parameter);
            hashCombine(static_cast<uint64_t>(s.size()));
            for (char c : s) {
                hashCombine(static_cast<uint64_t>(static_cast<unsigned char>(c)));
            }
        }
    }
    
    // Hash links (deterministic order)
    std::vector<std::pair<PinID, PinID>> linkPairs;
    linkPairs.reserve(m_Links.size());
    for (const auto& [_, link] : m_Links) {
        linkPairs.emplace_back(link.startPinId, link.endPinId);
    }
    std::sort(linkPairs.begin(), linkPairs.end());
    for (const auto& [startPin, endPin] : linkPairs) {
        hashCombine(startPin);
        hashCombine(endPin);
    }
    
    // Hash texture slots
    for (const auto& slot : m_TextureSlots) {
        for (char c : slot.path) {
            hashCombine(static_cast<uint64_t>(static_cast<unsigned char>(c)));
        }
        hashCombine(slot.sRGB ? 1 : 0);
    }
    
    return hash;
}

} // namespace lucent::material

