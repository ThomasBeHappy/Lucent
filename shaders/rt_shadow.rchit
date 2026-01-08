#version 460
#extension GL_EXT_ray_tracing : require

// Ray payload: must match raygen/closesthit
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
    // Volume payload (only valid for volume hits)
    bool volumeHit;
    vec3 volumeColor;   // premultiplied
    float volumeAlpha;
    vec3 volumeExitPos;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

void main() {
    // Shadow ray hit - light is occluded
    payload.radiance = vec3(0.0);
    payload.volumeHit = false;
}

