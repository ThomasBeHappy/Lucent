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
    
    // Get topologically sorted nodes
    auto sortedNodes = graph.TopologicalSort();
    
    for (NodeID nodeId : sortedNodes) {
        const MaterialNode* node = graph.GetNode(nodeId);
        if (!node) continue;
        
        IRInstruction instr;
        instr.id = nextInstrId++;
        nodeToInstr[nodeId] = instr.id;
        
        // Convert node type
        switch (node->type) {
            case NodeType::ConstFloat:
                instr.type = IRNodeType::ConstFloat;
                instr.operands[0] = std::get<float>(node->parameter);
                break;
                
            case NodeType::ConstVec3:
                instr.type = IRNodeType::ConstVec3;
                instr.operands[0] = std::get<glm::vec3>(node->parameter);
                break;
                
            case NodeType::Texture2D:
            case NodeType::NormalMap: {
                instr.type = node->type == NodeType::Texture2D ? 
                    IRNodeType::Texture2D : IRNodeType::NormalMap;
                    
                // Extract texture path from parameter
                if (auto* str = std::get_if<std::string>(&node->parameter)) {
                    instr.texture.path = *str;
                    instr.texture.bindingSlot = static_cast<uint32_t>(outIR.textures.size());
                    instr.texture.isSRGB = (node->type == NodeType::Texture2D);
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
                
            case NodeType::SeparateXYZ:
                instr.type = IRNodeType::SeparateRGB;
                break;
                
            case NodeType::CombineXYZ:
                instr.type = IRNodeType::CombineRGB;
                break;
                
            case NodeType::Noise:
                instr.type = IRNodeType::Noise;
                // Noise parameters would be extracted here
                instr.noiseScale = 1.0f;
                instr.noiseOctaves = 4;
                break;
                
            case NodeType::Fresnel:
                instr.type = IRNodeType::Fresnel;
                break;
                
            case NodeType::ColorRamp: {
                instr.type = IRNodeType::ColorRamp;
                // Parse color ramp stops from parameter
                if (auto* str = std::get_if<std::string>(&node->parameter)) {
                    // Parse RAMP: format from ColorRampStops
                    // This is a simplified version - full implementation would parse the stops
                }
                break;
            }
            
            case NodeType::OutputPBR:
                instr.type = IRNodeType::OutputPBR;
                // Map connected inputs to PBR outputs
                break;
                
            default:
                // Unsupported node type
                errorMsg = "Unsupported node type: " + std::to_string(static_cast<int>(node->type));
                LUCENT_CORE_WARN("MaterialIR: {}", errorMsg);
                // Continue with a constant fallback
                instr.type = IRNodeType::ConstFloat;
                instr.operands[0] = 0.5f;
                break;
        }
        
        // Resolve input connections
        for (const auto& [inputPinId, outputPinId] : graph.GetLinks()) {
            // Find if this link connects to one of our input pins
            const MaterialPin* inputPin = graph.GetPin(inputPinId);
            const MaterialPin* outputPin = graph.GetPin(outputPinId);
            
            if (inputPin && inputPin->nodeId == nodeId) {
                // This node receives input from outputPin's node
                if (outputPin) {
                    auto srcIt = nodeToInstr.find(outputPin->nodeId);
                    if (srcIt != nodeToInstr.end()) {
                        // Store reference to source instruction
                        // Map input pin index to operand index
                        // (simplified - full version would track pin indices)
                        instr.operands[0] = srcIt->second;
                    }
                }
            }
        }
        
        outIR.instructions.push_back(instr);
        
        // Track PBR output connections
        if (node->type == NodeType::OutputPBR) {
            // The OutputPBR node itself is the output marker
            // We need to trace back its input connections
            for (const auto& pin : node->inputs) {
                if (pin.name == "Base Color") {
                    // Find what's connected to this pin
                    for (const auto& link : graph.GetLinks()) {
                        if (link.inputPinId == pin.id) {
                            const MaterialPin* srcPin = graph.GetPin(link.outputPinId);
                            if (srcPin && nodeToInstr.count(srcPin->nodeId)) {
                                outIR.output.baseColorInstr = nodeToInstr[srcPin->nodeId];
                            }
                        }
                    }
                } else if (pin.name == "Metallic") {
                    for (const auto& link : graph.GetLinks()) {
                        if (link.inputPinId == pin.id) {
                            const MaterialPin* srcPin = graph.GetPin(link.outputPinId);
                            if (srcPin && nodeToInstr.count(srcPin->nodeId)) {
                                outIR.output.metallicInstr = nodeToInstr[srcPin->nodeId];
                            }
                        }
                    }
                } else if (pin.name == "Roughness") {
                    for (const auto& link : graph.GetLinks()) {
                        if (link.inputPinId == pin.id) {
                            const MaterialPin* srcPin = graph.GetPin(link.outputPinId);
                            if (srcPin && nodeToInstr.count(srcPin->nodeId)) {
                                outIR.output.roughnessInstr = nodeToInstr[srcPin->nodeId];
                            }
                        }
                    }
                } else if (pin.name == "Emissive") {
                    for (const auto& link : graph.GetLinks()) {
                        if (link.inputPinId == pin.id) {
                            const MaterialPin* srcPin = graph.GetPin(link.outputPinId);
                            if (srcPin && nodeToInstr.count(srcPin->nodeId)) {
                                outIR.output.emissiveInstr = nodeToInstr[srcPin->nodeId];
                            }
                        }
                    }
                }
            }
        }
    }
    
    return true;
}

bool MaterialIRCompiler::IsTracedCompatible(const MaterialGraph& graph) {
    // Check if all nodes in the graph are supported in traced mode
    auto sortedNodes = graph.TopologicalSort();
    
    for (NodeID nodeId : sortedNodes) {
        const MaterialNode* node = graph.GetNode(nodeId);
        if (!node) continue;
        
        switch (node->type) {
            // Supported nodes
            case NodeType::ConstFloat:
            case NodeType::ConstVec3:
            case NodeType::Texture2D:
            case NodeType::NormalMap:
            case NodeType::UV:
            case NodeType::Add:
            case NodeType::Multiply:
            case NodeType::Lerp:
            case NodeType::SeparateXYZ:
            case NodeType::CombineXYZ:
            case NodeType::Noise:
            case NodeType::Fresnel:
            case NodeType::ColorRamp:
            case NodeType::OutputPBR:
            case NodeType::Remap:
            case NodeType::Step:
            case NodeType::Smoothstep:
            case NodeType::Sin:
            case NodeType::Cos:
                break;
                
            // Unsupported or partially supported
            default:
                return false;
        }
    }
    
    return true;
}

} // namespace lucent::material

