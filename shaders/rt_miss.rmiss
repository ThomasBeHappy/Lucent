#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 payload;

void main() {
    // Sky color (simple gradient)
    vec3 direction = gl_WorldRayDirectionEXT;
    float t = 0.5 * (direction.y + 1.0);
    payload = mix(vec3(1.0), vec3(0.5, 0.7, 1.0), t) * 0.5;
}


