#version 450

layout(location = 0) out vec2 outUV;

// Fullscreen triangle vertices
void main() {
    // Generate fullscreen triangle (3 vertices cover the entire screen)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}


