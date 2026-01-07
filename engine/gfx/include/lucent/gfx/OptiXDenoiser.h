#pragma once

#ifdef LUCENT_ENABLE_OPTIX

#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Image.h"
#include <glm/glm.hpp>
#include <memory>
#include <cuda.h>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stubs.h>

namespace lucent::gfx {

// Vulkan-CUDA shared image wrapper
struct CudaVulkanImage {
    VkImage vkImage = VK_NULL_HANDLE;
    VkDeviceMemory vkMemory = VK_NULL_HANDLE;
    VkImageView vkView = VK_NULL_HANDLE;
    cudaExternalMemory_t cudaExtMem = nullptr;
    cudaMipmappedArray_t cudaMipArray = nullptr;
    cudaArray_t cudaArray = nullptr;
    cudaSurfaceObject_t cudaSurface = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// OptiX AI Denoiser with AOV support (albedo + normal guides)
class OptiXDenoiser {
public:
    OptiXDenoiser() = default;
    ~OptiXDenoiser();

    // Initialize OptiX context and denoiser
    bool Init(VulkanContext* context, Device* device);
    void Shutdown();

    // Check if OptiX is available and initialized
    bool IsAvailable() const { return m_Initialized; }
    
    // Check if denoise was performed this frame (skip normal blit if true)
    bool WasDenoisePerformed() const { return m_DenoisePerformed; }
    void ResetDenoiseFlag() { m_DenoisePerformed = false; }

    // Resize denoiser buffers (call when viewport size changes)
    bool Resize(uint32_t width, uint32_t height);

    // Denoise the input image using albedo and normal guides
    // All images must be RGBA32F format
    // inputColor: noisy path traced color
    // inputAlbedo: surface albedo (diffuse color without lighting)
    // inputNormal: world-space normals (normalized, in [-1,1] range)
    // output: denoised result
    bool Denoise(
        Image* inputColor,
        Image* inputAlbedo,
        Image* inputNormal,
        Image* output,
        VkCommandBuffer cmd,
        VkSemaphore waitSemaphore,
        VkSemaphore signalSemaphore
    );

    // Get intensity (blend factor for temporal stability)
    float GetIntensity() const { return m_Intensity; }
    void SetIntensity(float intensity) { m_Intensity = intensity; }

private:
    bool InitCuda();
    bool InitOptix();
    bool CreateDenoiser();
    
    // Create Vulkan image with external memory for CUDA interop
    bool CreateSharedImage(CudaVulkanImage& img, uint32_t width, uint32_t height, const char* debugName);
    void DestroySharedImage(CudaVulkanImage& img);
    
    // GPU-only image layout transitions
    void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    
    // Resize shared Vulkan-CUDA images
    void ResizeSharedImages(uint32_t width, uint32_t height);

private:
    VulkanContext* m_Context = nullptr;
    Device* m_Device = nullptr;
    bool m_Initialized = false;
    bool m_DenoisePerformed = false;
    
    // CUDA context
    CUcontext m_CudaContext = nullptr;
    CUdevice m_CudaDevice = 0;
    cudaStream_t m_CudaStream = nullptr;
    
    // OptiX
    OptixDeviceContext m_OptixContext = nullptr;
    OptixDenoiser m_Denoiser = nullptr;
    OptixDenoiserSizes m_DenoiserSizes{};
    
    // Denoiser GPU buffers
    CUdeviceptr m_StateBuffer = 0;
    CUdeviceptr m_ScratchBuffer = 0;
    CUdeviceptr m_ColorBuffer = 0;      // Input noisy color
    CUdeviceptr m_AlbedoBuffer = 0;     // Albedo AOV
    CUdeviceptr m_NormalBuffer = 0;     // Normal AOV
    CUdeviceptr m_OutputBuffer = 0;     // Denoised output
    CUdeviceptr m_IntensityBuffer = 0;  // For HDR intensity
    
    // Shared images for Vulkan-CUDA interop
    CudaVulkanImage m_SharedColor;
    CudaVulkanImage m_SharedAlbedo;
    CudaVulkanImage m_SharedNormal;
    CudaVulkanImage m_SharedOutput;
    
    
    // Synchronization
    cudaExternalSemaphore_t m_CudaWaitSemaphore = nullptr;
    cudaExternalSemaphore_t m_CudaSignalSemaphore = nullptr;
    VkSemaphore m_VkWaitSemaphore = VK_NULL_HANDLE;
    VkSemaphore m_VkSignalSemaphore = VK_NULL_HANDLE;
    
    // Denoiser settings
    float m_Intensity = 1.0f;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
};

} // namespace lucent::gfx

#endif // LUCENT_ENABLE_OPTIX


