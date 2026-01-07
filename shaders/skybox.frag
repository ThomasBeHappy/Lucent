#version 450

layout(location = 0) in vec3 inDirection;
layout(location = 0) out vec4 outColor;

// Procedural sky gradient (no texture needed for basic version)
// Later can be replaced with HDR environment map

const vec3 skyColorTop = vec3(0.3, 0.5, 0.9);      // Blue sky
const vec3 skyColorHorizon = vec3(0.7, 0.8, 0.95); // Light horizon
const vec3 groundColor = vec3(0.2, 0.15, 0.1);     // Brown ground

// Sun parameters
const vec3 sunDir = normalize(vec3(1.0, 0.4, 0.5));
const vec3 sunColor = vec3(1.0, 0.95, 0.85);
const float sunSize = 0.005;
const float sunGlow = 0.1;

void main() {
    vec3 dir = normalize(inDirection);
    
    // Vertical gradient (sky to ground)
    float t = dir.y * 0.5 + 0.5;
    
    vec3 skyColor;
    if (dir.y > 0.0) {
        // Sky gradient (horizon to zenith)
        skyColor = mix(skyColorHorizon, skyColorTop, pow(t, 0.5));
    } else {
        // Ground gradient
        skyColor = mix(skyColorHorizon, groundColor, pow(-dir.y, 0.5));
    }
    
    // Sun disc
    float sunDot = dot(dir, sunDir);
    if (sunDot > 0.0) {
        // Sun disc
        float sun = smoothstep(1.0 - sunSize, 1.0, sunDot);
        skyColor = mix(skyColor, sunColor * 3.0, sun);
        
        // Sun glow
        float glow = pow(max(sunDot, 0.0), 32.0) * sunGlow;
        skyColor += sunColor * glow;
    }
    
    // Simple tonemap for sky
    skyColor = skyColor / (skyColor + vec3(1.0));
    
    // Gamma
    skyColor = pow(skyColor, vec3(1.0 / 2.2));
    
    outColor = vec4(skyColor, 1.0);
}

