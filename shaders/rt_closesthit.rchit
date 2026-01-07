#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require

layout(location = 0) rayPayloadInEXT vec3 payload;

layout(set = 0, binding = 0) uniform accelerationStructureEXT topLevelAS;

layout(set = 0, binding = 2, scalar) readonly buffer Vertices {
    vec3 positions[];
};

layout(set = 0, binding = 4, scalar) readonly buffer Materials {
    vec4 materials[];  // 3 vec4s per material: baseColor, emissive, props
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

void main() {
    // Get triangle vertices
    uint primIdx = gl_PrimitiveID;
    uint idx0 = primIdx * 3 + 0;
    uint idx1 = primIdx * 3 + 1;
    uint idx2 = primIdx * 3 + 2;
    
    vec3 v0 = positions[idx0];
    vec3 v1 = positions[idx1];
    vec3 v2 = positions[idx2];
    
    // Compute barycentric coordinates
    vec3 bary = vec3(1.0 - hitAttribs.x - hitAttribs.y, hitAttribs.x, hitAttribs.y);
    
    // Compute hit position
    vec3 hitPos = v0 * bary.x + v1 * bary.y + v2 * bary.z;
    
    // Compute geometric normal
    vec3 edge1 = v1 - v0;
    vec3 edge2 = v2 - v0;
    vec3 normal = normalize(cross(edge1, edge2));
    
    // Ensure normal faces the ray
    if (dot(normal, gl_WorldRayDirectionEXT) > 0.0) {
        normal = -normal;
    }
    
    // Get material (simplified - use material 0 for now)
    uint matIdx = 0;
    vec4 baseColor = materials[matIdx * 3 + 0];
    vec4 emissive = materials[matIdx * 3 + 1];
    
    // Simple diffuse shading
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    float NdotL = max(dot(normal, lightDir), 0.0);
    
    // Combine direct lighting and ambient
    vec3 diffuse = baseColor.rgb * (NdotL * 0.7 + 0.3);
    
    // Add emission
    payload = diffuse + emissive.rgb * emissive.a;
}

