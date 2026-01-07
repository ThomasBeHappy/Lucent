#include "lucent/gfx/RenderCapabilities.h"
#include "lucent/gfx/VulkanContext.h"
#include "lucent/core/Log.h"

namespace lucent::gfx {

RenderCapabilities RenderCapabilities::FromDeviceFeatures(const DeviceFeatures& features, uint32_t vulkanVersion) {
    RenderCapabilities caps;
    caps.vulkanVersion = vulkanVersion;
    
    // Copy feature flags
    caps.hasDynamicRendering = features.dynamicRendering;
    caps.hasSynchronization2 = features.synchronization2;
    caps.hasBufferDeviceAddress = features.bufferDeviceAddress;
    caps.hasRayTracingPipeline = features.rayTracingPipeline;
    caps.hasAccelerationStructure = features.accelerationStructure;
    
    // Copy RT properties
    caps.maxRayRecursionDepth = features.maxRayRecursionDepth;
    caps.shaderGroupHandleSize = features.shaderGroupHandleSize;
    caps.shaderGroupBaseAlignment = features.shaderGroupBaseAlignment;
    
    // Compute and storage are core features in Vulkan 1.0+
    // Image load/store is also core. These are always available on any Vulkan GPU.
    caps.hasCompute = true;
    caps.hasStorageBuffers = true;
    caps.hasImageLoadStore = true;
    
    // Mode availability
    // Simple: always available
    caps.simpleAvailable = true;
    
    // Traced: requires compute + storage buffers + image load/store
    // These are Vulkan 1.0 core features, so Traced is almost always available
    caps.tracedAvailable = caps.hasCompute && caps.hasStorageBuffers && caps.hasImageLoadStore;
    
    // RayTraced: requires full KHR ray tracing stack
    caps.rayTracedAvailable = features.rayTracingPipeline && 
                              features.accelerationStructure &&
                              features.bufferDeviceAddress;
    
    // Log capabilities
    LUCENT_CORE_INFO("Render Capabilities:");
    LUCENT_CORE_INFO("  Vulkan Version: {}.{}.{}", 
        VK_VERSION_MAJOR(vulkanVersion), 
        VK_VERSION_MINOR(vulkanVersion), 
        VK_VERSION_PATCH(vulkanVersion));
    LUCENT_CORE_INFO("  Simple Mode: {}", caps.simpleAvailable ? "available" : "unavailable");
    LUCENT_CORE_INFO("  Traced Mode: {}", caps.tracedAvailable ? "available" : "unavailable");
    LUCENT_CORE_INFO("  RayTraced Mode: {}", caps.rayTracedAvailable ? "available" : "unavailable");
    
    if (caps.rayTracedAvailable) {
        LUCENT_CORE_INFO("  RT Max Recursion: {}", caps.maxRayRecursionDepth);
    }
    
    return caps;
}

} // namespace lucent::gfx

