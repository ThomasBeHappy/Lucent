#pragma once

#include "lucent/core/Core.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <cstdint>
#include <functional>

namespace lucent::material {

// Forward declarations
struct MaterialNode;
struct MaterialPin;
struct MaterialLink;
class MaterialGraph;

// Material domain: determines which output node drives compilation
enum class MaterialDomain {
    Surface,    // Uses PBROutput node
    Volume      // Uses VolumetricOutput node
};

// Unique identifiers
using NodeID = uint64_t;
using PinID = uint64_t;
using LinkID = uint64_t;

constexpr NodeID INVALID_NODE_ID = 0;
constexpr PinID INVALID_PIN_ID = 0;
constexpr LinkID INVALID_LINK_ID = 0;

// Pin data types
enum class PinType {
    Float,      // Single float
    Vec2,       // vec2
    Vec3,       // vec3 (color, position, etc.)
    Vec4,       // vec4 (color with alpha)
    Sampler2D,  // Texture sampler
};

// Get component count for a pin type
inline int GetPinTypeComponents(PinType type) {
    switch (type) {
        case PinType::Float: return 1;
        case PinType::Vec2: return 2;
        case PinType::Vec3: return 3;
        case PinType::Vec4: return 4;
        case PinType::Sampler2D: return 0; // Special case
    }
    return 0;
}

// Pin direction
enum class PinDirection {
    Input,
    Output
};

// Node types
enum class NodeType {
    // Input nodes
    UV,             // Outputs UV coordinates
    VertexColor,    // Outputs vertex color
    Time,           // Outputs time value
    
    // Constants
    ConstFloat,     // Float constant
    ConstVec2,      // Vec2 constant
    ConstVec3,      // Vec3 constant (also used for color)
    ConstVec4,      // Vec4 constant
    
    // Textures
    Texture2D,      // Sample a 2D texture
    NormalMap,      // Sample and decode normal map

    // Procedural
    Noise,          // 2D/3D noise (fbm), outputs value + color

    // Color/curves
    ColorRamp,      // Map scalar to color via gradient
    
    // Math - scalar/vector
    Add,            // A + B
    Subtract,       // A - B
    Multiply,       // A * B
    Divide,         // A / B
    Power,          // pow(A, B)
    Lerp,           // mix(A, B, T)
    Remap,          // remap(x, inMin..inMax, outMin..outMax)
    Step,           // step(edge, x)
    Smoothstep,     // smoothstep(edge0, edge1, x)
    Sin,            // sin(x)
    Cos,            // cos(x)
    Clamp,          // clamp(X, Min, Max)
    OneMinus,       // 1.0 - X
    Abs,            // abs(X)

    // Shading helpers
    Fresnel,        // Fresnel term from N.V
    
    // Vector operations
    Dot,            // dot(A, B)
    Normalize,      // normalize(V)
    Length,         // length(V)
    
    // Split/Combine
    SeparateVec3,   // Split vec3 into R, G, B
    SeparateVec4,   // Split vec4 into R, G, B, A
    CombineVec3,    // Combine R, G, B into vec3
    CombineVec4,    // Combine R, G, B, A into vec4
    
    // Output
    PBROutput,          // Final PBR material output (surface domain)
    VolumetricOutput,   // Final volumetric material output (volume domain)
    
    // Utility / Editor
    Reroute,            // Passthrough node for wire organization
    Frame,              // Comment/group frame (editor-only, no compilation)
    
    // Type Conversion
    FloatToVec3,        // Broadcast float to vec3
    Vec3ToFloat,        // Extract first component (R) from vec3
    Vec2ToVec3,         // Extend vec2 to vec3 (z=0)
    Vec3ToVec4,         // Extend vec3 to vec4 (a=1)
    Vec4ToVec3,         // Drop alpha from vec4

    // ---------------------------------------------------------------------
    // IMPORTANT: New nodes must be APPENDED ONLY.
    // NodeType is serialized as an integer in .lmat files; reordering will
    // break backwards compatibility for existing materials.
    // ---------------------------------------------------------------------

    // More Math
    Min,            // min(A, B) (component-wise for vectors)
    Max,            // max(A, B) (component-wise for vectors)
    Saturate,       // clamp(X, 0..1) (component-wise for vectors)
    Sqrt,           // sqrt(X)
    Floor,          // floor(X)
    Ceil,           // ceil(X)
    Fract,          // fract(X)
    Mod,            // mod(A, B)
    Exp,            // exp(X)
    Log,            // log(X)
    Negate,         // -X

    // More Vector
    Cross,          // cross(A, B)
    Reflect,        // reflect(I, N)
    Refract,        // refract(I, N, eta)

    // More Split/Combine
    SeparateVec2,   // Split vec2 into X, Y
    CombineVec2,    // Combine X, Y into vec2

    // More Inputs
    WorldPosition,  // Outputs world-space position
    WorldNormal,    // Outputs world-space normal
    ViewDirection,  // Outputs view direction (from fragment to camera, normalized)
};

// Get node category for UI
inline const char* GetNodeCategory(NodeType type) {
    switch (type) {
        case NodeType::UV:
        case NodeType::VertexColor:
        case NodeType::Time:
        case NodeType::WorldPosition:
        case NodeType::WorldNormal:
        case NodeType::ViewDirection:
            return "Input";
        case NodeType::ConstFloat:
        case NodeType::ConstVec2:
        case NodeType::ConstVec3:
        case NodeType::ConstVec4:
            return "Constants";
        case NodeType::Texture2D:
        case NodeType::NormalMap:
            return "Texture";
        case NodeType::Noise:
            return "Procedural";
        case NodeType::ColorRamp:
            return "Color";
        case NodeType::Add:
        case NodeType::Subtract:
        case NodeType::Multiply:
        case NodeType::Divide:
        case NodeType::Power:
        case NodeType::Lerp:
        case NodeType::Remap:
        case NodeType::Step:
        case NodeType::Smoothstep:
        case NodeType::Sin:
        case NodeType::Cos:
        case NodeType::Clamp:
        case NodeType::OneMinus:
        case NodeType::Abs:
        case NodeType::Min:
        case NodeType::Max:
        case NodeType::Saturate:
        case NodeType::Sqrt:
        case NodeType::Floor:
        case NodeType::Ceil:
        case NodeType::Fract:
        case NodeType::Mod:
        case NodeType::Exp:
        case NodeType::Log:
        case NodeType::Negate:
            return "Math";
        case NodeType::Fresnel:
            return "Shading";
        case NodeType::Dot:
        case NodeType::Normalize:
        case NodeType::Length:
        case NodeType::Cross:
        case NodeType::Reflect:
        case NodeType::Refract:
            return "Vector";
        case NodeType::SeparateVec3:
        case NodeType::SeparateVec4:
        case NodeType::SeparateVec2:
        case NodeType::CombineVec3:
        case NodeType::CombineVec4:
        case NodeType::CombineVec2:
            return "Convert";
        case NodeType::PBROutput:
        case NodeType::VolumetricOutput:
            return "Output";
        case NodeType::Reroute:
        case NodeType::Frame:
            return "Utility";
        case NodeType::FloatToVec3:
        case NodeType::Vec3ToFloat:
        case NodeType::Vec2ToVec3:
        case NodeType::Vec3ToVec4:
        case NodeType::Vec4ToVec3:
            return "Convert";
    }
    return "Other";
}

// Get human-readable node name
inline const char* GetNodeTypeName(NodeType type) {
    switch (type) {
        case NodeType::UV: return "UV";
        case NodeType::VertexColor: return "Vertex Color";
        case NodeType::Time: return "Time";
        case NodeType::WorldPosition: return "World Position";
        case NodeType::WorldNormal: return "World Normal";
        case NodeType::ViewDirection: return "View Direction";
        case NodeType::ConstFloat: return "Float";
        case NodeType::ConstVec2: return "Vector2";
        case NodeType::ConstVec3: return "Vector3 / Color";
        case NodeType::ConstVec4: return "Vector4";
        case NodeType::Texture2D: return "Texture2D";
        case NodeType::NormalMap: return "Normal Map";
        case NodeType::Noise: return "Noise";
        case NodeType::ColorRamp: return "Color Ramp";
        case NodeType::Add: return "Add";
        case NodeType::Subtract: return "Subtract";
        case NodeType::Multiply: return "Multiply";
        case NodeType::Divide: return "Divide";
        case NodeType::Power: return "Power";
        case NodeType::Lerp: return "Lerp";
        case NodeType::Remap: return "Remap";
        case NodeType::Step: return "Step";
        case NodeType::Smoothstep: return "Smoothstep";
        case NodeType::Sin: return "Sine";
        case NodeType::Cos: return "Cosine";
        case NodeType::Clamp: return "Clamp";
        case NodeType::OneMinus: return "One Minus";
        case NodeType::Abs: return "Abs";
        case NodeType::Min: return "Min";
        case NodeType::Max: return "Max";
        case NodeType::Saturate: return "Saturate";
        case NodeType::Sqrt: return "Sqrt";
        case NodeType::Floor: return "Floor";
        case NodeType::Ceil: return "Ceil";
        case NodeType::Fract: return "Fract";
        case NodeType::Mod: return "Mod";
        case NodeType::Exp: return "Exp";
        case NodeType::Log: return "Log";
        case NodeType::Negate: return "Negate";
        case NodeType::Fresnel: return "Fresnel";
        case NodeType::Dot: return "Dot Product";
        case NodeType::Normalize: return "Normalize";
        case NodeType::Length: return "Length";
        case NodeType::Cross: return "Cross Product";
        case NodeType::Reflect: return "Reflect";
        case NodeType::Refract: return "Refract";
        case NodeType::SeparateVec3: return "Separate RGB";
        case NodeType::SeparateVec4: return "Separate RGBA";
        case NodeType::SeparateVec2: return "Separate XY";
        case NodeType::CombineVec3: return "Combine RGB";
        case NodeType::CombineVec4: return "Combine RGBA";
        case NodeType::CombineVec2: return "Combine XY";
        case NodeType::PBROutput: return "PBR Output";
        case NodeType::VolumetricOutput: return "Volume Output";
        case NodeType::Reroute: return "Reroute";
        case NodeType::Frame: return "Frame";
        case NodeType::FloatToVec3: return "Float to Vec3";
        case NodeType::Vec3ToFloat: return "Vec3 to Float";
        case NodeType::Vec2ToVec3: return "Vec2 to Vec3";
        case NodeType::Vec3ToVec4: return "Vec3 to Vec4";
        case NodeType::Vec4ToVec3: return "Vec4 to Vec3";
    }
    return "Unknown";
}

// Value that can be stored in a pin/constant
using PinValue = std::variant<float, glm::vec2, glm::vec3, glm::vec4, std::string>;

// A pin on a node (input or output)
struct MaterialPin {
    PinID id = INVALID_PIN_ID;
    NodeID nodeId = INVALID_NODE_ID;
    std::string name;
    PinType type = PinType::Float;
    PinDirection direction = PinDirection::Input;
    
    // Default value for inputs (used when not connected)
    PinValue defaultValue = 0.0f;
    
    // For texture pins: texture slot index (-1 = not set)
    int textureSlot = -1;
};

// A link between two pins
struct MaterialLink {
    LinkID id = INVALID_LINK_ID;
    PinID startPinId = INVALID_PIN_ID; // Output pin
    PinID endPinId = INVALID_PIN_ID;   // Input pin
};

// A node in the material graph
struct MaterialNode {
    NodeID id = INVALID_NODE_ID;
    NodeType type = NodeType::ConstFloat;
    std::string name;
    
    // Position in the node editor (for UI)
    glm::vec2 position = glm::vec2(0.0f);
    
    // Node-specific parameters (for constants, texture paths, etc.)
    PinValue parameter;
    
    // Pins owned by this node
    std::vector<PinID> inputPins;
    std::vector<PinID> outputPins;
};

// Texture slot definition
struct TextureSlot {
    std::string path;
    bool sRGB = true;  // true for albedo, false for data textures
    int bindingIndex = -1;
};

// The material graph
class MaterialGraph {
public:
    MaterialGraph();
    ~MaterialGraph() = default;
    
    // Clear the graph
    void Clear();
    
    // Create a default graph with just an output node
    void CreateDefault();
    
    // Node management
    NodeID CreateNode(NodeType type, const glm::vec2& position = glm::vec2(0.0f));
    void DeleteNode(NodeID nodeId);
    MaterialNode* GetNode(NodeID nodeId);
    const MaterialNode* GetNode(NodeID nodeId) const;
    
    // Pin management
    MaterialPin* GetPin(PinID pinId);
    const MaterialPin* GetPin(PinID pinId) const;
    NodeID GetPinNodeId(PinID pinId) const;
    
    // Link management
    LinkID CreateLink(PinID startPinId, PinID endPinId);
    void DeleteLink(LinkID linkId);
    bool CanCreateLink(PinID startPinId, PinID endPinId) const;
    const MaterialLink* GetLink(LinkID linkId) const;
    LinkID FindLinkByEndPin(PinID endPinId) const;
    
    // Texture slots
    int AddTextureSlot(const std::string& path, bool sRGB = true);
    const std::vector<TextureSlot>& GetTextureSlots() const { return m_TextureSlots; }
    void SetTextureSlot(int index, const std::string& path, bool sRGB);
    
    // Iteration
    const std::unordered_map<NodeID, MaterialNode>& GetNodes() const { return m_Nodes; }
    const std::unordered_map<PinID, MaterialPin>& GetPins() const { return m_Pins; }
    const std::unordered_map<LinkID, MaterialLink>& GetLinks() const { return m_Links; }
    
    // Get the output node (there should be exactly one per domain)
    NodeID GetOutputNodeId() const { return m_OutputNodeId; }
    void SetOutputNodeId(NodeID nodeId) { m_OutputNodeId = nodeId; }
    
    // Volumetric output node (separate from PBR output)
    NodeID GetVolumeOutputNodeId() const { return m_VolumeOutputNodeId; }
    void SetVolumeOutputNodeId(NodeID nodeId) { m_VolumeOutputNodeId = nodeId; }
    
    // Material domain (Surface or Volume)
    MaterialDomain GetDomain() const { return m_Domain; }
    void SetDomain(MaterialDomain domain) { m_Domain = domain; }
    
    // Check if a specific output node exists
    bool HasPBROutput() const { return m_OutputNodeId != INVALID_NODE_ID; }
    bool HasVolumeOutput() const { return m_VolumeOutputNodeId != INVALID_NODE_ID; }
    
    // Get active output node based on domain
    NodeID GetActiveOutputNodeId() const {
        return m_Domain == MaterialDomain::Volume ? m_VolumeOutputNodeId : m_OutputNodeId;
    }
    
    // Compute a hash of the graph for caching
    uint64_t ComputeHash() const;
    
    // Get graph name
    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }
    
private:
    uint64_t AllocateId();
    PinID CreatePin(NodeID nodeId, const std::string& name, PinType type, PinDirection direction, PinValue defaultValue = 0.0f);
    void SetupNodePins(MaterialNode& node);
    
    uint64_t m_NextId = 1;
    
    std::unordered_map<NodeID, MaterialNode> m_Nodes;
    std::unordered_map<PinID, MaterialPin> m_Pins;
    std::unordered_map<LinkID, MaterialLink> m_Links;
    
    std::vector<TextureSlot> m_TextureSlots;
    
    NodeID m_OutputNodeId = INVALID_NODE_ID;         // PBR Output node
    NodeID m_VolumeOutputNodeId = INVALID_NODE_ID;  // Volumetric Output node
    MaterialDomain m_Domain = MaterialDomain::Surface;
    std::string m_Name = "New Material";
};

} // namespace lucent::material

