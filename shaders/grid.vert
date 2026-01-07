#version 450

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outUV;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Generate an infinite grid plane
void main() {
    // Generate grid vertices (large quad on XZ plane)
    vec2 positions[6] = vec2[](
        vec2(-1, -1), vec2(1, -1), vec2(1, 1),
        vec2(-1, -1), vec2(1, 1), vec2(-1, 1)
    );
    
    vec2 pos = positions[gl_VertexIndex];
    
    // Scale the grid to be very large
    float gridSize = 100.0;
    vec3 worldPos = vec3(pos.x * gridSize, 0.0, pos.y * gridSize);
    
    outWorldPos = worldPos;
    outUV = pos * gridSize;
    
    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}

