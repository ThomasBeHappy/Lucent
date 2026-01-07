#version 450

// Fullscreen triangle vertex shader
// Generates a triangle that covers the entire screen
// No vertex buffer needed - uses gl_VertexIndex

layout(location = 0) out vec2 outUV;

void main() {
    // Generate fullscreen triangle vertices
    // Vertex 0: (-1, -1), UV (0, 0)
    // Vertex 1: ( 3, -1), UV (2, 0)
    // Vertex 2: (-1,  3), UV (0, 2)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}

