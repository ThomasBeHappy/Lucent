#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrInput;

layout(push_constant) uniform PostFXParams {
    vec4 settings; // exposure, tonemapMode, gamma, unused
} params;

#define EXPOSURE params.settings.x
#define TONEMAP_MODE int(params.settings.y)
#define GAMMA params.settings.z

// Reinhard tonemapping
vec3 tonemapReinhard(vec3 color) {
    return color / (color + vec3(1.0));
}

// Reinhard extended (better highlight preservation)
vec3 tonemapReinhardExtended(vec3 color, float maxWhite) {
    vec3 numerator = color * (1.0 + (color / vec3(maxWhite * maxWhite)));
    return numerator / (1.0 + color);
}

// ACES filmic tonemapping
vec3 tonemapACES(vec3 color) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// Uncharted 2 filmic tonemapping
vec3 uncharted2Partial(vec3 x) {
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemapUncharted2(vec3 color) {
    float exposureBias = 2.0;
    vec3 curr = uncharted2Partial(color * exposureBias);
    vec3 W = vec3(11.2);
    vec3 whiteScale = vec3(1.0) / uncharted2Partial(W);
    return curr * whiteScale;
}

// AgX tonemapping (newer, more neutral)
vec3 tonemapAgX(vec3 color) {
    // Simplified AgX approximation
    mat3 agxTransform = mat3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104
    );
    
    vec3 val = agxTransform * color;
    val = clamp(log2(val), -10.0, 6.5);
    val = (val - vec3(-10.0)) / vec3(16.5);
    
    // Sigmoid
    val = val * val * (3.0 - 2.0 * val);
    
    return val;
}

void main() {
    vec3 hdrColor = texture(hdrInput, inUV).rgb;
    
    // Apply exposure
    vec3 exposed = hdrColor * EXPOSURE;
    
    // Apply tonemapping based on mode
    vec3 mapped;
    switch (TONEMAP_MODE) {
        case 0: // None (linear clamp)
            mapped = clamp(exposed, 0.0, 1.0);
            break;
        case 1: // Reinhard
            mapped = tonemapReinhard(exposed);
            break;
        case 2: // ACES
            mapped = tonemapACES(exposed);
            break;
        case 3: // Uncharted 2
            mapped = tonemapUncharted2(exposed);
            break;
        case 4: // AgX
            mapped = tonemapAgX(exposed);
            break;
        default:
            mapped = tonemapReinhard(exposed);
            break;
    }
    
    // Gamma correction
    vec3 corrected = pow(mapped, vec3(1.0 / GAMMA));
    
    outColor = vec4(corrected, 1.0);
}


