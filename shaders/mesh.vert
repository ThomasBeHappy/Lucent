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

// Push constants for per-object data
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;
    vec4 baseColor;      // RGB + alpha
    vec4 materialParams; // metallic, roughness, emissiveIntensity, unused
    vec4 emissive;       // RGB + unused
    vec4 cameraPos;      // Camera world position
} pc;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    outWorldPos = worldPos.xyz;
    
    // Transform normal to world space
    mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
    outNormal = normalize(normalMatrix * inNormal);
    outTangent = normalize(normalMatrix * inTangent.xyz);
    outBitangent = cross(outNormal, outTangent) * inTangent.w;
    
    outUV = inUV;
    
    gl_Position = pc.viewProj * worldPos;
}
