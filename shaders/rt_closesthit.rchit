#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : enable

// Ray payload: carries radiance, throughput, hit info for multi-bounce
struct RayPayload {
    vec3 radiance;
    vec3 throughput;
    vec3 hitPos;
    vec3 hitNormal;
    vec3 nextDir;
    uint seed;
    bool hit;
    bool done;
    // Material properties at hit
    vec3 albedo;
    vec3 emissive;
    float metallic;
    float roughness;
    float hitT;
    // Volume payload (only valid for volume hits)
    bool volumeHit;
    uint volumeIdx;
    float volumeEnterT;
    float volumeExitT;
    vec3 volumeExitPos;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

layout(set = 0, binding = 0) uniform accelerationStructureEXT topLevelAS;

// RTVertex: pos(vec3) + pad, normal(vec3) + pad, uv(vec2) + pad2 = 48 bytes
struct RTVertex {
    vec3 position;
    float pad0;
    vec3 normal;
    float pad1;
    vec2 uv;
    vec2 pad2;
};

layout(set = 0, binding = 2, std430) readonly buffer Vertices {
    RTVertex vertices[];
};

layout(set = 0, binding = 4, scalar) readonly buffer Materials {
    vec4 materials[];  // 3 vec4s per material: baseColor, emissive, props
};

layout(set = 0, binding = 6, scalar) readonly buffer PrimitiveMaterials {
    uint materialIds[];
};

// Material evaluation: bind a fixed-size global texture array + per-material IR
layout(set = 0, binding = 14) uniform sampler2D materialTextures[256];

struct RTMaterialHeader {
    uint instrOffset;
    uint instrCount;
    uint baseColorReg;
    uint metallicReg;
    uint roughnessReg;
    uint emissiveReg;
    uint normalReg;
    uint alphaReg;
};

layout(set = 0, binding = 15, scalar) readonly buffer MaterialHeaders {
    RTMaterialHeader headers[];
};

struct RTMaterialInstr {
    uint type;
    uint a;
    uint b;
    uint c;
    uint texIndex;
    vec4 imm;
};

layout(set = 0, binding = 16, scalar) readonly buffer MaterialInstrs {
    RTMaterialInstr instrs[];
};

layout(push_constant) uniform PushConstants {
    uint frameIndex;
    uint sampleIndex;
    uint maxBounces;
    float clampValue;
} pc;

hitAttributeEXT vec2 hitAttribs;

const float PI = 3.14159265359;

bool isScalar(vec4 v) {
    return abs(v.y) < 1e-6 && abs(v.z) < 1e-6 && abs(v.w) < 1e-6;
}

vec4 splatScalar(vec4 v) {
    return vec4(v.x, v.x, v.x, v.x);
}

// Instruction opcodes (must match CPU-side compiler)
const uint OP_CONST      = 1u;
const uint OP_UV         = 2u;
const uint OP_TEX2D      = 3u;
const uint OP_ADD        = 4u;
const uint OP_MUL        = 5u;
const uint OP_LERP       = 6u;
const uint OP_CLAMP      = 7u;
const uint OP_SATURATE   = 8u;
const uint OP_ONEMINUS   = 9u;
const uint OP_SWIZZLE    = 10u; // texIndex: 0=r,1=g,2=b,3=a,4=rgb
const uint OP_COMBINE3   = 11u;

void evalMaterialIR(uint matIdx, vec2 uv, out vec4 outBaseColor, out vec4 outEmissive, out float outMetallic, out float outRoughness) {
    outBaseColor = vec4(0.0);
    outEmissive = vec4(0.0);
    outMetallic = 0.0;
    outRoughness = 0.5;

    if (matIdx >= headers.length()) return;
    RTMaterialHeader h = headers[matIdx];
    if (h.instrCount == 0u) return;

    // Hard cap to keep shader stack bounded
    const uint MAX_REGS = 128u;
    const uint count = min(h.instrCount, MAX_REGS);
    vec4 regs[129];
    regs[0] = vec4(0.0);

    for (uint i = 0u; i < count; ++i) {
        RTMaterialInstr ins = instrs[h.instrOffset + i];
        vec4 a = (ins.a <= MAX_REGS) ? regs[ins.a] : vec4(0.0);
        vec4 b = (ins.b <= MAX_REGS) ? regs[ins.b] : vec4(0.0);
        vec4 c = (ins.c <= MAX_REGS) ? regs[ins.c] : vec4(0.0);

        vec4 r = vec4(0.0);
        if (ins.type == OP_CONST) {
            r = ins.imm;
        } else if (ins.type == OP_UV) {
            r = vec4(uv, 0.0, 0.0);
        } else if (ins.type == OP_TEX2D) {
            vec2 tuv = (ins.a != 0u) ? a.xy : uv;
            r = texture(materialTextures[nonuniformEXT(ins.texIndex)], tuv);
        } else if (ins.type == OP_ADD) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa + bb;
        } else if (ins.type == OP_MUL) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa * bb;
        } else if (ins.type == OP_LERP) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            float t = c.x;
            r = mix(aa, bb, t);
        } else if (ins.type == OP_CLAMP) {
            r = clamp(a, b, c);
        } else if (ins.type == OP_SATURATE) {
            r = clamp(a, vec4(0.0), vec4(1.0));
        } else if (ins.type == OP_ONEMINUS) {
            r = vec4(1.0) - a;
        } else if (ins.type == OP_SWIZZLE) {
            if (ins.texIndex == 0u) r = vec4(a.x, 0.0, 0.0, 0.0);
            else if (ins.texIndex == 1u) r = vec4(a.y, 0.0, 0.0, 0.0);
            else if (ins.texIndex == 2u) r = vec4(a.z, 0.0, 0.0, 0.0);
            else if (ins.texIndex == 3u) r = vec4(a.w, 0.0, 0.0, 0.0);
            else r = vec4(a.xyz, 1.0);
        } else if (ins.type == OP_COMBINE3) {
            r = vec4(a.x, b.x, c.x, 1.0);
        }

        uint dst = i + 1u;
        regs[dst] = r;
    }

    if (h.baseColorReg != 0u && h.baseColorReg <= MAX_REGS) outBaseColor = regs[h.baseColorReg];
    if (h.emissiveReg != 0u && h.emissiveReg <= MAX_REGS) outEmissive = regs[h.emissiveReg];
    if (h.metallicReg != 0u && h.metallicReg <= MAX_REGS) outMetallic = regs[h.metallicReg].x;
    if (h.roughnessReg != 0u && h.roughnessReg <= MAX_REGS) outRoughness = regs[h.roughnessReg].x;
}

// PCG random
uint pcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint seed) {
    seed = pcgHash(seed);
    return float(seed) / 4294967295.0;
}

vec3 randomInUnitSphere(inout uint seed) {
    float z = randomFloat(seed) * 2.0 - 1.0;
    float a = randomFloat(seed) * 2.0 * PI;
    float r = sqrt(1.0 - z * z);
    return vec3(r * cos(a), r * sin(a), z);
}

vec3 randomCosineHemisphere(vec3 normal, inout uint seed) {
    return normalize(normal + randomInUnitSphere(seed));
}

// Schlick Fresnel approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    payload.volumeHit = false;
    payload.hitT = gl_HitTEXT;
    // Get triangle vertices using gl_PrimitiveID
    uint primIdx = gl_PrimitiveID;
    uint idx0 = primIdx * 3 + 0;
    uint idx1 = primIdx * 3 + 1;
    uint idx2 = primIdx * 3 + 2;
    
    RTVertex vtx0 = vertices[idx0];
    RTVertex vtx1 = vertices[idx1];
    RTVertex vtx2 = vertices[idx2];
    
    // Compute barycentric coordinates
    vec3 bary = vec3(1.0 - hitAttribs.x - hitAttribs.y, hitAttribs.x, hitAttribs.y);
    
    // Interpolate position
    vec3 hitPos = vtx0.position * bary.x + vtx1.position * bary.y + vtx2.position * bary.z;
    
    // Interpolate smooth normal
    vec3 normal = normalize(vtx0.normal * bary.x + vtx1.normal * bary.y + vtx2.normal * bary.z);
    
    // Interpolate UV (for future texture sampling)
    vec2 uv = vtx0.uv * bary.x + vtx1.uv * bary.y + vtx2.uv * bary.z;
    
    // Ensure normal faces the ray
    if (dot(normal, gl_WorldRayDirectionEXT) > 0.0) {
        normal = -normal;
    }
    
    // Material lookup: per-primitive material id (packed as 3 vec4s per material)
    uint matCount = max(1u, uint(materials.length()) / 3u);
    uint matIdx = materialIds[primIdx];
    matIdx = min(matIdx, matCount - 1u);
    
    vec4 baseColor = materials[matIdx * 3 + 0];
    vec4 emissivePacked = materials[matIdx * 3 + 1];
    vec4 props = materials[matIdx * 3 + 2];  // metallic, roughness, ior, flags
    
    vec3 albedo = baseColor.rgb;
    vec3 emissive = emissivePacked.rgb * emissivePacked.a;
    float metallic = props.x;
    float roughness = max(props.y, 0.04);  // Clamp to avoid perfect mirrors

    // If a UV-driven IR is present, override the constant-packed material values
    vec4 irBase = vec4(0.0);
    vec4 irEmissive = vec4(0.0);
    float irMetallic = metallic;
    float irRoughness = roughness;
    evalMaterialIR(matIdx, uv, irBase, irEmissive, irMetallic, irRoughness);
    bool hasIR = (matIdx < headers.length()) && (headers[matIdx].instrCount != 0u);
    if (hasIR) {
        albedo = (irBase.xyz);
        emissive = irEmissive.xyz;
        metallic = irMetallic;
        roughness = max(irRoughness, 0.04);
    }
    
    // Store hit info in payload for raygen to handle bouncing
    payload.hit = true;
    payload.hitPos = hitPos;
    payload.hitNormal = normal;
    payload.albedo = albedo;
    payload.emissive = emissive;
    payload.metallic = metallic;
    payload.roughness = roughness;
    
    // Generate next bounce direction based on material
    uint seed = payload.seed;
    
    // Blend between diffuse and specular based on metallic
    float specChance = mix(0.04, 1.0, metallic);  // Non-metals have ~4% specular
    
    if (randomFloat(seed) < specChance) {
        // Specular reflection (glossy based on roughness)
        vec3 reflected = reflect(gl_WorldRayDirectionEXT, normal);
        // Roughness-based perturbation
        vec3 perturbation = randomInUnitSphere(seed) * roughness;
        payload.nextDir = normalize(reflected + perturbation);
        // Ensure above surface
        if (dot(payload.nextDir, normal) < 0.0) {
            payload.nextDir = reflected;
        }
    } else {
        // Diffuse reflection (cosine-weighted hemisphere)
        payload.nextDir = randomCosineHemisphere(normal, seed);
    }
    
    payload.seed = seed;
}

