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
const uint OP_FRESNEL    = 12u; // pow(1 - clamp(dot(N, V),0,1), power)
const uint OP_SUB        = 13u;
const uint OP_DIV        = 14u;
const uint OP_POW        = 15u;
const uint OP_REMAP      = 16u; // currently expanded on CPU side
const uint OP_STEP       = 17u;
const uint OP_SMOOTHSTEP = 18u;
const uint OP_SIN        = 19u;
const uint OP_COS        = 20u;
const uint OP_ABS        = 21u;
const uint OP_MIN        = 22u;
const uint OP_MAX        = 23u;
const uint OP_SQRT       = 24u;
const uint OP_FLOOR      = 25u;
const uint OP_CEIL       = 26u;
const uint OP_FRACT      = 27u;
const uint OP_MOD        = 28u;
const uint OP_EXP        = 29u;
const uint OP_LOG        = 30u;
const uint OP_NEGATE     = 31u;
const uint OP_DOT        = 32u;
const uint OP_NORMALIZE  = 33u;
const uint OP_LENGTH     = 34u;
const uint OP_CROSS      = 35u;
const uint OP_REFLECT    = 36u;
const uint OP_REFRACT    = 37u;
const uint OP_COMBINE2   = 38u;
const uint OP_COMBINE4   = 39u; // alpha register stored in ins.texIndex
const uint OP_WORLDPOS   = 40u;
const uint OP_WORLDNORM  = 41u;
const uint OP_VIEWDIR    = 42u;
const uint OP_TIME       = 43u;
const uint OP_VCOLOR     = 44u;
const uint OP_NORMALMAP  = 45u;
const uint OP_NOISE      = 46u; // imm=(scale,detail,rough,distort), texIndex selects type

// -----------------------------------------------------------------------------
// Noise helpers (value noise + fbm variants) - matches MaterialCompiler
// -----------------------------------------------------------------------------
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

float ridgedFbm3(vec3 p, float octaves, float roughness) {
    float sum = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    int iters = int(clamp(octaves, 1.0, 12.0));
    for (int i = 0; i < iters; ++i) {
        float n = valueNoise3(p * freq);
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
        sum += amp * abs(n * 2.0 - 1.0);
        freq *= 2.0;
        amp *= clamp(roughness, 0.0, 1.0);
    }
    return sum;
}

void evalMaterialIR(
    uint matIdx,
    vec2 uv,
    vec3 hitPosWS,
    vec3 geomNormalWS,
    vec3 viewDirWS,
    vec3 tangentWS,
    vec3 bitangentWS,
    out vec4 outBaseColor,
    out vec4 outEmissive,
    out float outMetallic,
    out float outRoughness,
    out vec3 outNormalWS,
    out float outAlpha
) {
    outBaseColor = vec4(0.0);
    outEmissive = vec4(0.0);
    outMetallic = 0.0;
    outRoughness = 0.5;
    outNormalWS = geomNormalWS;
    outAlpha = 1.0;

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
        } else if (ins.type == OP_NORMALMAP) {
            vec2 tuv = (ins.a != 0u) ? a.xy : uv;
            float strength = (ins.b != 0u) ? b.x : 1.0;
            vec3 n = texture(materialTextures[nonuniformEXT(ins.texIndex)], tuv).xyz * 2.0 - 1.0;
            n = normalize(vec3(n.xy * strength, n.z));
            mat3 tbn = mat3(normalize(tangentWS), normalize(bitangentWS), normalize(geomNormalWS));
            vec3 nws = normalize(tbn * n);
            r = vec4(nws, 1.0);
        } else if (ins.type == OP_NOISE) {
            vec3 p = (ins.a != 0u) ? a.xyz : vec3(uv, 0.0);
            float scale = ins.imm.x;
            float detail = ins.imm.y;
            float rough = ins.imm.z;
            float distort = ins.imm.w;
            vec3 pp = p * scale;
            if (distort > 0.0) {
                pp += distort * vec3(valueNoise3(pp + vec3(31.7)), valueNoise3(pp + vec3(17.3)), valueNoise3(pp + vec3(9.2)));
            }
            float n = fbm3(pp, detail, rough);
            if (ins.texIndex == 1u) n = valueNoise3(pp);
            else if (ins.texIndex == 2u) n = ridgedFbm3(pp, detail, rough);
            else if (ins.texIndex == 3u) n = turbulence3(pp, detail, rough);
            r = vec4(n, n, n, 1.0);
        } else if (ins.type == OP_ADD) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa + bb;
        } else if (ins.type == OP_SUB) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa - bb;
        } else if (ins.type == OP_MUL) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa * bb;
        } else if (ins.type == OP_DIV) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = aa / max(bb, vec4(1e-6));
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
        } else if (ins.type == OP_COMBINE2) {
            r = vec4(a.x, b.x, 0.0, 0.0);
        } else if (ins.type == OP_COMBINE4) {
            vec4 d = (ins.texIndex <= MAX_REGS) ? regs[ins.texIndex] : vec4(1.0);
            r = vec4(a.x, b.x, c.x, d.x);
        } else if (ins.type == OP_FRESNEL) {
            float power = max(a.x, 0.0);
            float ndv = clamp(dot(normalize(geomNormalWS), normalize(viewDirWS)), 0.0, 1.0);
            float f = pow(1.0 - ndv, power);
            r = vec4(f, 0.0, 0.0, 0.0);
        } else if (ins.type == OP_STEP) {
            r = vec4(step(a.x, b.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_SMOOTHSTEP) {
            r = vec4(smoothstep(a.x, b.x, c.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_SIN) {
            r = vec4(sin(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_COS) {
            r = vec4(cos(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_ABS) {
            r = vec4(abs(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_MIN) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = min(aa, bb);
        } else if (ins.type == OP_MAX) {
            vec4 aa = isScalar(a) ? splatScalar(a) : a;
            vec4 bb = isScalar(b) ? splatScalar(b) : b;
            r = max(aa, bb);
        } else if (ins.type == OP_SQRT) {
            r = vec4(sqrt(max(a.x, 0.0)), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_FLOOR) {
            r = vec4(floor(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_CEIL) {
            r = vec4(ceil(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_FRACT) {
            r = vec4(fract(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_MOD) {
            r = vec4(mod(a.x, max(b.x, 1e-6)), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_EXP) {
            r = vec4(exp(a.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_LOG) {
            r = vec4(log(max(a.x, 1e-6)), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_NEGATE) {
            r = vec4(-a.x, 0.0, 0.0, 0.0);
        } else if (ins.type == OP_POW) {
            r = vec4(pow(max(a.x, 0.0), b.x), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_DOT) {
            r = vec4(dot(a.xyz, b.xyz), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_NORMALIZE) {
            r = vec4(normalize(a.xyz), 1.0);
        } else if (ins.type == OP_LENGTH) {
            r = vec4(length(a.xyz), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_CROSS) {
            r = vec4(cross(a.xyz, b.xyz), 1.0);
        } else if (ins.type == OP_REFLECT) {
            r = vec4(reflect(a.xyz, normalize(b.xyz)), 1.0);
        } else if (ins.type == OP_REFRACT) {
            r = vec4(refract(a.xyz, normalize(b.xyz), c.x), 1.0);
        } else if (ins.type == OP_WORLDPOS) {
            r = vec4(hitPosWS, 1.0);
        } else if (ins.type == OP_WORLDNORM) {
            r = vec4(geomNormalWS, 1.0);
        } else if (ins.type == OP_VIEWDIR) {
            r = vec4(viewDirWS, 1.0);
        } else if (ins.type == OP_TIME) {
            r = vec4(float(pc.frameIndex), 0.0, 0.0, 0.0);
        } else if (ins.type == OP_VCOLOR) {
            r = vec4(1.0);
        }

        uint dst = i + 1u;
        regs[dst] = r;
    }

    if (h.baseColorReg != 0u && h.baseColorReg <= MAX_REGS) outBaseColor = regs[h.baseColorReg];
    if (h.emissiveReg != 0u && h.emissiveReg <= MAX_REGS) outEmissive = regs[h.emissiveReg];
    if (h.metallicReg != 0u && h.metallicReg <= MAX_REGS) outMetallic = regs[h.metallicReg].x;
    if (h.roughnessReg != 0u && h.roughnessReg <= MAX_REGS) outRoughness = regs[h.roughnessReg].x;
    if (h.normalReg != 0u && h.normalReg <= MAX_REGS) {
        vec3 n = regs[h.normalReg].xyz;
        if (dot(n, n) > 1e-8) outNormalWS = normalize(n);
    }
    if (h.alphaReg != 0u && h.alphaReg <= MAX_REGS) {
        outAlpha = clamp(regs[h.alphaReg].x, 0.0, 1.0);
    }
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
    
    // Build tangent basis from triangle (for NormalMap)
    vec3 p0 = vtx0.position;
    vec3 p1 = vtx1.position;
    vec3 p2 = vtx2.position;
    vec2 w0 = vtx0.uv;
    vec2 w1 = vtx1.uv;
    vec2 w2 = vtx2.uv;
    vec3 e1 = p1 - p0;
    vec3 e2 = p2 - p0;
    vec2 d1 = w1 - w0;
    vec2 d2 = w2 - w0;
    float det = d1.x * d2.y - d1.y * d2.x;
    vec3 tangent = vec3(1.0, 0.0, 0.0);
    vec3 bitangent = vec3(0.0, 1.0, 0.0);
    if (abs(det) > 1e-8) {
        float invDet = 1.0 / det;
        tangent = normalize((e1 * d2.y - e2 * d1.y) * invDet);
        bitangent = normalize((e2 * d1.x - e1 * d2.x) * invDet);
        // Orthonormalize w.r.t. normal
        tangent = normalize(tangent - normal * dot(normal, tangent));
        bitangent = normalize(cross(normal, tangent));
    } else {
        // Fallback: build an arbitrary basis from normal
        vec3 up = (abs(normal.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        tangent = normalize(cross(up, normal));
        bitangent = normalize(cross(normal, tangent));
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
    vec3 irNormal = normal;
    float irAlpha = 1.0;
    vec3 viewDir = normalize(-gl_WorldRayDirectionEXT);
    evalMaterialIR(matIdx, uv, hitPos, normal, viewDir, tangent, bitangent, irBase, irEmissive, irMetallic, irRoughness, irNormal, irAlpha);
    bool hasIR = (matIdx < headers.length()) && (headers[matIdx].instrCount != 0u);
    if (hasIR) {
        albedo = (irBase.xyz);
        emissive = irEmissive.xyz;
        metallic = irMetallic;
        roughness = max(irRoughness, 0.04);
        normal = normalize(irNormal);
    }

    // Ensure normal faces the ray (after IR override)
    if (dot(normal, gl_WorldRayDirectionEXT) > 0.0) {
        normal = -normal;
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

