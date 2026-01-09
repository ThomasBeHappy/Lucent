#include "lucent/material/MaterialCompiler.h"
#include "lucent/core/Log.h"
#include <shaderc/shaderc.hpp>
#include <sstream>
#include <queue>
#include <set>
#include <mutex>

namespace lucent::material {

// Noise node parameter (optional, V2): "NOISE2:<type>;<scale>,<detail>,<roughness>,<distortion>"
// - type: 0=FBM, 1=Value, 2=Ridged, 3=Turbulence
static bool ParseNoise2Param(const std::string& s, int& outType, glm::vec4& outParams) {
    if (s.rfind("NOISE2:", 0) != 0) return false;
    int t = 0;
    float x = 5.0f, y = 4.0f, z = 0.5f, w = 0.0f;
    if (sscanf_s(s.c_str(), "NOISE2:%d;%f,%f,%f,%f", &t, &x, &y, &z, &w) == 5) {
        outType = t;
        outParams = glm::vec4(x, y, z, w);
        return true;
    }
    return false;
}

// Standard vertex shader source (same interface as mesh.vert)
static const char* s_StandardVertexShaderGLSL = R"(
#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec3 outTangent;
layout(location = 4) out vec3 outBitangent;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;
    vec4 baseColor;
    vec4 materialParams;
    vec4 emissive;
    vec4 cameraPos;
} pc;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    outWorldPos = worldPos.xyz;
    
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    outNormal = normalize(normalMatrix * inNormal);
    outTangent = normalize(normalMatrix * inTangent.xyz);
    outBitangent = cross(outNormal, outTangent) * inTangent.w;
    
    outUV = inUV;
    
    gl_Position = pc.viewProj * worldPos;
}
)";

static std::vector<uint32_t> s_StandardVertexShaderSPIRV;
static std::once_flag s_StandardVertexShaderOnce;

const std::vector<uint32_t>& MaterialCompiler::GetStandardVertexShaderSPIRV() {
    std::call_once(s_StandardVertexShaderOnce, []() {
        // Compile the standard vertex shader
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        // We target Vulkan 1.2 to keep SPIR-V compatible with fallback GPUs (SPIR-V 1.5).
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        
        auto result = compiler.CompileGlslToSpv(
            s_StandardVertexShaderGLSL,
            shaderc_vertex_shader,
            "standard_material.vert",
            options
        );
        
        if (result.GetCompilationStatus() == shaderc_compilation_status_success) {
            s_StandardVertexShaderSPIRV.assign(result.begin(), result.end());
        } else {
            LUCENT_CORE_ERROR("Failed to compile standard vertex shader: {}", result.GetErrorMessage());
        }
    });
    return s_StandardVertexShaderSPIRV;
}

CompileResult MaterialCompiler::Compile(const MaterialGraph& graph) {
    CompileResult result;
    result.graphHash = graph.ComputeHash();
    result.domain = graph.GetDomain();
    
    // Generate GLSL based on domain
    result.fragmentShaderGLSL = GenerateFragmentGLSL(graph);
    
    if (result.fragmentShaderGLSL.empty()) {
        result.success = false;
        result.errorMessage = "Failed to generate GLSL";
        return result;
    }
    
    // Compile to SPIR-V
    if (!CompileGLSLToSPIRV(result.fragmentShaderGLSL, result.fragmentShaderSPIRV, result.errorMessage)) {
        result.success = false;
        return result;
    }
    
    result.success = true;
    return result;
}

std::string MaterialCompiler::GenerateFragmentGLSL(const MaterialGraph& graph) {
    // Dispatch based on material domain
    if (graph.GetDomain() == MaterialDomain::Volume) {
        return GenerateVolumeFragmentGLSL(graph);
    }
    return GenerateSurfaceFragmentGLSL(graph);
}

std::string MaterialCompiler::GenerateSurfaceFragmentGLSL(const MaterialGraph& graph) {
    std::ostringstream ss;
    
    // Header
    ss << "#version 450\n\n";
    
    // Inputs from vertex shader
    ss << "layout(location = 0) in vec3 inWorldPos;\n";
    ss << "layout(location = 1) in vec3 inNormal;\n";
    ss << "layout(location = 2) in vec2 inUV;\n";
    ss << "layout(location = 3) in vec3 inTangent;\n";
    ss << "layout(location = 4) in vec3 inBitangent;\n\n";
    
    // Output
    ss << "layout(location = 0) out vec4 outColor;\n\n";
    
    // Push constants
    ss << "layout(push_constant) uniform PushConstants {\n";
    ss << "    mat4 model;\n";
    ss << "    mat4 viewProj;\n";
    ss << "    vec4 baseColor;\n";
    ss << "    vec4 materialParams;\n";
    ss << "    vec4 emissive;\n";
    ss << "    vec4 cameraPos;\n";
    ss << "} pc;\n\n";
    
    // Texture samplers
    const auto& textureSlots = graph.GetTextureSlots();
    // NOTE: Texture2D nodes sample from `textures[slot]`. If the graph has Texture2D nodes but no texture slots
    // were registered, shader compilation would fail. We defensively declare at least one sampler to avoid
    // hard shader errors, but the material will still render incorrectly until a slot is assigned.
    bool hasTextureNodes = false;
    for (const auto& [id, node] : graph.GetNodes()) {
        if (node.type == NodeType::Texture2D) { hasTextureNodes = true; break; }
        if (node.type == NodeType::NormalMap) { hasTextureNodes = true; break; }
    }
    if (!textureSlots.empty()) {
        ss << "layout(set = 0, binding = 0) uniform sampler2D textures[" << textureSlots.size() << "];\n\n";
    } else if (hasTextureNodes) {
        ss << "layout(set = 0, binding = 0) uniform sampler2D textures[1];\n\n";
    }

    // Procedural helpers (inject only if needed)
    bool needsNoise = false;
    bool needsColorRamp = false;
    for (const auto& [id, node] : graph.GetNodes()) {
        if (node.type == NodeType::Noise) {
            needsNoise = true;
        }
        if (node.type == NodeType::ColorRamp) {
            needsColorRamp = true;
        }
    }
    if (needsNoise) {
        ss << R"(
// -----------------------------------------------------------------------------
// Noise helpers (value noise + fbm variants)
// -----------------------------------------------------------------------------
float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash12(vec2 p) {
    vec3 p3  = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash12(i + vec2(0.0, 0.0));
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float valueNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    float n000 = hash13(i + vec3(0,0,0));
    float n100 = hash13(i + vec3(1,0,0));
    float n010 = hash13(i + vec3(0,1,0));
    float n110 = hash13(i + vec3(1,1,0));
    float n001 = hash13(i + vec3(0,0,1));
    float n101 = hash13(i + vec3(1,0,1));
    float n011 = hash13(i + vec3(0,1,1));
    float n111 = hash13(i + vec3(1,1,1));
    vec3 u = f * f * (3.0 - 2.0 * f);
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

float fbm3(vec3 p, float octaves, float roughness) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    int iters = int(clamp(octaves, 1.0, 12.0));
    for (int i = 0; i < iters; ++i) {
        sum += amp * valueNoise3(p * freq);
        freq *= 2.0;
        amp *= clamp(roughness, 0.0, 1.0);
    }
    return sum;
}

float ridgedFbm3(vec3 p, float octaves, float roughness) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    int iters = int(clamp(octaves, 1.0, 12.0));
    for (int i = 0; i < iters; ++i) {
        float n = valueNoise3(p * freq);
        // Make ridges: map [0..1] -> [-1..1], abs, then invert.
        float r = 1.0 - abs(n * 2.0 - 1.0);
        sum += amp * r;
        freq *= 2.0;
        amp *= clamp(roughness, 0.0, 1.0);
    }
    return sum;
}

float turbulence3(vec3 p, float octaves, float roughness) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    int iters = int(clamp(octaves, 1.0, 12.0));
    for (int i = 0; i < iters; ++i) {
        float n = valueNoise3(p * freq);
        // Absolute noise around 0: map [0..1] -> [-1..1] then abs.
        sum += amp * abs(n * 2.0 - 1.0);
        freq *= 2.0;
        amp *= clamp(roughness, 0.0, 1.0);
    }
    return sum;
}

)";
    }

    if (needsColorRamp) {
        ss << R"(
// -----------------------------------------------------------------------------
// Color ramp helpers (piecewise linear)
// -----------------------------------------------------------------------------
vec4 ramp_eval(float t, vec4 c0, float t0, vec4 c1, float t1) {
    float u = clamp((t - t0) / max(t1 - t0, 1e-6), 0.0, 1.0);
    return mix(c0, c1, u);
}
)";
    }
    
    // PBR lighting functions
    ss << R"(
const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom + 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

)";
    
    // Main function
    ss << "void main() {\n";
    
    // Topological sort of nodes
    std::vector<NodeID> sortedNodes = TopologicalSort(graph);
    
    // Map from pin ID to variable name
    std::unordered_map<PinID, std::string> pinVarNames;
    
    // Generate code for each node
    for (NodeID nodeId : sortedNodes) {
        const MaterialNode* node = graph.GetNode(nodeId);
        if (!node) continue;
        
        ss << GenerateNodeCode(graph, *node, pinVarNames);
    }
    
    // Get output values from PBR output node
    const MaterialNode* outputNode = graph.GetNode(graph.GetOutputNodeId());
    if (!outputNode || outputNode->type != NodeType::PBROutput) {
        LUCENT_CORE_ERROR("Material graph has no PBR output node");
        return "";
    }
    
    // Get pin values for PBR parameters
    std::string baseColor = GetPinValue(graph, outputNode->inputPins[0], PinType::Vec3, pinVarNames);
    std::string metallic = GetPinValue(graph, outputNode->inputPins[1], PinType::Float, pinVarNames);
    std::string roughness = GetPinValue(graph, outputNode->inputPins[2], PinType::Float, pinVarNames);
    std::string normal = GetPinValue(graph, outputNode->inputPins[3], PinType::Vec3, pinVarNames);
    std::string emissiveVal = GetPinValue(graph, outputNode->inputPins[4], PinType::Vec3, pinVarNames);
    std::string alpha = GetPinValue(graph, outputNode->inputPins[5], PinType::Float, pinVarNames);
    
    // PBR shading
    ss << "\n    // PBR Shading\n";
    ss << "    vec3 albedo = " << baseColor << ";\n";
    ss << "    float metal = " << metallic << ";\n";
    ss << "    float rough = max(" << roughness << ", 0.04);\n";
    ss << "    vec3 N = normalize(" << normal << ");\n";
    ss << "    vec3 emissiveColor = " << emissiveVal << ";\n";
    ss << "    float alphaVal = " << alpha << ";\n\n";
    
    ss << "    vec3 V = normalize(pc.cameraPos.xyz - inWorldPos);\n";
    ss << "    vec3 L = normalize(vec3(1.0, 1.0, 0.5));\n";
    ss << "    vec3 H = normalize(V + L);\n\n";
    
    ss << "    vec3 F0 = mix(vec3(0.04), albedo, metal);\n";
    ss << "    float NDF = distributionGGX(N, H, rough);\n";
    ss << "    float G = geometrySmith(N, V, L, rough);\n";
    ss << "    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);\n\n";
    
    ss << "    vec3 numerator = NDF * G * F;\n";
    ss << "    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;\n";
    ss << "    vec3 specular = numerator / denominator;\n\n";
    
    ss << "    vec3 kD = (1.0 - F) * (1.0 - metal);\n";
    ss << "    float NdotL = max(dot(N, L), 0.0);\n";
    ss << "    vec3 Lo = (kD * albedo / PI + specular) * vec3(2.5) * NdotL;\n\n";
    
    ss << "    vec3 ambient = vec3(0.1) * albedo;\n";
    ss << "    vec3 color = ambient + Lo + emissiveColor;\n\n";
    
    // Tonemap and gamma
    ss << "    color = color / (color + vec3(1.0));\n";
    ss << "    color = pow(color, vec3(1.0 / 2.2));\n\n";
    
    ss << "    outColor = vec4(color, alphaVal);\n";
    ss << "}\n";
    
    return ss.str();
}

std::string MaterialCompiler::GenerateVolumeFragmentGLSL(const MaterialGraph& graph) {
    std::ostringstream ss;
    
    // Header
    ss << "#version 450\n\n";
    
    // Inputs from vertex shader
    ss << "layout(location = 0) in vec3 inWorldPos;\n";
    ss << "layout(location = 1) in vec3 inNormal;\n";
    ss << "layout(location = 2) in vec2 inUV;\n";
    ss << "layout(location = 3) in vec3 inTangent;\n";
    ss << "layout(location = 4) in vec3 inBitangent;\n\n";
    
    // Output (premultiplied RGBA)
    ss << "layout(location = 0) out vec4 outColor;\n\n";
    
    // Push constants
    ss << "layout(push_constant) uniform PushConstants {\n";
    ss << "    mat4 model;\n";
    ss << "    mat4 viewProj;\n";
    ss << "    vec4 baseColor;\n";
    ss << "    vec4 materialParams;\n";
    ss << "    vec4 emissive;\n";
    ss << "    vec4 cameraPos;\n";
    ss << "} pc;\n\n";
    
    // Texture samplers (same as surface for node reuse)
    const auto& textureSlots = graph.GetTextureSlots();
    bool hasTextureNodes = false;
    for (const auto& [id, node] : graph.GetNodes()) {
        if (node.type == NodeType::Texture2D || node.type == NodeType::NormalMap) {
            hasTextureNodes = true;
            break;
        }
    }
    if (!textureSlots.empty()) {
        ss << "layout(set = 0, binding = 0) uniform sampler2D textures[" << textureSlots.size() << "];\n\n";
    } else if (hasTextureNodes) {
        ss << "layout(set = 0, binding = 0) uniform sampler2D textures[1];\n\n";
    }
    
    // Noise helpers (inject if needed)
    bool needsNoise = false;
    for (const auto& [id, node] : graph.GetNodes()) {
        if (node.type == NodeType::Noise) {
            needsNoise = true;
            break;
        }
    }
    if (needsNoise) {
        ss << R"(
// Noise helpers for volumes
float hash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    float n000 = hash13(i + vec3(0,0,0));
    float n100 = hash13(i + vec3(1,0,0));
    float n010 = hash13(i + vec3(0,1,0));
    float n110 = hash13(i + vec3(1,1,0));
    float n001 = hash13(i + vec3(0,0,1));
    float n101 = hash13(i + vec3(1,0,1));
    float n011 = hash13(i + vec3(0,1,1));
    float n111 = hash13(i + vec3(1,1,1));
    vec3 u = f * f * (3.0 - 2.0 * f);
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

float fbm3(vec3 p, float octaves, float roughness) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    int iters = int(clamp(octaves, 1.0, 12.0));
    for (int i = 0; i < iters; ++i) {
        sum += amp * valueNoise3(p * freq);
        freq *= 2.0;
        amp *= clamp(roughness, 0.0, 1.0);
    }
    return sum;
}
)";
    }
    
    // Ray-box intersection helper
    ss << R"(
// Ray-box intersection for unit cube [-0.5, 0.5]^3
bool rayBoxIntersect(vec3 ro, vec3 rd, out float tNear, out float tFar) {
    vec3 invDir = 1.0 / rd;
    vec3 t0 = (-0.5 - ro) * invDir;
    vec3 t1 = (0.5 - ro) * invDir;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    tNear = max(max(tMin.x, tMin.y), tMin.z);
    tFar = min(min(tMax.x, tMax.y), tMax.z);
    return tNear <= tFar && tFar >= 0.0;
}

)";
    
    // Main function
    ss << "void main() {\n";
    
    // Compute camera ray in world space
    ss << "    vec3 camPos = pc.cameraPos.xyz;\n";
    ss << "    vec3 rayDir = normalize(inWorldPos - camPos);\n\n";
    
    // Transform to local space
    ss << "    mat4 invModel = inverse(pc.model);\n";
    ss << "    vec3 localCamPos = (invModel * vec4(camPos, 1.0)).xyz;\n";
    ss << "    vec3 localRayDir = normalize(mat3(invModel) * rayDir);\n\n";
    
    // Ray-box intersection
    ss << "    float tNear, tFar;\n";
    ss << "    if (!rayBoxIntersect(localCamPos, localRayDir, tNear, tFar)) {\n";
    ss << "        discard;\n";
    ss << "    }\n";
    ss << "    tNear = max(tNear, 0.0);\n\n";
    
    // Topological sort nodes
    std::vector<NodeID> sortedNodes = TopologicalSort(graph);
    std::unordered_map<PinID, std::string> pinVarNames;
    
    // Generate code for non-output nodes
    for (NodeID nodeId : sortedNodes) {
        const MaterialNode* node = graph.GetNode(nodeId);
        if (!node || node->type == NodeType::VolumetricOutput) continue;
        ss << GenerateNodeCode(graph, *node, pinVarNames);
    }
    
    // Get output values from Volume output node
    const MaterialNode* outputNode = graph.GetNode(graph.GetVolumeOutputNodeId());
    if (!outputNode || outputNode->type != NodeType::VolumetricOutput) {
        LUCENT_CORE_ERROR("Material graph has no Volumetric output node");
        return "";
    }
    
    // Get pin values for volume parameters
    std::string scatterColor = GetPinValue(graph, outputNode->inputPins[0], PinType::Vec3, pinVarNames);
    std::string density = GetPinValue(graph, outputNode->inputPins[1], PinType::Float, pinVarNames);
    std::string anisotropy = GetPinValue(graph, outputNode->inputPins[2], PinType::Float, pinVarNames);
    std::string absorption = GetPinValue(graph, outputNode->inputPins[3], PinType::Vec3, pinVarNames);
    std::string emission = GetPinValue(graph, outputNode->inputPins[4], PinType::Vec3, pinVarNames);
    std::string emissionStrength = GetPinValue(graph, outputNode->inputPins[5], PinType::Float, pinVarNames);
    
    // Raymarch parameters
    ss << "\n    // Volume parameters\n";
    ss << "    vec3 volColor = " << scatterColor << ";\n";
    ss << "    float volDensity = " << density << ";\n";
    ss << "    float volAnisotropy = clamp(" << anisotropy << ", -0.99, 0.99);\n";
    ss << "    vec3 volAbsorption = " << absorption << ";\n";
    ss << "    vec3 volEmission = " << emission << " * " << emissionStrength << ";\n\n";
    
    // Raymarching
    ss << R"(
    // Raymarch through volume
    const int MAX_STEPS = 64;
    float stepSize = (tFar - tNear) / float(MAX_STEPS);
    
    vec3 accumColor = vec3(0.0);
    float accumTransmittance = 1.0;
    
    // Simple directional light
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    vec3 lightColor = vec3(2.5);
    
    for (int i = 0; i < MAX_STEPS; ++i) {
        float t = tNear + (float(i) + 0.5) * stepSize;
        vec3 samplePos = localCamPos + localRayDir * t;
        
        // Skip if outside volume
        if (any(lessThan(samplePos, vec3(-0.5))) || any(greaterThan(samplePos, vec3(0.5)))) {
            continue;
        }
        
        // Sample density (use UV-based position for node evaluation)
        float sampleDensity = volDensity;
        
        // Extinction coefficient (absorption + scattering)
        vec3 sigma_t = volAbsorption + volColor * sampleDensity;
        
        // Transmittance through this step (Beer-Lambert)
        vec3 stepTransmittance = exp(-sigma_t * stepSize);
        float avgTransmittance = (stepTransmittance.r + stepTransmittance.g + stepTransmittance.b) / 3.0;
        
        // Henyey-Greenstein phase function
        float cosTheta = dot(-localRayDir, lightDir);
        float g = volAnisotropy;
        float g2 = g * g;
        float phase = (1.0 - g2) / (4.0 * 3.14159 * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
        
        // In-scattering (simplified single scatter)
        vec3 scattering = volColor * sampleDensity * lightColor * phase;
        
        // Add emission
        scattering += volEmission;
        
        // Integrate (premultiplied alpha)
        vec3 S = scattering * stepSize;
        accumColor += accumTransmittance * S;
        accumTransmittance *= avgTransmittance;
        
        // Early termination if fully opaque
        if (accumTransmittance < 0.01) break;
    }
    
    // Final alpha from transmittance
    float alpha = 1.0 - accumTransmittance;
    
    // Output premultiplied RGBA
    outColor = vec4(accumColor, alpha);
)";
    
    ss << "}\n";
    
    return ss.str();
}

bool MaterialCompiler::CompileGLSLToSPIRV(const std::string& glsl, std::vector<uint32_t>& spirv, std::string& errorMsg) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    // We target Vulkan 1.2 to keep SPIR-V compatible with fallback GPUs (SPIR-V 1.5).
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    
    auto result = compiler.CompileGlslToSpv(
        glsl,
        shaderc_fragment_shader,
        "material.frag",
        options
    );
    
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        errorMsg = result.GetErrorMessage();
        LUCENT_CORE_ERROR("Material shader compilation failed: {}", errorMsg);
        return false;
    }
    
    spirv.assign(result.begin(), result.end());
    return true;
}

std::vector<NodeID> MaterialCompiler::TopologicalSort(const MaterialGraph& graph) {
    std::vector<NodeID> result;
    std::set<NodeID> visited;
    std::set<NodeID> visiting;
    
    std::function<bool(NodeID)> visit = [&](NodeID nodeId) -> bool {
        if (visited.count(nodeId)) return true;
        if (visiting.count(nodeId)) {
            LUCENT_CORE_ERROR("Cycle detected in material graph");
            return false;
        }
        
        visiting.insert(nodeId);
        
        const MaterialNode* node = graph.GetNode(nodeId);
        if (!node) return true;
        
        // Visit all nodes connected to our inputs
        for (PinID inputPinId : node->inputPins) {
            LinkID linkId = graph.FindLinkByEndPin(inputPinId);
            if (linkId != INVALID_LINK_ID) {
                const MaterialLink* link = graph.GetLink(linkId);
                if (link) {
                    const MaterialPin* startPin = graph.GetPin(link->startPinId);
                    if (startPin) {
                        if (!visit(startPin->nodeId)) return false;
                    }
                }
            }
        }
        
        visiting.erase(nodeId);
        visited.insert(nodeId);
        result.push_back(nodeId);
        
        return true;
    };
    
    // Start from active output node (PBR or Volume based on domain)
    visit(graph.GetActiveOutputNodeId());
    
    return result;
}

std::string MaterialCompiler::GenerateNodeCode(const MaterialGraph& graph, const MaterialNode& node,
                                                std::unordered_map<PinID, std::string>& pinVarNames) {
    std::ostringstream ss;
    std::string varPrefix = "n" + std::to_string(node.id) + "_";
    
    switch (node.type) {
        case NodeType::UV:
            pinVarNames[node.outputPins[0]] = "inUV";
            break;
            
        case NodeType::VertexColor:
            pinVarNames[node.outputPins[0]] = "vec4(1.0)"; // Placeholder
            break;
            
        case NodeType::Time:
            pinVarNames[node.outputPins[0]] = "0.0"; // Placeholder
            break;
            
        case NodeType::ConstFloat: {
            float val = std::holds_alternative<float>(node.parameter) ? std::get<float>(node.parameter) : 0.0f;
            pinVarNames[node.outputPins[0]] = std::to_string(val);
            break;
        }
        
        case NodeType::ConstVec2: {
            glm::vec2 val = std::holds_alternative<glm::vec2>(node.parameter) ? std::get<glm::vec2>(node.parameter) : glm::vec2(0.0f);
            std::ostringstream v;
            v << "vec2(" << val.x << ", " << val.y << ")";
            pinVarNames[node.outputPins[0]] = v.str();
            break;
        }
        
        case NodeType::ConstVec3: {
            glm::vec3 val = std::holds_alternative<glm::vec3>(node.parameter) ? std::get<glm::vec3>(node.parameter) : glm::vec3(0.0f);
            std::ostringstream v;
            v << "vec3(" << val.x << ", " << val.y << ", " << val.z << ")";
            pinVarNames[node.outputPins[0]] = v.str();
            break;
        }
        
        case NodeType::ConstVec4: {
            glm::vec4 val = std::holds_alternative<glm::vec4>(node.parameter) ? std::get<glm::vec4>(node.parameter) : glm::vec4(0.0f);
            std::ostringstream v;
            v << "vec4(" << val.x << ", " << val.y << ", " << val.z << ", " << val.w << ")";
            pinVarNames[node.outputPins[0]] = v.str();
            break;
        }
        
        case NodeType::Texture2D: {
            // If UV is left unconnected, default to mesh UVs (otherwise you'll sample a single texel).
            std::string uvVal;
            if (graph.FindLinkByEndPin(node.inputPins[0]) != INVALID_LINK_ID) {
                uvVal = GetPinValue(graph, node.inputPins[0], PinType::Vec2, pinVarNames);
            } else {
                uvVal = "inUV";
            }
            // Get texture slot index from parameter
            int texSlot = 0; // Default to first texture
            if (std::holds_alternative<std::string>(node.parameter)) {
                // Find texture slot by path
                const auto& slots = graph.GetTextureSlots();
                const std::string& path = std::get<std::string>(node.parameter);
                for (size_t i = 0; i < slots.size(); ++i) {
                    if (slots[i].path == path) {
                        texSlot = static_cast<int>(i);
                        break;
                    }
                }
            }
            
            std::string texVar = varPrefix + "tex";
            ss << "    vec4 " << texVar << " = texture(textures[" << texSlot << "], " << uvVal << ");\n";
            
            // RGB and individual channels
            pinVarNames[node.outputPins[0]] = texVar + ".rgb";
            pinVarNames[node.outputPins[1]] = texVar + ".r";
            pinVarNames[node.outputPins[2]] = texVar + ".g";
            pinVarNames[node.outputPins[3]] = texVar + ".b";
            pinVarNames[node.outputPins[4]] = texVar + ".a";
            break;
        }
        
        case NodeType::NormalMap: {
            // If UV is left unconnected, default to mesh UVs.
            std::string uvVal;
            if (graph.FindLinkByEndPin(node.inputPins[0]) != INVALID_LINK_ID) {
                uvVal = GetPinValue(graph, node.inputPins[0], PinType::Vec2, pinVarNames);
            } else {
                uvVal = "inUV";
            }
            std::string strength = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            
            // Sample normal map and decode
            std::string nVar = varPrefix + "normal";
            ss << "    vec3 " << nVar << " = inNormal; // Normal map placeholder\n";
            pinVarNames[node.outputPins[0]] = nVar;
            break;
        }

        case NodeType::Noise: {
            // Parameter (optional) controls the *defaults* when pins are unconnected.
            // Also supports a noise "type" selection via NOISE2 string.
            int noiseType = 0; // 0=FBM, 1=Value, 2=Ridged, 3=Turbulence
            glm::vec4 p = glm::vec4(5.0f, 4.0f, 0.5f, 0.0f); // scale, detail, roughness, distortion
            if (std::holds_alternative<glm::vec4>(node.parameter)) {
                p = std::get<glm::vec4>(node.parameter);
            } else if (std::holds_alternative<std::string>(node.parameter)) {
                (void)ParseNoise2Param(std::get<std::string>(node.parameter), noiseType, p);
            }

            // Inputs
            // If the coordinate is left unconnected, default to UVs so the node "just works".
            // (Default vec3(0) would sample the same point and produce a flat color.)
            auto isConnected = [&](size_t inputIndex) -> bool {
                return graph.FindLinkByEndPin(node.inputPins[inputIndex]) != INVALID_LINK_ID;
            };

            std::string vecIn = isConnected(0)
                ? GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames)
                : "vec3(inUV, 0.0)";
            std::string scale = isConnected(1)
                ? GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames)
                : std::to_string(p.x);
            std::string detail = isConnected(2)
                ? GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames)
                : std::to_string(p.y);
            std::string rough = isConnected(3)
                ? GetPinValue(graph, node.inputPins[3], PinType::Float, pinVarNames)
                : std::to_string(p.z);
            std::string distort = isConnected(4)
                ? GetPinValue(graph, node.inputPins[4], PinType::Float, pinVarNames)
                : std::to_string(p.w);

            std::string pVar = varPrefix + "p";
            std::string nVar = varPrefix + "n";
            ss << "    vec3 " << pVar << " = (" << vecIn << ") * " << scale << ";\n";
            // Distortion: offset by another noise lookup
            ss << "    if (" << distort << " > 0.0) { " << pVar << " += " << distort << " * vec3(valueNoise3(" << pVar << " + vec3(31.7)), valueNoise3(" << pVar << " + vec3(17.3)), valueNoise3(" << pVar << " + vec3(9.2))); }\n";
            switch (noiseType) {
                default:
                case 0: // FBM
                    ss << "    float " << nVar << " = fbm3(" << pVar << ", " << detail << ", " << rough << ");\n";
                    break;
                case 1: // Value
                    ss << "    float " << nVar << " = valueNoise3(" << pVar << ");\n";
                    break;
                case 2: // Ridged
                    ss << "    float " << nVar << " = ridgedFbm3(" << pVar << ", " << detail << ", " << rough << ");\n";
                    break;
                case 3: // Turbulence
                    ss << "    float " << nVar << " = turbulence3(" << pVar << ", " << detail << ", " << rough << ");\n";
                    break;
            }

            pinVarNames[node.outputPins[0]] = nVar;
            pinVarNames[node.outputPins[1]] = "vec3(" + nVar + ")";
            break;
        }

        case NodeType::ColorRamp: {
            std::string f = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);

            // Parse stops from node.parameter string and emit constants.
            // Format: "RAMP:t,r,g,b;..." (alpha not supported by ImGradient)
            std::vector<std::pair<float, glm::vec4>> stops;
            if (std::holds_alternative<std::string>(node.parameter)) {
                const std::string blob = std::get<std::string>(node.parameter);
                const std::string prefix = "RAMP:";
                size_t start = (blob.rfind(prefix, 0) == 0) ? prefix.size() : 0;
                while (start < blob.size()) {
                    size_t end = blob.find(';', start);
                    std::string token = blob.substr(start, end == std::string::npos ? std::string::npos : (end - start));
                    if (!token.empty()) {
                        float t = 0, r = 1, g = 1, b = 1;
                        if (sscanf_s(token.c_str(), "%f,%f,%f,%f", &t, &r, &g, &b) == 4) {
                            stops.emplace_back(t, glm::vec4(r, g, b, 1.0f));
                        }
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
            if (stops.size() < 2) {
                stops.clear();
                stops.emplace_back(0.0f, glm::vec4(0, 0, 0, 1));
                stops.emplace_back(1.0f, glm::vec4(1, 1, 1, 1));
            }
            std::sort(stops.begin(), stops.end(), [](auto& a, auto& b) { return a.first < b.first; });

            // Clamp t to ends
            std::string var = varPrefix + "ramp";
            ss << "    float " << varPrefix << "t = clamp(" << f << ", " << stops.front().first << ", " << stops.back().first << ");\n";

            // Start with first color
            ss << "    vec4 " << var << " = vec4(" << stops.front().second.r << ", " << stops.front().second.g << ", " << stops.front().second.b << ", " << stops.front().second.a << ");\n";

            // Build piecewise interpolation
            for (size_t i = 0; i + 1 < stops.size(); ++i) {
                const auto& a = stops[i];
                const auto& b = stops[i + 1];
                ss << "    if (" << varPrefix << "t >= " << a.first << " && " << varPrefix << "t <= " << b.first << ") {\n";
                ss << "        " << var << " = ramp_eval(" << varPrefix << "t, vec4(" << a.second.r << ", " << a.second.g << ", " << a.second.b << ", " << a.second.a
                   << "), " << a.first << ", vec4(" << b.second.r << ", " << b.second.g << ", " << b.second.b << ", " << b.second.a << "), " << b.first << ");\n";
                ss << "    }\n";
            }

            pinVarNames[node.outputPins[0]] = var + ".rgb";
            pinVarNames[node.outputPins[1]] = "1.0";
            break;
        }
        
        case NodeType::Add: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "add";
            ss << "    vec3 " << var << " = " << a << " + " << b << ";\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Subtract: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "sub";
            ss << "    vec3 " << var << " = " << a << " - " << b << ";\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Multiply: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "mul";
            ss << "    vec3 " << var << " = " << a << " * " << b << ";\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Divide: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "div";
            ss << "    vec3 " << var << " = " << a << " / max(" << b << ", vec3(0.0001));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Lerp: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string t = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string var = varPrefix + "lerp";
            ss << "    vec3 " << var << " = mix(" << a << ", " << b << ", " << t << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Remap: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string inMin = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string inMax = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string outMin = GetPinValue(graph, node.inputPins[3], PinType::Float, pinVarNames);
            std::string outMax = GetPinValue(graph, node.inputPins[4], PinType::Float, pinVarNames);
            std::string var = varPrefix + "remap";
            ss << "    float " << var << " = mix(" << outMin << ", " << outMax << ", clamp((" << v << " - " << inMin << ") / max((" << inMax << " - " << inMin << "), 0.0001), 0.0, 1.0));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Step: {
            std::string edge = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string x = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "step";
            ss << "    float " << var << " = step(" << edge << ", " << x << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Smoothstep: {
            std::string e0 = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string e1 = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string x = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string var = varPrefix + "smoothstep";
            ss << "    float " << var << " = smoothstep(" << e0 << ", " << e1 << ", " << x << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Sin: {
            std::string x = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "sin";
            ss << "    float " << var << " = sin(" << x << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Cos: {
            std::string x = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "cos";
            ss << "    float " << var << " = cos(" << x << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Clamp: {
            std::string val = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string minVal = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string maxVal = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string var = varPrefix + "clamp";
            ss << "    float " << var << " = clamp(" << val << ", " << minVal << ", " << maxVal << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::OneMinus: {
            std::string val = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "oneminus";
            ss << "    float " << var << " = 1.0 - " << val << ";\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::SeparateVec3: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            pinVarNames[node.outputPins[0]] = "(" + vec + ").x";
            pinVarNames[node.outputPins[1]] = "(" + vec + ").y";
            pinVarNames[node.outputPins[2]] = "(" + vec + ").z";
            break;
        }
        
        case NodeType::CombineVec3: {
            std::string r = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string g = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string var = varPrefix + "combine";
            ss << "    vec3 " << var << " = vec3(" << r << ", " << g << ", " << b << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Fresnel: {
            std::string power = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "fresnel";
            ss << "    float " << var << " = pow(1.0 - clamp(dot(normalize(inNormal), normalize(pc.cameraPos.xyz - inWorldPos)), 0.0, 1.0), " << power << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::PBROutput:
            // Output node doesn't generate code, values are read directly
            break;
            
        case NodeType::VolumetricOutput:
            // Output node doesn't generate code, values are read directly
            break;
            
        case NodeType::Power: {
            std::string base = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string exp = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "pow";
            ss << "    float " << var << " = pow(" << base << ", " << exp << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Abs: {
            std::string val = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "abs";
            ss << "    float " << var << " = abs(" << val << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Min: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "minv";
            ss << "    vec3 " << var << " = min(" << a << ", " << b << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Max: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "maxv";
            ss << "    vec3 " << var << " = max(" << a << ", " << b << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Saturate: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "sat";
            ss << "    vec3 " << var << " = clamp(" << v << ", vec3(0.0), vec3(1.0));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Sqrt: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "sqrt";
            ss << "    float " << var << " = sqrt(max(" << v << ", 0.0));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Floor: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "floor";
            ss << "    float " << var << " = floor(" << v << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Ceil: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "ceil";
            ss << "    float " << var << " = ceil(" << v << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Fract: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "fract";
            ss << "    float " << var << " = fract(" << v << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Mod: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "mod";
            ss << "    float " << var << " = mod(" << a << ", max(" << b << ", 0.0001));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Exp: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "exp";
            ss << "    float " << var << " = exp(" << v << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Log: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "log";
            ss << "    float " << var << " = log(max(" << v << ", 0.000001));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Negate: {
            std::string v = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "neg";
            ss << "    float " << var << " = -(" << v << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Dot: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "dot";
            ss << "    float " << var << " = dot(" << a << ", " << b << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Normalize: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "norm";
            ss << "    vec3 " << var << " = normalize(" << vec << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Length: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "len";
            ss << "    float " << var << " = length(" << vec << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Cross: {
            std::string a = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "cross";
            ss << "    vec3 " << var << " = cross(" << a << ", " << b << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Reflect: {
            std::string i = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string n = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "refl";
            ss << "    vec3 " << var << " = reflect(" << i << ", normalize(" << n << "));\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::Refract: {
            std::string i = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string n = GetPinValue(graph, node.inputPins[1], PinType::Vec3, pinVarNames);
            std::string eta = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string var = varPrefix + "refr";
            ss << "    vec3 " << var << " = refract(" << i << ", normalize(" << n << "), " << eta << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::SeparateVec4: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec4, pinVarNames);
            pinVarNames[node.outputPins[0]] = "(" + vec + ").x";
            pinVarNames[node.outputPins[1]] = "(" + vec + ").y";
            pinVarNames[node.outputPins[2]] = "(" + vec + ").z";
            pinVarNames[node.outputPins[3]] = "(" + vec + ").w";
            break;
        }

        case NodeType::SeparateVec2: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec2, pinVarNames);
            pinVarNames[node.outputPins[0]] = "(" + vec + ").x";
            pinVarNames[node.outputPins[1]] = "(" + vec + ").y";
            break;
        }
        
        case NodeType::CombineVec4: {
            std::string r = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string g = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string b = GetPinValue(graph, node.inputPins[2], PinType::Float, pinVarNames);
            std::string a = GetPinValue(graph, node.inputPins[3], PinType::Float, pinVarNames);
            std::string var = varPrefix + "combine4";
            ss << "    vec4 " << var << " = vec4(" << r << ", " << g << ", " << b << ", " << a << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }

        case NodeType::CombineVec2: {
            std::string x = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string y = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "combine2";
            ss << "    vec2 " << var << " = vec2(" << x << ", " << y << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        // Utility nodes
        case NodeType::Reroute: {
            // Passthrough: forward input directly to output
            std::string val = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            pinVarNames[node.outputPins[0]] = val;
            break;
        }
        
        case NodeType::Frame:
            // Frame is editor-only, no code generation
            break;
        
        // Type conversion nodes
        case NodeType::FloatToVec3: {
            std::string val = GetPinValue(graph, node.inputPins[0], PinType::Float, pinVarNames);
            std::string var = varPrefix + "f2v3";
            ss << "    vec3 " << var << " = vec3(" << val << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Vec3ToFloat: {
            std::string vec = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string var = varPrefix + "v3f";
            ss << "    float " << var << " = (" << vec << ").x;\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Vec2ToVec3: {
            std::string vec2 = GetPinValue(graph, node.inputPins[0], PinType::Vec2, pinVarNames);
            std::string z = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "v2v3";
            ss << "    vec3 " << var << " = vec3(" << vec2 << ", " << z << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Vec3ToVec4: {
            std::string vec3 = GetPinValue(graph, node.inputPins[0], PinType::Vec3, pinVarNames);
            std::string a = GetPinValue(graph, node.inputPins[1], PinType::Float, pinVarNames);
            std::string var = varPrefix + "v3v4";
            ss << "    vec4 " << var << " = vec4(" << vec3 << ", " << a << ");\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
        
        case NodeType::Vec4ToVec3: {
            std::string vec4 = GetPinValue(graph, node.inputPins[0], PinType::Vec4, pinVarNames);
            std::string var = varPrefix + "v4v3";
            ss << "    vec3 " << var << " = (" << vec4 << ").xyz;\n";
            pinVarNames[node.outputPins[0]] = var;
            break;
        }
            
        default:
            break;
    }
    
    return ss.str();
}

std::string MaterialCompiler::GetPinValue(const MaterialGraph& graph, PinID pinId,
                                           PinType desiredType,
                                           const std::unordered_map<PinID, std::string>& pinVarNames) {
    const MaterialPin* pin = graph.GetPin(pinId);
    if (!pin) return GetDefaultValue(desiredType, 0.0f);

    // If input is connected, convert from output pin type to desired type.
    if (pin->direction == PinDirection::Input) {
        LinkID linkId = graph.FindLinkByEndPin(pinId);
        if (linkId != INVALID_LINK_ID) {
            const MaterialLink* link = graph.GetLink(linkId);
            if (link) {
                const MaterialPin* startPin = graph.GetPin(link->startPinId);
                auto it = pinVarNames.find(link->startPinId);
                if (startPin && it != pinVarNames.end()) {
                    return ConvertType(it->second, startPin->type, desiredType);
                }
            }
        }
    }

    // Not connected: use this pin's default, converted to desired type if needed.
    return ConvertType(GetDefaultValue(pin->type, pin->defaultValue), pin->type, desiredType);
}

std::string MaterialCompiler::GetDefaultValue(PinType type, const PinValue& defaultVal) {
    switch (type) {
        case PinType::Float:
            if (std::holds_alternative<float>(defaultVal)) {
                return std::to_string(std::get<float>(defaultVal));
            }
            return "0.0";
            
        case PinType::Vec2:
            if (std::holds_alternative<glm::vec2>(defaultVal)) {
                const auto& v = std::get<glm::vec2>(defaultVal);
                return "vec2(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")";
            }
            return "vec2(0.0)";
            
        case PinType::Vec3:
            if (std::holds_alternative<glm::vec3>(defaultVal)) {
                const auto& v = std::get<glm::vec3>(defaultVal);
                return "vec3(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
            }
            return "vec3(0.0)";
            
        case PinType::Vec4:
            if (std::holds_alternative<glm::vec4>(defaultVal)) {
                const auto& v = std::get<glm::vec4>(defaultVal);
                return "vec4(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ", " + std::to_string(v.w) + ")";
            }
            return "vec4(0.0)";
            
        default:
            return "0.0";
    }
}

std::string MaterialCompiler::GetGLSLTypeName(PinType type) {
    switch (type) {
        case PinType::Float: return "float";
        case PinType::Vec2: return "vec2";
        case PinType::Vec3: return "vec3";
        case PinType::Vec4: return "vec4";
        case PinType::Sampler2D: return "sampler2D";
    }
    return "float";
}

std::string MaterialCompiler::ConvertType(const std::string& value, PinType from, PinType to) {
    if (from == to) return value;
    
    int fromComp = GetPinTypeComponents(from);
    int toComp = GetPinTypeComponents(to);
    
    if (fromComp == 1 && toComp > 1) {
        // Broadcast float to vector
        return GetGLSLTypeName(to) + "(" + value + ")";
    } else if (fromComp > 1 && toComp == 1) {
        // Extract first component
        return "(" + value + ").x";
    } else if (fromComp == 2 && toComp == 3) {
        return "vec3(" + value + ", 0.0)";
    } else if (fromComp == 2 && toComp == 4) {
        return "vec4(" + value + ", 0.0, 1.0)";
    } else if (fromComp == 3 && toComp == 2) {
        return "(" + value + ").xy";
    } else if (fromComp == 4 && toComp == 2) {
        return "(" + value + ").xy";
    } else if (fromComp == 4 && toComp == 3) {
        return "(" + value + ").xyz";
    } else if (fromComp < toComp) {
        // Expand with zeros/ones
        if (toComp == 4 && fromComp == 3) {
            return "vec4(" + value + ", 1.0)";
        }
    } else if (fromComp > toComp) {
        // Truncate
        if (toComp == 3) return "(" + value + ").xyz";
        if (toComp == 2) return "(" + value + ").xy";
    }
    
    return value;
}

} // namespace lucent::material

