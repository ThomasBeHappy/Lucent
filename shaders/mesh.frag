#version 450
#extension GL_EXT_scalar_block_layout : enable

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;
layout(location = 5) in vec4 inShadowCoord;

layout(location = 0) out vec4 outColor;

// Shadow map sampler
layout(set = 0, binding = 0) uniform sampler2D shadowMap;

// Light buffer - use scalar layout to match C++ struct packing
struct GPULight {
    vec3 position;
    uint type;       // 0=Directional, 1=Point, 2=Spot, 3=Area
    vec3 color;
    float intensity;
    vec3 direction;
    float range;
    float innerAngle;
    float outerAngle;
    vec2 padding;
};

layout(scalar, set = 0, binding = 1) readonly buffer LightBuffer {
    GPULight lights[];
};

// Light types
const uint LIGHT_DIRECTIONAL = 0u;
const uint LIGHT_POINT = 1u;
const uint LIGHT_SPOT = 2u;
const uint LIGHT_AREA = 3u;

// Push constants matching vertex shader
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;
    vec4 baseColor;      // RGB + alpha
    vec4 materialParams; // metallic, roughness, emissiveIntensity, shadowBias
    vec4 emissive;       // RGB + shadowEnabled
    vec4 cameraPos;      // Camera world position
    mat4 lightViewProj;  // Light space matrix for shadows
} pc;

// Material parameters from push constants
#define u_BaseColor pc.baseColor.rgb
#define u_Alpha pc.baseColor.a
#define u_Metallic pc.materialParams.x
#define u_Roughness pc.materialParams.y
#define u_EmissiveIntensity pc.materialParams.z
#define u_ShadowBias pc.materialParams.w
#define u_Emissive pc.emissive.rgb
#define u_ShadowEnabled pc.emissive.w
#define u_CameraPos pc.cameraPos.xyz
#define u_Exposure pc.cameraPos.w

// Ambient/environment approximation
const vec3 ambientTop = vec3(0.3, 0.35, 0.5);    // Sky color
const vec3 ambientBottom = vec3(0.1, 0.08, 0.05); // Ground color

const float PI = 3.14159265359;
const float MAX_DIST = 10000.0;

// Fresnel-Schlick
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX/Trowbridge-Reitz normal distribution
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return a2 / max(denom, 0.0001);
}

// Smith's Schlick-GGX geometry function
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Simple hemisphere ambient
vec3 hemisphereAmbient(vec3 N) {
    float blend = N.y * 0.5 + 0.5;
    return mix(ambientBottom, ambientTop, blend);
}

// Shadow calculation with PCF
float calcShadow(vec4 shadowCoord, float bias) {
    // Perspective divide
    vec3 projCoords = shadowCoord.xyz / shadowCoord.w;
    
    // Transform to [0,1] range
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    
    // Check if outside shadow map
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0) {
        return 1.0; // Not in shadow
    }
    
    // Current fragment depth
    float currentDepth = projCoords.z;
    
    // PCF (Percentage-Closer Filtering) - 3x3 kernel
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    
    return shadow;
}

void main() {
    vec3 N = normalize(inNormal);
    vec3 V = normalize(u_CameraPos - inWorldPos);
    
    // Material properties
    vec3 albedo = u_BaseColor;
    float metallic = u_Metallic;
    float roughness = max(u_Roughness, 0.04); // Prevent div by zero
    
    // Material F0 (dielectric = 0.04, metallic = albedo)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    
    // Shadow calculation (for primary directional light)
    float primaryShadow = 1.0;
    if (u_ShadowEnabled > 0.5) {
        float bias = u_ShadowBias;
        primaryShadow = calcShadow(inShadowCoord, bias);
    }
    
    // Accumulate lighting from all scene lights
    vec3 Lo = vec3(0.0);
    uint numLights = lights.length();
    bool firstDirectional = true;  // Track if we've seen the first directional light
    
    for (uint i = 0; i < numLights; i++) {
        GPULight light = lights[i];
        
        vec3 L;
        float attenuation = 1.0;
        float shadow = 1.0;
        
        if (light.type == LIGHT_DIRECTIONAL) {
            L = light.direction;
            // Use shadow map only for the FIRST directional light (matches UpdateLightMatrix)
            if (firstDirectional) {
                shadow = primaryShadow;
                firstDirectional = false;
            }
        } else if (light.type == LIGHT_POINT || light.type == LIGHT_SPOT) {
            vec3 toLight = light.position - inWorldPos;
            float lightDist = length(toLight);
            L = toLight / lightDist;
            
            // Distance attenuation
            if (light.range > 0.0) {
                attenuation = 1.0 - clamp(lightDist / light.range, 0.0, 1.0);
                attenuation *= attenuation;
            } else {
                attenuation = 1.0 / (lightDist * lightDist + 1.0);
            }
            
            // Spot cone
            if (light.type == LIGHT_SPOT) {
                float theta = dot(-L, light.direction);
                float epsilon = light.innerAngle - light.outerAngle;
                float spotFactor = clamp((theta - cos(light.outerAngle)) / epsilon, 0.0, 1.0);
                attenuation *= spotFactor;
            }
        } else {
            continue;
        }
        
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0 || attenuation <= 0.0) continue;
        
        vec3 H = normalize(V + L);
        
        // Cook-Torrance BRDF
        float NDF = distributionGGX(N, H, roughness);
        float G = geometrySmith(N, V, L, roughness);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
        vec3 specular = numerator / denominator;
        
        vec3 kS = F;
        vec3 kD = (1.0 - kS) * (1.0 - metallic);
        
        Lo += (kD * albedo / PI + specular) * light.color * light.intensity * NdotL * attenuation * shadow;
    }
    
    // Ambient (simple hemisphere + Fresnel for metals) - not affected by shadow
    vec3 ambient = hemisphereAmbient(N) * albedo * (1.0 - metallic) * 0.3;
    vec3 F_ambient = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    ambient += F_ambient * hemisphereAmbient(reflect(-V, N)) * 0.15;
    
    // Emissive
    vec3 emission = u_Emissive * u_EmissiveIntensity;
    
    // Final color (HDR)
    vec3 color = ambient + Lo + emission;
    
    // Apply exposure
    color *= u_Exposure;
    
    // ACES filmic tonemapping
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    color = clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
    
    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));
    
    outColor = vec4(color, u_Alpha);
}
