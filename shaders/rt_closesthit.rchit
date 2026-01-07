#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require

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

layout(push_constant) uniform PushConstants {
    uint frameIndex;
    uint sampleIndex;
    uint maxBounces;
    float clampValue;
} pc;

hitAttributeEXT vec2 hitAttribs;

const float PI = 3.14159265359;

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

