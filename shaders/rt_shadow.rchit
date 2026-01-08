#version 460
#extension GL_EXT_ray_tracing : require

// Shadow ray payload (used by rt_raygen.rgen traceShadow via location=1)
struct ShadowPayload {
    float visibility; // 1 = visible, 0 = occluded
};
layout(location = 1) rayPayloadInEXT ShadowPayload shadowPayload;

void main() {
    // Shadow ray hit - light is occluded
    shadowPayload.visibility = 0.0;
}

