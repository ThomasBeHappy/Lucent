#version 450

layout(location = 0) out vec3 outDirection;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Fullscreen triangle that covers the entire screen
void main() {
    // Generate fullscreen triangle
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0)
    );
    
    vec2 pos = positions[gl_VertexIndex];
    
    // Reconstruct view direction from clip space
    // We use inverse viewProj to get world direction
    vec4 clipPos = vec4(pos, 1.0, 1.0); // Far plane
    vec4 worldPos = inverse(pc.viewProj) * clipPos;
    outDirection = normalize(worldPos.xyz / worldPos.w);
    
    gl_Position = vec4(pos, 0.9999, 1.0); // Near far plane to render behind everything
}

