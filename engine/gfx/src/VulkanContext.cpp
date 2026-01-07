#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Swapchain.h"
#include "lucent/gfx/VkResultUtils.h"
#include <set>
#include <algorithm>

namespace lucent::gfx {

// Minimum required device extensions (Vulkan 1.1 compatible)
static const std::vector<const char*> s_RequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// Optional Vulkan 1.3 extensions (enables better performance/features when available)
static const std::vector<const char*> s_Vulkan13Extensions = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
};

// Optional RT extensions
static const std::vector<const char*> s_RayTracingExtensions = {
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
};

// Validation layers
static const std::vector<const char*> s_ValidationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

VulkanContext::~VulkanContext() {
    Shutdown();
}

bool VulkanContext::Init(const VulkanContextConfig& config, GLFWwindow* window) {
    m_ValidationEnabled = config.enableValidation;
    
    if (!CreateInstance(config)) {
        LUCENT_CORE_ERROR("Failed to create Vulkan instance");
        return false;
    }
    
    if (m_ValidationEnabled && !SetupDebugMessenger()) {
        LUCENT_CORE_WARN("Failed to setup debug messenger");
    }
    
    if (!CreateSurface(window)) {
        LUCENT_CORE_ERROR("Failed to create window surface");
        return false;
    }
    
    if (!SelectPhysicalDevice(config)) {
        LUCENT_CORE_ERROR("Failed to find suitable GPU");
        return false;
    }
    
    if (!CreateLogicalDevice(config)) {
        LUCENT_CORE_ERROR("Failed to create logical device");
        return false;
    }
    
    LUCENT_CORE_INFO("Vulkan context initialized successfully");
    return true;
}

void VulkanContext::Shutdown() {
    if (m_Device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    
    if (m_Surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    
    if (m_DebugMessenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            m_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(m_Instance, m_DebugMessenger, nullptr);
        }
        m_DebugMessenger = VK_NULL_HANDLE;
    }
    
    if (m_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
}

void VulkanContext::WaitIdle() const {
    if (m_Device != VK_NULL_HANDLE) {
        VkResult res = vkDeviceWaitIdle(m_Device);
        if (res != VK_SUCCESS) {
            LUCENT_CORE_ERROR("vkDeviceWaitIdle failed: {} ({})", VkResultToString(res), static_cast<int>(res));
        }
    }
}

bool VulkanContext::CreateInstance(const VulkanContextConfig& config) {
    // Check validation layer support
    if (m_ValidationEnabled) {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        
        for (const char* layerName : s_ValidationLayers) {
            bool found = false;
            for (const auto& layer : availableLayers) {
                if (strcmp(layerName, layer.layerName) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                LUCENT_CORE_WARN("Validation layer {} not available", layerName);
                m_ValidationEnabled = false;
                break;
            }
        }
    }
    
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = config.appName;
    appInfo.applicationVersion = config.appVersion;
    appInfo.pEngineName = "Lucent";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;
    
    // Get required extensions
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
    
    if (m_ValidationEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (m_ValidationEnabled) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(s_ValidationLayers.size());
        createInfo.ppEnabledLayerNames = s_ValidationLayers.data();
        
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = DebugCallback;
        createInfo.pNext = &debugCreateInfo;
    }
    
    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (result != VK_SUCCESS) {
        LUCENT_CORE_ERROR("vkCreateInstance failed: {}", static_cast<int>(result));
        return false;
    }
    
    LUCENT_CORE_INFO("Vulkan instance created (API 1.3)");
    return true;
}

bool VulkanContext::SetupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugCallback;
    
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        m_Instance, "vkCreateDebugUtilsMessengerEXT");
    
    if (!func || func(m_Instance, &createInfo, nullptr, &m_DebugMessenger) != VK_SUCCESS) {
        return false;
    }
    
    return true;
}

bool VulkanContext::CreateSurface(GLFWwindow* window) {
    VkResult result = glfwCreateWindowSurface(m_Instance, window, nullptr, &m_Surface);
    return result == VK_SUCCESS;
}

bool VulkanContext::SelectPhysicalDevice(const VulkanContextConfig& config) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    
    if (deviceCount == 0) {
        LUCENT_CORE_ERROR("No GPUs with Vulkan support found");
        return false;
    }
    
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());
    
    // Rate and select best device
    int bestScore = -1;
    bool foundPreferred = false;
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);

        int score = RateDeviceSuitability(device, config);
        if (score < 0) {
            LUCENT_CORE_DEBUG("GPU candidate rejected: {}", props.deviceName);
            continue;
        }

        bool preferredMatch = false;
        if (config.preferredDeviceName && config.preferredDeviceName[0] != '\0') {
            std::string hay = props.deviceName;
            std::string needle = config.preferredDeviceName;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
            preferredMatch = (hay.find(needle) != std::string::npos);
            if (preferredMatch) {
                score += 100000; // force win among suitable devices
                foundPreferred = true;
            } else if (!foundPreferred) {
                // keep score as-is until we find a preferred match
            } else {
                // if a preferred match exists, strongly deprioritize non-matching devices
                score -= 100000;
            }
        }

        LUCENT_CORE_INFO("GPU candidate: {} (type={}, score={}, preferredMatch={})",
            props.deviceName,
            (int)props.deviceType,
            score,
            preferredMatch ? "true" : "false");

        if (score > bestScore) {
            bestScore = score;
            m_PhysicalDevice = device;
        }
    }
    
    if (m_PhysicalDevice == VK_NULL_HANDLE) {
        LUCENT_CORE_ERROR("Failed to find a suitable GPU");
        return false;
    }
    
    // Get device properties and features
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &props);
    LUCENT_CORE_INFO("Selected GPU: {}", props.deviceName);
    LUCENT_CORE_INFO("  Driver Version: {}.{}.{}", 
        VK_VERSION_MAJOR(props.driverVersion),
        VK_VERSION_MINOR(props.driverVersion),
        VK_VERSION_PATCH(props.driverVersion));
    
    // Query and store device features
    m_QueueFamilies = FindQueueFamilies(m_PhysicalDevice);
    QueryDeviceFeatures(m_PhysicalDevice, m_DeviceFeatures);
    
    if (m_DeviceFeatures.rayTracingPipeline) {
        LUCENT_CORE_INFO("  Ray Tracing: SUPPORTED (max recursion: {})", 
            m_DeviceFeatures.maxRayRecursionDepth);
    } else {
        LUCENT_CORE_WARN("  Ray Tracing: NOT SUPPORTED");
    }
    
    return true;
}

bool VulkanContext::CreateLogicalDevice(const VulkanContextConfig& config) {
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {
        m_QueueFamilies.graphics,
        m_QueueFamilies.present
    };
    
    if (m_QueueFamilies.compute != UINT32_MAX) {
        uniqueQueueFamilies.insert(m_QueueFamilies.compute);
    }
    
    if (m_QueueFamilies.transfer != UINT32_MAX) {
        uniqueQueueFamilies.insert(m_QueueFamilies.transfer);
    }
    
    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }
    
    // Collect extensions
    std::vector<const char*> deviceExtensions = s_RequiredDeviceExtensions;
    
    // Check if Vulkan 1.3 extensions are available
    bool hasVulkan13Exts = CheckDeviceExtensionSupport(m_PhysicalDevice, s_Vulkan13Extensions);
    if (hasVulkan13Exts) {
        for (const char* ext : s_Vulkan13Extensions) {
            deviceExtensions.push_back(ext);
        }
        LUCENT_CORE_INFO("  Vulkan 1.3 features: ENABLED");
    } else {
        LUCENT_CORE_WARN("  Vulkan 1.3 features: NOT AVAILABLE (using fallback)");
        // Update device features to reflect what's actually available
        m_DeviceFeatures.dynamicRendering = false;
        m_DeviceFeatures.synchronization2 = false;
        m_DeviceFeatures.maintenance4 = false;
        m_DeviceFeatures.bufferDeviceAddress = false;
    }
    
    // Add RT extensions if supported and requested
    bool enableRT = config.enableRayTracing && m_DeviceFeatures.rayTracingPipeline && hasVulkan13Exts;
    if (enableRT) {
        for (const char* ext : s_RayTracingExtensions) {
            deviceExtensions.push_back(ext);
        }
    }
    
    // Build feature chain
    VkPhysicalDeviceFeatures2 deviceFeatures2{};
    deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures2.features.fillModeNonSolid = VK_TRUE;  // Enable wireframe mode
    
    // Enable robust buffer access when supported (helps prevent GPU hangs on OOB in shaders)
    VkPhysicalDeviceFeatures coreFeatures{};
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &coreFeatures);
    if (coreFeatures.robustBufferAccess) {
        deviceFeatures2.features.robustBufferAccess = VK_TRUE;
        LUCENT_CORE_INFO("  robustBufferAccess: ENABLED");
    } else {
        LUCENT_CORE_WARN("  robustBufferAccess: NOT AVAILABLE");
    }
    
    // Vulkan 1.2 features - only request if device supports them
    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    if (hasVulkan13Exts) {
        vulkan12Features.bufferDeviceAddress = VK_TRUE;
    }
    vulkan12Features.descriptorIndexing = VK_TRUE;
    vulkan12Features.runtimeDescriptorArray = VK_TRUE;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    // Required for our RT shaders using `layout(scalar)` storage buffers with vec3 arrays
    vulkan12Features.scalarBlockLayout = VK_TRUE;
    vulkan12Features.timelineSemaphore = VK_TRUE;
    
    deviceFeatures2.pNext = &vulkan12Features;
    
    // Vulkan 1.3 features - only include in chain if available
    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    void** pNextChainTail = &vulkan12Features.pNext;
    
    if (hasVulkan13Exts) {
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.dynamicRendering = VK_TRUE;
        vulkan13Features.synchronization2 = VK_TRUE;
        vulkan13Features.maintenance4 = VK_TRUE;
        
        *pNextChainTail = &vulkan13Features;
        pNextChainTail = &vulkan13Features.pNext;
    }
    
    // RT features
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    
    if (enableRT) {
        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
        
        asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asFeatures.accelerationStructure = VK_TRUE;
        
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rayQueryFeatures.rayQuery = VK_TRUE;
        
        *pNextChainTail = &rtPipelineFeatures;
        rtPipelineFeatures.pNext = &asFeatures;
        asFeatures.pNext = &rayQueryFeatures;
    }
    
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &deviceFeatures2;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
    
    // Deprecated but required for compatibility
    if (m_ValidationEnabled) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(s_ValidationLayers.size());
        createInfo.ppEnabledLayerNames = s_ValidationLayers.data();
    }
    
    VkResult result = vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device);
    if (result != VK_SUCCESS) {
        LUCENT_CORE_ERROR("vkCreateDevice failed: {}", static_cast<int>(result));
        return false;
    }
    
    // Get queue handles
    vkGetDeviceQueue(m_Device, m_QueueFamilies.graphics, 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, m_QueueFamilies.present, 0, &m_PresentQueue);
    
    if (m_QueueFamilies.compute != UINT32_MAX) {
        vkGetDeviceQueue(m_Device, m_QueueFamilies.compute, 0, &m_ComputeQueue);
    } else {
        m_ComputeQueue = m_GraphicsQueue;
    }
    
    if (m_QueueFamilies.transfer != UINT32_MAX) {
        vkGetDeviceQueue(m_Device, m_QueueFamilies.transfer, 0, &m_TransferQueue);
    } else {
        m_TransferQueue = m_GraphicsQueue;
    }
    
    LUCENT_CORE_INFO("Logical device created");
    return true;
}

QueueFamilyIndices VulkanContext::FindQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;
    
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
    
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const auto& queueFamily = queueFamilies[i];
        
        // Graphics queue
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = i;
        }
        
        // Dedicated compute queue (prefer separate from graphics)
        if ((queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) && 
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            indices.compute = i;
        }
        
        // Transfer queue
        if ((queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            indices.transfer = i;
        }
        
        // Present support
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_Surface, &presentSupport);
        if (presentSupport) {
            indices.present = i;
        }
    }
    
    // Fallback: use graphics queue for compute if no dedicated
    if (indices.compute == UINT32_MAX && indices.graphics != UINT32_MAX) {
        indices.compute = indices.graphics;
    }
    
    return indices;
}

bool VulkanContext::CheckDeviceExtensionSupport(VkPhysicalDevice device, 
                                                 const std::vector<const char*>& extensions) const {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());
    
    std::set<std::string> requiredExtensions(extensions.begin(), extensions.end());
    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    
    return requiredExtensions.empty();
}

void VulkanContext::QueryDeviceFeatures(VkPhysicalDevice device, DeviceFeatures& features) const {
    // Query Vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    
    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan12Features.pNext = &vulkan13Features;
    
    // RT features
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
    rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
    asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    
    // Check if RT extensions are available
    bool hasRTExtensions = CheckDeviceExtensionSupport(device, s_RayTracingExtensions);
    if (hasRTExtensions) {
        vulkan13Features.pNext = &rtPipelineFeatures;
        rtPipelineFeatures.pNext = &asFeatures;
        asFeatures.pNext = &rayQueryFeatures;
    }
    
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &vulkan12Features;
    
    vkGetPhysicalDeviceFeatures2(device, &features2);
    
    features.bufferDeviceAddress = vulkan12Features.bufferDeviceAddress;
    features.descriptorIndexing = vulkan12Features.descriptorIndexing;
    features.dynamicRendering = vulkan13Features.dynamicRendering;
    features.synchronization2 = vulkan13Features.synchronization2;
    features.maintenance4 = vulkan13Features.maintenance4;
    
    if (hasRTExtensions) {
        features.rayTracingPipeline = rtPipelineFeatures.rayTracingPipeline;
        features.accelerationStructure = asFeatures.accelerationStructure;
        features.rayQuery = rayQueryFeatures.rayQuery;
        
        // Query RT properties
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
        rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &rtProps;
        
        vkGetPhysicalDeviceProperties2(device, &props2);
        
        features.maxRayRecursionDepth = rtProps.maxRayRecursionDepth;
        features.shaderGroupHandleSize = rtProps.shaderGroupHandleSize;
        features.shaderGroupBaseAlignment = rtProps.shaderGroupBaseAlignment;
    }
}

int VulkanContext::RateDeviceSuitability(VkPhysicalDevice device, const VulkanContextConfig& config) const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    
    int score = 0;
    
    // Discrete GPUs are preferred
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    }
    
    // Check required extensions
    if (!CheckDeviceExtensionSupport(device, s_RequiredDeviceExtensions)) {
        LUCENT_CORE_WARN("GPU '{}' rejected: missing required device extensions", props.deviceName);
        return -1;
    }
    
    // Check queue families
    QueueFamilyIndices indices = FindQueueFamilies(device);
    if (!indices.IsComplete()) {
        LUCENT_CORE_WARN("GPU '{}' rejected: missing required queue families (graphics/present)", props.deviceName);
        return -1;
    }
    
    // Check swapchain support
    SwapchainSupportDetails swapchainSupport = Swapchain::QuerySupport(device, m_Surface);
    if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty()) {
        LUCENT_CORE_WARN("GPU '{}' rejected: insufficient swapchain support (formats or present modes empty)", props.deviceName);
        return -1;
    }
    
    // Bonus for Vulkan 1.3 features (dynamic rendering, sync2)
    if (CheckDeviceExtensionSupport(device, s_Vulkan13Extensions)) {
        score += 200;
        LUCENT_CORE_DEBUG("GPU '{}' has Vulkan 1.3 extensions", props.deviceName);
    } else {
        LUCENT_CORE_DEBUG("GPU '{}' will use Vulkan 1.1/1.2 fallback path", props.deviceName);
    }
    
    // Bonus for RT support
    if (config.enableRayTracing && CheckDeviceExtensionSupport(device, s_RayTracingExtensions)) {
        score += 500;
    }
    
    // Bonus for dedicated compute queue
    if (indices.compute != indices.graphics) {
        score += 100;
    }
    
    // Max image dimension affects quality
    score += props.limits.maxImageDimension2D / 1000;
    
    return score;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData) 
{
    (void)type;
    (void)userData;
    
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LUCENT_CORE_ERROR("[Vulkan] {}", callbackData->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LUCENT_CORE_WARN("[Vulkan] {}", callbackData->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LUCENT_CORE_DEBUG("[Vulkan] {}", callbackData->pMessage);
    }
    
    return VK_FALSE;
}

} // namespace lucent::gfx

