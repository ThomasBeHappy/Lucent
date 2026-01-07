#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <variant>
#include <cstdint>

namespace lucent::material {

// Material IR represents a compiled material in a format suitable for both
// raster (GLSL generation) and traced (GPU buffer evaluation) modes.

// Supported node types in the IR (subset of full material graph)
enum class IRNodeType : uint8_t {
    // Constants
    ConstFloat,
    ConstVec2,
    ConstVec3,
    ConstVec4,
    
    // Textures
    Texture2D,      // Sample texture at UV
    NormalMap,      // Sample normal map with tangent space conversion
    
    // Math
    Add,
    Subtract,
    Multiply,
    Divide,
    Lerp,           // mix(a, b, factor)
    Clamp,
    Saturate,       // clamp(0, 1)
    Pow,
    Sqrt,
    Abs,
    Min,
    Max,
    
    // Utility
    SeparateRGB,    // Split vec3/vec4 into components
    CombineRGB,     // Combine components into vec3/vec4
    DotProduct,
    Normalize,
    
    // Procedural
    Noise,          // Perlin/FBM noise
    Fresnel,        // Fresnel term based on view angle
    
    // Color
    ColorRamp,      // Gradient lookup
    
    // Inputs
    UV,             // Texture coordinates
    WorldPos,       // World space position
    WorldNormal,    // World space normal
    ViewDir,        // View direction
    
    // Output
    OutputPBR       // Final PBR material output
};

// IR instruction operand types
using IRValue = std::variant<
    float,                  // Single float
    glm::vec2,              // 2D vector
    glm::vec3,              // 3D vector
    glm::vec4,              // 4D vector
    uint32_t                // Reference to another instruction's output
>;

// Texture reference
struct IRTextureRef {
    std::string path;
    uint32_t bindingSlot = 0;
    bool isSRGB = true;
};

// Color ramp stop
struct IRColorStop {
    float position;
    glm::vec3 color;
};

// Single IR instruction
struct IRInstruction {
    uint32_t id = 0;                    // Instruction ID (output register)
    IRNodeType type = IRNodeType::ConstFloat;
    
    // Operands (up to 4 inputs)
    IRValue operands[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    
    // Additional data for specific node types
    IRTextureRef texture;               // For Texture2D, NormalMap
    std::vector<IRColorStop> colorRamp; // For ColorRamp
    float noiseScale = 1.0f;            // For Noise
    uint32_t noiseOctaves = 4;          // For Noise
};

// PBR material output channels
struct PBROutput {
    uint32_t baseColorInstr = 0;    // vec3
    uint32_t metallicInstr = 0;     // float
    uint32_t roughnessInstr = 0;    // float
    uint32_t normalInstr = 0;       // vec3 (optional, 0 = use geometry normal)
    uint32_t emissiveInstr = 0;     // vec3
    uint32_t alphaInstr = 0;        // float (optional)
};

// Complete compiled material IR
struct MaterialIR {
    std::string name;
    std::vector<IRInstruction> instructions;
    PBROutput output;
    
    // Texture bindings needed
    std::vector<IRTextureRef> textures;
    
    // Check if material is valid
    bool IsValid() const { return !instructions.empty(); }
    
    // Get simplified GPU-friendly representation
    // Returns packed material data suitable for traced mode
    struct GPUMaterialData {
        glm::vec4 baseColor;    // RGB + alpha
        glm::vec4 emissive;     // RGB + intensity
        float metallic;
        float roughness;
        float ior;
        uint32_t flags;         // Texture flags, etc.
    };
    
    // Evaluate material to simple constants (for traced mode fallback)
    GPUMaterialData EvaluateConstant() const;
};

// Compiler that converts MaterialGraph to MaterialIR
class MaterialIRCompiler {
public:
    // Compile a material graph to IR
    // Returns true on success, false if unsupported nodes are present
    static bool Compile(const class MaterialGraph& graph, MaterialIR& outIR, std::string& errorMsg);
    
    // Check if a material graph is fully supported in traced mode
    static bool IsTracedCompatible(const class MaterialGraph& graph);
};

} // namespace lucent::material


