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
    // Volume payload
    bool volumeHit;
    vec3 volumeColor;   // premultiplied
    float volumeAlpha;
    vec3 volumeExitPos;
};

layout(location = 0) rayPayloadInEXT RayPayload payload;

// Lights (same layout as raygen)
struct GPULight {
    vec3 position;
    uint type;
    vec3 color;
    float intensity;
    vec3 direction;
    float range;
    float innerAngle;
    float outerAngle;
    float areaWidth;
    float areaHeight;
    vec3 areaTangent;
    uint areaShape;
};

layout(scalar, set = 0, binding = 9) readonly buffer LightBuffer {
    GPULight lights[];
};

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

const float PI = 3.14159265359;

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

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
    float tEnter = gl_HitTEXT;

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

    // Clamp
    tEnter = max(tEnter, 0.0);

    // Integrate homogeneous medium between [tEnter, tExit]
    const int MAX_STEPS = 48;
    float stepSize = (tExit - tEnter) / float(MAX_STEPS);

    vec3 accum = vec3(0.0);
    float trans = 1.0;

    // Pick first directional light if present, otherwise fallback
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    vec3 lightCol = vec3(2.5);
    if (lights.length() > 0) {
        lightDir = normalize(lights[0].direction);
        lightCol = lights[0].color * lights[0].intensity;
    }

    float g = clamp(vol.anisotropy, -0.99, 0.99);

    for (int i = 0; i < MAX_STEPS; i++) {
        // Midpoint sampling
        float t = tEnter + (float(i) + 0.5) * stepSize;

        // Extinction (very simplified): sigma_t = density + absorption luminance
        float sigma_t = max(vol.density, 0.0);

        // Beer-Lambert step transmittance
        float stepT = exp(-sigma_t * stepSize);

        float cosTheta = dot(-gl_WorldRayDirectionEXT, lightDir);
        float phase = phaseHG(cosTheta, g);

        vec3 scatter = vol.scatterColor * vol.density * lightCol * phase;
        scatter += vol.emission * vol.emissionStrength;

        accum += trans * scatter * stepSize;
        trans *= stepT;

        if (trans < 0.01) break;
    }

    float alpha = 1.0 - trans;
    vec3 exitPos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * tExit;

    payload.hit = true;
    payload.volumeHit = true;
    payload.volumeColor = accum;
    payload.volumeAlpha = alpha;
    payload.volumeExitPos = exitPos;
}

