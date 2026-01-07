#pragma once

#include <cstdint>

namespace lucent::gfx {

// Forward declaration
struct DeviceFeatures;

// Available render modes
enum class RenderMode : uint8_t {
    Simple = 0,     // Fast raster PBR + shadows (always available)
    Traced,         // GPU compute path tracer with software BVH (Vulkan 1.1+)
    RayTraced       // Vulkan KHR ray tracing pipeline (modern GPUs only)
};

// Get human-readable name for render mode
inline const char* RenderModeName(RenderMode mode) {
    switch (mode) {
        case RenderMode::Simple:    return "Simple";
        case RenderMode::Traced:    return "Traced";
        case RenderMode::RayTraced: return "Ray Traced";
        default:                    return "Unknown";
    }
}

// Capabilities detected at runtime from Vulkan device
struct RenderCapabilities {
    // Vulkan version
    uint32_t vulkanVersion = 0;
    
    // Mode availability
    bool simpleAvailable = true;    // Always true
    bool tracedAvailable = false;   // Requires compute + storage buffers + image load/store
    bool rayTracedAvailable = false; // Requires KHR RT extensions
    
    // Feature details
    bool hasCompute = false;
    bool hasStorageBuffers = false;
    bool hasImageLoadStore = false;
    bool hasDynamicRendering = false;
    bool hasSynchronization2 = false;
    bool hasBufferDeviceAddress = false;
    bool hasRayTracingPipeline = false;
    bool hasAccelerationStructure = false;
    
    // RT properties
    uint32_t maxRayRecursionDepth = 0;
    uint32_t shaderGroupHandleSize = 0;
    uint32_t shaderGroupBaseAlignment = 0;
    
    // Get best available mode
    RenderMode GetBestMode() const {
        if (rayTracedAvailable) return RenderMode::RayTraced;
        if (tracedAvailable) return RenderMode::Traced;
        return RenderMode::Simple;
    }
    
    // Check if a mode is available
    bool IsModeAvailable(RenderMode mode) const {
        switch (mode) {
            case RenderMode::Simple:    return simpleAvailable;
            case RenderMode::Traced:    return tracedAvailable;
            case RenderMode::RayTraced: return rayTracedAvailable;
            default:                    return false;
        }
    }
    
    // Build from DeviceFeatures
    static RenderCapabilities FromDeviceFeatures(const DeviceFeatures& features, uint32_t vulkanVersion);
};

} // namespace lucent::gfx

