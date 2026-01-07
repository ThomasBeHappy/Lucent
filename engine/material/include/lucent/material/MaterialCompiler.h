#pragma once

#include "lucent/material/MaterialGraph.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace lucent::material {

// Result of material compilation
struct CompileResult {
    bool success = false;
    std::string fragmentShaderGLSL;
    std::vector<uint32_t> fragmentShaderSPIRV;
    std::string errorMessage;
    uint64_t graphHash = 0;
};

// Compiles a MaterialGraph to GLSL and SPIR-V
class MaterialCompiler {
public:
    MaterialCompiler() = default;
    
    // Compile the material graph to GLSL and SPIR-V
    CompileResult Compile(const MaterialGraph& graph);
    
    // Get the standard vertex shader SPIR-V (shared by all materials)
    static const std::vector<uint32_t>& GetStandardVertexShaderSPIRV();
    
private:
    // Generate GLSL fragment shader from graph
    std::string GenerateFragmentGLSL(const MaterialGraph& graph);
    
    // Compile GLSL to SPIR-V
    bool CompileGLSLToSPIRV(const std::string& glsl, std::vector<uint32_t>& spirv, std::string& errorMsg);
    
    // Topological sort of nodes
    std::vector<NodeID> TopologicalSort(const MaterialGraph& graph);
    
    // Generate GLSL code for a single node
    std::string GenerateNodeCode(const MaterialGraph& graph, const MaterialNode& node,
                                  std::unordered_map<PinID, std::string>& pinVarNames);
    
    // Get the expression for a pin's value (with optional conversion)
    std::string GetPinValue(const MaterialGraph& graph, PinID pinId, PinType desiredType,
                            const std::unordered_map<PinID, std::string>& pinVarNames);
    
    // Type conversion helpers
    std::string ConvertType(const std::string& value, PinType from, PinType to);
    std::string GetGLSLTypeName(PinType type);
    std::string GetDefaultValue(PinType type, const PinValue& defaultVal);
};

} // namespace lucent::material

