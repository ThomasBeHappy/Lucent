#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_scalar_block_layout : enable

// Volume closest-hit: integrates a homogeneous medium inside the AABB and returns premultiplied color+alpha

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
    float hitT;
    // Volume payload
    bool volumeHit;
    uint volumeIdx;
    float volumeEnterT;
    float volumeExitT;
    vec3 volumeExitPos;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

// Volume buffer (matches C++ GPUVolume)
struct GPUVolume {
    mat4 transform;
    vec3 scatterColor;
    float density;
    vec3 absorption;
    float anisotropy;
    vec3 emission;
    float emissionStrength;
    vec3 aabbMin;
    float pad0;
    vec3 aabbMax;
    float pad1;
};

layout(scalar, set = 0, binding = 13) readonly buffer VolumeBuffer {
    GPUVolume volumes[];
};

void main() {
    // Identify which volume primitive we hit (one AABB per primitive)
    uint volIdx = gl_PrimitiveID;
    if (volIdx >= uint(volumes.length())) {
        payload.volumeHit = false;
        payload.hit = false;
        return;
    }

    GPUVolume vol = volumes[volIdx];

    // Entry point for this hit
    float tEnter = max(gl_HitTEXT, 0.0);

    // Compute exit distance by intersecting the same AABB.
    // We store per-volume bounds in VolumeBuffer; TLAS volume instance transform is identity,
    // so object space == world space for volumes.
    vec3 ro = gl_ObjectRayOriginEXT;
    vec3 rd = gl_ObjectRayDirectionEXT;
    vec3 invDir = 1.0 / rd;
    vec3 t0 = (vol.aabbMin - ro) * invDir;
    vec3 t1 = (vol.aabbMax - ro) * invDir;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar  = min(min(tMax.x, tMax.y), tMax.z);
    float tExit = max(tFar, tEnter);

    payload.hit = true;
    payload.hitT = tEnter;
    payload.volumeHit = true;
    payload.volumeIdx = volIdx;
    payload.volumeEnterT = tEnter;
    payload.volumeExitT = tExit;
    payload.volumeExitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * tExit;
}

