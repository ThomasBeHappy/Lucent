#include "lucent/gfx/DebugUtils.h"

namespace lucent::gfx {

PFN_vkSetDebugUtilsObjectNameEXT DebugUtils::s_vkSetDebugUtilsObjectNameEXT = nullptr;
PFN_vkCmdBeginDebugUtilsLabelEXT DebugUtils::s_vkCmdBeginDebugUtilsLabelEXT = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT DebugUtils::s_vkCmdEndDebugUtilsLabelEXT = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT DebugUtils::s_vkCmdInsertDebugUtilsLabelEXT = nullptr;
PFN_vkQueueBeginDebugUtilsLabelEXT DebugUtils::s_vkQueueBeginDebugUtilsLabelEXT = nullptr;
PFN_vkQueueEndDebugUtilsLabelEXT DebugUtils::s_vkQueueEndDebugUtilsLabelEXT = nullptr;

void DebugUtils::Init(VkInstance instance) {
    s_vkSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)
        vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT");
    s_vkCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)
        vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
    s_vkCmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT)
        vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
    s_vkCmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT)
        vkGetInstanceProcAddr(instance, "vkCmdInsertDebugUtilsLabelEXT");
    s_vkQueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT)
        vkGetInstanceProcAddr(instance, "vkQueueBeginDebugUtilsLabelEXT");
    s_vkQueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT)
        vkGetInstanceProcAddr(instance, "vkQueueEndDebugUtilsLabelEXT");
}

void DebugUtils::SetObjectName(VkDevice device, uint64_t object, VkObjectType type, const char* name) {
    if (!s_vkSetDebugUtilsObjectNameEXT || !name) return;
    
    VkDebugUtilsObjectNameInfoEXT nameInfo{};
    nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    nameInfo.objectType = type;
    nameInfo.objectHandle = object;
    nameInfo.pObjectName = name;
    
    s_vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
}

void DebugUtils::BeginLabel(VkCommandBuffer cmd, const char* name, float r, float g, float b) {
    if (!s_vkCmdBeginDebugUtilsLabelEXT) return;
    
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = r;
    label.color[1] = g;
    label.color[2] = b;
    label.color[3] = 1.0f;
    
    s_vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void DebugUtils::EndLabel(VkCommandBuffer cmd) {
    if (!s_vkCmdEndDebugUtilsLabelEXT) return;
    s_vkCmdEndDebugUtilsLabelEXT(cmd);
}

void DebugUtils::InsertLabel(VkCommandBuffer cmd, const char* name, float r, float g, float b) {
    if (!s_vkCmdInsertDebugUtilsLabelEXT) return;
    
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = r;
    label.color[1] = g;
    label.color[2] = b;
    label.color[3] = 1.0f;
    
    s_vkCmdInsertDebugUtilsLabelEXT(cmd, &label);
}

void DebugUtils::BeginQueueLabel(VkQueue queue, const char* name, float r, float g, float b) {
    if (!s_vkQueueBeginDebugUtilsLabelEXT) return;
    
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = r;
    label.color[1] = g;
    label.color[2] = b;
    label.color[3] = 1.0f;
    
    s_vkQueueBeginDebugUtilsLabelEXT(queue, &label);
}

void DebugUtils::EndQueueLabel(VkQueue queue) {
    if (!s_vkQueueEndDebugUtilsLabelEXT) return;
    s_vkQueueEndDebugUtilsLabelEXT(queue);
}

} // namespace lucent::gfx

