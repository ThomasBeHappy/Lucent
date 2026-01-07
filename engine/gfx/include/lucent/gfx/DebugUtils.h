#pragma once

#include <vulkan/vulkan.h>
#include "lucent/core/Core.h"

namespace lucent::gfx {

class DebugUtils {
public:
    // Object naming for RenderDoc/validation
    static void SetObjectName(VkDevice device, uint64_t object, VkObjectType type, const char* name);
    
    template<typename T>
    static void SetObjectName(VkDevice device, T object, VkObjectType type, const char* name) {
        SetObjectName(device, reinterpret_cast<uint64_t>(object), type, name);
    }
    
    // Command buffer debug regions
    static void BeginLabel(VkCommandBuffer cmd, const char* name, float r = 1.0f, float g = 1.0f, float b = 1.0f);
    static void EndLabel(VkCommandBuffer cmd);
    static void InsertLabel(VkCommandBuffer cmd, const char* name, float r = 1.0f, float g = 1.0f, float b = 1.0f);
    
    // Queue debug regions
    static void BeginQueueLabel(VkQueue queue, const char* name, float r = 1.0f, float g = 1.0f, float b = 1.0f);
    static void EndQueueLabel(VkQueue queue);
    
    // Initialize function pointers (call after device creation)
    static void Init(VkInstance instance);
    
private:
    static PFN_vkSetDebugUtilsObjectNameEXT s_vkSetDebugUtilsObjectNameEXT;
    static PFN_vkCmdBeginDebugUtilsLabelEXT s_vkCmdBeginDebugUtilsLabelEXT;
    static PFN_vkCmdEndDebugUtilsLabelEXT s_vkCmdEndDebugUtilsLabelEXT;
    static PFN_vkCmdInsertDebugUtilsLabelEXT s_vkCmdInsertDebugUtilsLabelEXT;
    static PFN_vkQueueBeginDebugUtilsLabelEXT s_vkQueueBeginDebugUtilsLabelEXT;
    static PFN_vkQueueEndDebugUtilsLabelEXT s_vkQueueEndDebugUtilsLabelEXT;
};

// RAII helper for debug regions
class ScopedDebugLabel {
public:
    ScopedDebugLabel(VkCommandBuffer cmd, const char* name, float r = 1.0f, float g = 1.0f, float b = 1.0f)
        : m_Cmd(cmd) {
        DebugUtils::BeginLabel(cmd, name, r, g, b);
    }
    
    ~ScopedDebugLabel() {
        DebugUtils::EndLabel(m_Cmd);
    }
    
private:
    VkCommandBuffer m_Cmd;
};

#if LUCENT_DEBUG
    #define LUCENT_GPU_SCOPE(cmd, name) ::lucent::gfx::ScopedDebugLabel _debug_label_##__LINE__(cmd, name)
    #define LUCENT_GPU_SCOPE_COLOR(cmd, name, r, g, b) ::lucent::gfx::ScopedDebugLabel _debug_label_##__LINE__(cmd, name, r, g, b)
#else
    #define LUCENT_GPU_SCOPE(cmd, name)
    #define LUCENT_GPU_SCOPE_COLOR(cmd, name, r, g, b)
#endif

} // namespace lucent::gfx

