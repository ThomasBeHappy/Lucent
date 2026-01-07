#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D offscreenImage;

void main() {
    // Sample the offscreen HDR image and output to swapchain
    vec3 hdrColor = texture(offscreenImage, inUV).rgb;
    
    // Simple Reinhard tonemapping for now
    vec3 mapped = hdrColor / (hdrColor + vec3(1.0));
    
    // Gamma correction (sRGB)
    mapped = pow(mapped, vec3(1.0 / 2.2));
    
    outColor = vec4(mapped, 1.0);
}

