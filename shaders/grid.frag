#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec4 outColor;

float gridLine(float coord, float lineWidth) {
    float derivative = fwidth(coord);
    float grid = abs(fract(coord - 0.5) - 0.5) / derivative;
    return 1.0 - min(grid, 1.0);
}

void main() {
    // Grid lines
    float lineWidth = 0.02;
    
    // Small grid (every 1 unit)
    float smallGridX = gridLine(inWorldPos.x, lineWidth);
    float smallGridZ = gridLine(inWorldPos.z, lineWidth);
    float smallGrid = max(smallGridX, smallGridZ);
    
    // Large grid (every 10 units)
    float largeGridX = gridLine(inWorldPos.x / 10.0, lineWidth * 2.0);
    float largeGridZ = gridLine(inWorldPos.z / 10.0, lineWidth * 2.0);
    float largeGrid = max(largeGridX, largeGridZ);
    
    // Axis lines (X = red, Z = blue)
    float axisWidth = 0.05;
    float xAxis = 1.0 - smoothstep(0.0, axisWidth, abs(inWorldPos.z));
    float zAxis = 1.0 - smoothstep(0.0, axisWidth, abs(inWorldPos.x));
    
    // Base grid color (gray)
    vec3 gridColor = vec3(0.3) * smallGrid + vec3(0.5) * largeGrid;
    
    // Add colored axes
    gridColor = mix(gridColor, vec3(0.8, 0.2, 0.2), xAxis);  // X axis red
    gridColor = mix(gridColor, vec3(0.2, 0.2, 0.8), zAxis);  // Z axis blue
    
    // Fade out at distance
    float dist = length(inWorldPos.xz);
    float fade = 1.0 - smoothstep(20.0, 80.0, dist);
    
    // Background color
    vec3 bgColor = vec3(0.08, 0.08, 0.1);
    
    // Combine
    float alpha = max(smallGrid, max(largeGrid, max(xAxis, zAxis))) * fade;
    outColor = vec4(mix(bgColor, gridColor, alpha), 1.0);
}

