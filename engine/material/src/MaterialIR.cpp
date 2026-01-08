#include "lucent/material/MaterialIR.h"
#include "lucent/material/MaterialGraph.h"
#include "lucent/core/Log.h"
#include <unordered_map>

namespace lucent::material {

MaterialIR::GPUMaterialData MaterialIR::EvaluateConstant() const {
    GPUMaterialData data{};
    data.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    data.emissive = glm::vec4(0.0f);
    data.metallic = 0.0f;
    data.roughness = 0.5f;
    data.ior = 1.5f;
    data.flags = 0;
    
    if (instructions.empty()) return data;
    
    // Evaluate instructions to find constant values
    // This is a simplified evaluation that only handles constant nodes
    std::unordered_map<uint32_t, glm::vec4> values;
    
    for (const auto& instr : instructions) {
        glm::vec4 result(0.0f);
        
        switch (instr.type) {
            case IRNodeType::ConstFloat: {
                if (auto* v = std::get_if<float>(&instr.operands[0])) {
                    result = glm::vec4(*v, 0.0f, 0.0f, 0.0f);
                }
                break;
            }
            case IRNodeType::ConstVec3: {
                if (auto* v = std::get_if<glm::vec3>(&instr.operands[0])) {
                    result = glm::vec4(*v, 1.0f);
                }
                break;
            }
            case IRNodeType::ConstVec4: {
                if (auto* v = std::get_if<glm::vec4>(&instr.operands[0])) {
                    result = *v;
                }
                break;
            }
            default:
                // For non-constant nodes, use default/placeholder values
                result = glm::vec4(0.5f);
                break;
        }
        
        values[instr.id] = result;
    }
    
    // Extract PBR outputs
    if (output.baseColorInstr > 0 && values.count(output.baseColorInstr)) {
        data.baseColor = values[output.baseColorInstr];
    }
    if (output.metallicInstr > 0 && values.count(output.metallicInstr)) {
        data.metallic = values[output.metallicInstr].x;
    }
    if (output.roughnessInstr > 0 && values.count(output.roughnessInstr)) {
        data.roughness = values[output.roughnessInstr].x;
    }
    if (output.emissiveInstr > 0 && values.count(output.emissiveInstr)) {
        data.emissive = values[output.emissiveInstr];
    }
    
    return data;
}

bool MaterialIRCompiler::Compile(const MaterialGraph& graph, MaterialIR& outIR, std::string& errorMsg) {
    outIR = MaterialIR{};
    outIR.name = graph.GetName();
    
    // Map from graph node IDs to IR instruction IDs
    std::unordered_map<NodeID, uint32_t> nodeToInstr;
    uint32_t nextInstrId = 1;
    
    // Iterate all nodes in the graph
    const auto& nodes = graph.GetNodes();
    
    for (const auto& [nodeId, node] : nodes) {
        IRInstruction instr;
        instr.id = nextInstrId++;
        nodeToInstr[nodeId] = instr.id;
        
        // Convert node type
        switch (node.type) {
            case NodeType::ConstFloat:
                instr.type = IRNodeType::ConstFloat;
                if (auto* v = std::get_if<float>(&node.parameter)) {
                    instr.operands[0] = *v;
                }
                break;
                
            case NodeType::ConstVec3:
                instr.type = IRNodeType::ConstVec3;
                if (auto* v = std::get_if<glm::vec3>(&node.parameter)) {
                    instr.operands[0] = *v;
                }
                break;
                
            case NodeType::Texture2D:
            case NodeType::NormalMap: {
                instr.type = node.type == NodeType::Texture2D ? 
                    IRNodeType::Texture2D : IRNodeType::NormalMap;
                    
                // Extract texture path from parameter
                if (auto* str = std::get_if<std::string>(&node.parameter)) {
                    instr.texture.path = *str;
                    instr.texture.bindingSlot = static_cast<uint32_t>(outIR.textures.size());
                    instr.texture.isSRGB = (node.type == NodeType::Texture2D);
                    outIR.textures.push_back(instr.texture);
                }
                break;
            }
            
            case NodeType::UV:
                instr.type = IRNodeType::UV;
                break;
                
            case NodeType::Add:
                instr.type = IRNodeType::Add;
                break;
                
            case NodeType::Multiply:
                instr.type = IRNodeType::Multiply;
                break;
                
            case NodeType::Lerp:
                instr.type = IRNodeType::Lerp;
                break;
                
            case NodeType::SeparateVec3:
                instr.type = IRNodeType::SeparateRGB;
                break;
                
            case NodeType::CombineVec3:
                instr.type = IRNodeType::CombineRGB;
                break;
                
            case NodeType::Noise:
                instr.type = IRNodeType::Noise;
                instr.noiseScale = 1.0f;
                instr.noiseOctaves = 4;
                break;
                
            case NodeType::Fresnel:
                instr.type = IRNodeType::Fresnel;
                break;
                
            case NodeType::ColorRamp:
                instr.type = IRNodeType::ColorRamp;
                break;
                
            case NodeType::PBROutput:
                instr.type = IRNodeType::OutputPBR;
                break;
            
            // Utility nodes
            case NodeType::Reroute:
                // Passthrough - just copy input to output
                instr.type = IRNodeType::ConstFloat; // Placeholder - will be resolved in linking
                instr.operands[0] = 0.0f;
                break;
            
            case NodeType::Frame:
                // Frame is editor-only, skip entirely
                continue; // Don't add instruction
            
            // Type conversion nodes - these are semantic in IR
            case NodeType::FloatToVec3:
            case NodeType::Vec3ToFloat:
            case NodeType::Vec2ToVec3:
            case NodeType::Vec3ToVec4:
            case NodeType::Vec4ToVec3:
                // Treat as passthrough for IR purposes (actual conversion happens in shader codegen)
                instr.type = IRNodeType::ConstFloat; // Placeholder
                break;
                
            default:
                // Unsupported node type - use constant fallback
                errorMsg = "Unsupported node type: " + std::to_string(static_cast<int>(node.type));
                LUCENT_CORE_WARN("MaterialIR: {}", errorMsg);
                instr.type = IRNodeType::ConstFloat;
                instr.operands[0] = 0.5f;
                break;
        }
        
        outIR.instructions.push_back(instr);
        
        // Track PBR output node
        if (node.type == NodeType::PBROutput) {
            // Try to find connected inputs by looking at links
            const auto& links = graph.GetLinks();
            
            // Find pins that belong to this node
            const auto& pins = graph.GetPins();
            for (const auto& [pinId, pin] : pins) {
                if (pin.nodeId != nodeId || pin.direction != PinDirection::Input) continue;
                
                // Find link that ends at this pin
                for (const auto& [linkId, link] : links) {
                    if (link.endPinId != pinId) continue;
                    
                    // Find source pin and its node
                    auto srcPinIt = pins.find(link.startPinId);
                    if (srcPinIt == pins.end()) continue;
                    
                    auto srcNodeIt = nodeToInstr.find(srcPinIt->second.nodeId);
                    if (srcNodeIt == nodeToInstr.end()) continue;
                    
                    // Map pin name to output
                    if (pin.name == "Base Color") {
                        outIR.output.baseColorInstr = srcNodeIt->second;
                    } else if (pin.name == "Metallic") {
                        outIR.output.metallicInstr = srcNodeIt->second;
                    } else if (pin.name == "Roughness") {
                        outIR.output.roughnessInstr = srcNodeIt->second;
                    } else if (pin.name == "Emissive") {
                        outIR.output.emissiveInstr = srcNodeIt->second;
                    } else if (pin.name == "Normal") {
                        outIR.output.normalInstr = srcNodeIt->second;
                    }
                }
            }
        }
    }
    
    return true;
}

bool MaterialIRCompiler::IsTracedCompatible(const MaterialGraph& graph) {
    // Check if all nodes in the graph are supported in traced mode
    const auto& nodes = graph.GetNodes();
    
    for (const auto& [nodeId, node] : nodes) {
        switch (node.type) {
            // Supported nodes
            case NodeType::ConstFloat:
            case NodeType::ConstVec3:
            case NodeType::Texture2D:
            case NodeType::NormalMap:
            case NodeType::UV:
            case NodeType::Add:
            case NodeType::Multiply:
            case NodeType::Lerp:
            case NodeType::SeparateVec3:
            case NodeType::CombineVec3:
            case NodeType::Noise:
            case NodeType::Fresnel:
            case NodeType::ColorRamp:
            case NodeType::PBROutput:
            case NodeType::Remap:
            case NodeType::Step:
            case NodeType::Smoothstep:
            case NodeType::Sin:
            case NodeType::Cos:
            // Utility nodes
            case NodeType::Reroute:
            case NodeType::Frame:
            // Type conversion nodes
            case NodeType::FloatToVec3:
            case NodeType::Vec3ToFloat:
            case NodeType::Vec2ToVec3:
            case NodeType::Vec3ToVec4:
            case NodeType::Vec4ToVec3:
                break;
                
            // Unsupported or partially supported
            default:
                return false;
        }
    }
    
    return true;
}

} // namespace lucent::material
