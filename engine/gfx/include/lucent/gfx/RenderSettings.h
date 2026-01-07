#pragma once

#include "lucent/gfx/RenderCapabilities.h"
#include <cstdint>

namespace lucent::gfx {

// Tonemapping operators
enum class TonemapOperator : uint8_t {
    None = 0,       // Linear clamp
    Reinhard,       // Classic Reinhard
    ACES,           // ACES filmic
    Uncharted2,     // Filmic (Uncharted 2)
    AgX             // Neutral/AgX
};

inline const char* TonemapOperatorName(TonemapOperator op) {
    switch (op) {
        case TonemapOperator::None:       return "None";
        case TonemapOperator::Reinhard:   return "Reinhard";
        case TonemapOperator::ACES:       return "ACES";
        case TonemapOperator::Uncharted2: return "Uncharted 2";
        case TonemapOperator::AgX:        return "AgX";
        default:                          return "Unknown";
    }
}

// Denoiser backends (viewport/final render)
enum class DenoiserType : uint8_t {
    None = 0,
    Box,
    EdgeAware,
    OpenImageDenoise,
    OptiX,
    NRD
};

inline const char* DenoiserTypeName(DenoiserType type) {
    switch (type) {
        case DenoiserType::None:             return "None";
        case DenoiserType::Box:              return "Box Blur";
        case DenoiserType::EdgeAware:        return "Edge-Aware";
        case DenoiserType::OpenImageDenoise: return "OpenImageDenoise";
        case DenoiserType::OptiX:            return "OptiX";
        case DenoiserType::NRD:              return "NRD";
        default:                             return "Unknown";
    }
}

// Blender-like render settings shared by all render modes
struct RenderSettings {
    // === Sampling ===
    uint32_t viewportSamples = 32;      // Max samples for viewport (progressive)
    uint32_t finalSamples = 128;        // Samples for final render
    uint32_t minSamples = 1;            // Minimum samples before converge check
    
    // === Bounces ===
    uint32_t maxBounces = 4;            // Total max bounces
    uint32_t diffuseBounces = 4;        // Max diffuse bounces
    uint32_t specularBounces = 4;       // Max specular/glossy bounces
    uint32_t transmissionBounces = 8;   // Max transmission/refraction bounces
    
    // === Clamping ===
    float clampDirect = 0.0f;           // Clamp direct lighting (0 = no clamp)
    float clampIndirect = 10.0f;        // Clamp indirect lighting
    
    // === Film / Color ===
    float exposure = 1.0f;
    TonemapOperator tonemapOperator = TonemapOperator::ACES;
    float gamma = 2.2f;
    
    // === Denoise ===
    DenoiserType denoiser = DenoiserType::None;
    float denoiseStrength = 0.5f;
    uint32_t denoiseRadius = 2;
    
    // === Performance ===
    bool useHalfRes = false;            // Render at half resolution for viewport
    uint32_t tileSize = 256;            // Tile size for final render
    float maxFrameTimeMs = 16.67f;      // Budget for progressive passes (60fps = 16.67ms)
    
    // === Shadows (Simple mode specific) ===
    bool enableShadows = true;
    float shadowBias = 0.005f;
    uint32_t shadowMapSize = 2048;
    
    // === Mode-specific flags ===
    RenderMode activeMode = RenderMode::Simple;
    
    // Reset samples (for accumulation)
    bool needsReset = false;
    
    // Frame counter for accumulation
    uint32_t accumulatedSamples = 0;
    
    // Mark that settings changed (resets accumulation)
    void MarkDirty() { 
        needsReset = true; 
        accumulatedSamples = 0;
    }
    
    // Consume the reset flag
    bool ConsumeReset() {
        if (needsReset) {
            needsReset = false;
            return true;
        }
        return false;
    }
    
    // Check if we're done accumulating samples
    bool IsConverged() const {
        if (activeMode == RenderMode::Simple) return true;
        return accumulatedSamples >= viewportSamples;
    }
    
    // Increment sample count after a pass
    void IncrementSamples(uint32_t count = 1) {
        accumulatedSamples += count;
    }
};

} // namespace lucent::gfx

