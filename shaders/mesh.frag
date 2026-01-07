#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;

layout(location = 0) out vec4 outColor;

// Push constants matching vertex shader
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 viewProj;
    vec4 baseColor;      // RGB + alpha
    vec4 materialParams; // metallic, roughness, emissiveIntensity, unused
    vec4 emissive;       // RGB + unused
    vec4 cameraPos;      // Camera world position
} pc;

// Material parameters from push constants
#define u_BaseColor pc.baseColor.rgb
#define u_Alpha pc.baseColor.a
#define u_Metallic pc.materialParams.x
#define u_Roughness pc.materialParams.y
#define u_EmissiveIntensity pc.materialParams.z
#define u_Emissive pc.emissive.rgb
#define u_CameraPos pc.cameraPos.xyz

// Lighting constants
const vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
const vec3 lightColor = vec3(1.0, 0.98, 0.95);
const float lightIntensity = 2.5;

// Ambient/environment approximation
const vec3 ambientTop = vec3(0.3, 0.35, 0.5);    // Sky color
const vec3 ambientBottom = vec3(0.1, 0.08, 0.05); // Ground color

const float PI = 3.14159265359;

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

void main() {
    vec3 N = normalize(inNormal);
    vec3 V = normalize(u_CameraPos - inWorldPos);
    vec3 L = lightDir;
    vec3 H = normalize(V + L);
    
    // Material properties
    vec3 albedo = u_BaseColor;
    float metallic = u_Metallic;
    float roughness = max(u_Roughness, 0.04); // Prevent div by zero
    
    // Material F0 (dielectric = 0.04, metallic = albedo)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    
    // Cook-Torrance BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;
    
    // Energy conservation
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    
    // Direct lighting
    float NdotL = max(dot(N, L), 0.0);
    vec3 Lo = (kD * albedo / PI + specular) * lightColor * lightIntensity * NdotL;
    
    // Ambient (simple hemisphere + Fresnel for metals)
    vec3 ambient = hemisphereAmbient(N) * albedo * (1.0 - metallic) * 0.3;
    vec3 F_ambient = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    ambient += F_ambient * hemisphereAmbient(reflect(-V, N)) * 0.15;
    
    // Emissive
    vec3 emission = u_Emissive * u_EmissiveIntensity;
    
    // Final color
    vec3 color = ambient + Lo + emission;
    
    // Reinhard tonemap
    color = color / (color + vec3(1.0));
    
    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));
    
    outColor = vec4(color, u_Alpha);
}
