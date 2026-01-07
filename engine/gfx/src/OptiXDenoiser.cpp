#ifdef LUCENT_ENABLE_OPTIX

#include "lucent/gfx/OptiXDenoiser.h"
#include "lucent/core/Log.h"
#include <optix_function_table_definition.h>

// Windows-specific for Vulkan external memory handles
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#endif

namespace lucent::gfx {

// OptiX log callback
static void OptixLogCallback(unsigned int level, const char* tag, const char* message, void* /*cbdata*/) {
    switch (level) {
        case 1: LUCENT_CORE_ERROR("[OptiX][{}] {}", tag, message); break;
        case 2: LUCENT_CORE_ERROR("[OptiX][{}] {}", tag, message); break;
        case 3: LUCENT_CORE_WARN("[OptiX][{}] {}", tag, message); break;
        case 4: LUCENT_CORE_INFO("[OptiX][{}] {}", tag, message); break;
        default: LUCENT_CORE_DEBUG("[OptiX][{}] {}", tag, message); break;
    }
}

OptiXDenoiser::~OptiXDenoiser() {
    Shutdown();
}

bool OptiXDenoiser::Init(VulkanContext* context, Device* device) {
    m_Context = context;
    m_Device = device;
    
    if (!InitCuda()) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to initialize CUDA");
        return false;
    }
    
    if (!InitOptix()) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to initialize OptiX");
        return false;
    }
    
    if (!CreateDenoiser()) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to create denoiser");
        return false;
    }
    
    m_Initialized = true;
    LUCENT_CORE_INFO("OptiXDenoiser: Initialized successfully");
    return true;
}

void OptiXDenoiser::Shutdown() {
    if (!m_Initialized) return;
    
    // Wait for CUDA operations to complete
    if (m_CudaStream) {
        cudaStreamSynchronize(m_CudaStream);
    }
    
    // Free CUDA buffers
    if (m_StateBuffer) cudaFree(reinterpret_cast<void*>(m_StateBuffer));
    if (m_ScratchBuffer) cudaFree(reinterpret_cast<void*>(m_ScratchBuffer));
    if (m_ColorBuffer) cudaFree(reinterpret_cast<void*>(m_ColorBuffer));
    if (m_AlbedoBuffer) cudaFree(reinterpret_cast<void*>(m_AlbedoBuffer));
    if (m_NormalBuffer) cudaFree(reinterpret_cast<void*>(m_NormalBuffer));
    if (m_OutputBuffer) cudaFree(reinterpret_cast<void*>(m_OutputBuffer));
    if (m_IntensityBuffer) cudaFree(reinterpret_cast<void*>(m_IntensityBuffer));
    
    // Destroy shared images
    DestroySharedImage(m_SharedColor);
    DestroySharedImage(m_SharedAlbedo);
    DestroySharedImage(m_SharedNormal);
    DestroySharedImage(m_SharedOutput);
    
    // Destroy external semaphores
    if (m_CudaWaitSemaphore) cudaDestroyExternalSemaphore(m_CudaWaitSemaphore);
    if (m_CudaSignalSemaphore) cudaDestroyExternalSemaphore(m_CudaSignalSemaphore);
    
    // Destroy OptiX denoiser
    if (m_Denoiser) optixDenoiserDestroy(m_Denoiser);
    if (m_OptixContext) optixDeviceContextDestroy(m_OptixContext);
    
    // Destroy CUDA stream and context
    if (m_CudaStream) cudaStreamDestroy(m_CudaStream);
    if (m_CudaContext) cuCtxDestroy(m_CudaContext);
    
    m_Initialized = false;
    LUCENT_CORE_INFO("OptiXDenoiser: Shutdown complete");
}

bool OptiXDenoiser::InitCuda() {
    // Initialize CUDA driver API
    CUresult cuRes = cuInit(0);
    if (cuRes != CUDA_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: cuInit failed with error {}", static_cast<int>(cuRes));
        return false;
    }
    
    // Get CUDA device matching the Vulkan physical device
    // For now, use device 0 (assuming single GPU system)
    cuRes = cuDeviceGet(&m_CudaDevice, 0);
    if (cuRes != CUDA_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: cuDeviceGet failed");
        return false;
    }
    
    // Create CUDA context using the modern API (CUDA 12+)
    CUctxCreateParams ctxParams{};
    cuRes = cuCtxCreate_v4(&m_CudaContext, &ctxParams, 0, m_CudaDevice);
    if (cuRes != CUDA_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: cuCtxCreate failed with error {}", static_cast<int>(cuRes));
        return false;
    }
    
    // Create CUDA stream
    cudaError_t cudaRes = cudaStreamCreate(&m_CudaStream);
    if (cudaRes != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: cudaStreamCreate failed: {}", cudaGetErrorString(cudaRes));
        return false;
    }
    
    // Get device name for logging
    char deviceName[256];
    cuDeviceGetName(deviceName, sizeof(deviceName), m_CudaDevice);
    LUCENT_CORE_INFO("OptiXDenoiser: Using CUDA device: {}", deviceName);
    
    return true;
}

bool OptiXDenoiser::InitOptix() {
    // Load OptiX
    OptixResult res = optixInit();
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixInit failed with error {}", static_cast<int>(res));
        return false;
    }
    
    // Create OptiX device context
    OptixDeviceContextOptions options{};
    options.logCallbackFunction = OptixLogCallback;
    options.logCallbackLevel = 4;  // Info level
    
    res = optixDeviceContextCreate(m_CudaContext, &options, &m_OptixContext);
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDeviceContextCreate failed");
        return false;
    }
    
    return true;
}

bool OptiXDenoiser::CreateDenoiser() {
    // Create denoiser with AOV (albedo + normal) guides
    OptixDenoiserOptions denoiserOptions{};
    denoiserOptions.guideAlbedo = 1;  // Enable albedo guide
    denoiserOptions.guideNormal = 1;  // Enable normal guide
    
    OptixResult res = optixDenoiserCreate(
        m_OptixContext,
        OPTIX_DENOISER_MODEL_KIND_AOV,  // AOV model for best quality with guides
        &denoiserOptions,
        &m_Denoiser
    );
    
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDenoiserCreate failed");
        return false;
    }
    
    // Allocate intensity buffer (single float for HDR denoising)
    cudaError_t cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_IntensityBuffer), sizeof(float));
    if (cudaRes != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to allocate intensity buffer");
        return false;
    }
    
    return true;
}

bool OptiXDenoiser::Resize(uint32_t width, uint32_t height) {
    if (width == m_Width && height == m_Height) {
        return true;  // No resize needed
    }
    
    m_Width = width;
    m_Height = height;
    
    LUCENT_CORE_INFO("OptiXDenoiser: Resizing to {}x{}", width, height);
    
    // Compute denoiser memory requirements
    OptixResult res = optixDenoiserComputeMemoryResources(
        m_Denoiser,
        width, height,
        &m_DenoiserSizes
    );
    
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDenoiserComputeMemoryResources failed");
        return false;
    }
    
    // Free old buffers
    if (m_StateBuffer) cudaFree(reinterpret_cast<void*>(m_StateBuffer));
    if (m_ScratchBuffer) cudaFree(reinterpret_cast<void*>(m_ScratchBuffer));
    if (m_ColorBuffer) cudaFree(reinterpret_cast<void*>(m_ColorBuffer));
    if (m_AlbedoBuffer) cudaFree(reinterpret_cast<void*>(m_AlbedoBuffer));
    if (m_NormalBuffer) cudaFree(reinterpret_cast<void*>(m_NormalBuffer));
    if (m_OutputBuffer) cudaFree(reinterpret_cast<void*>(m_OutputBuffer));
    
    // Allocate new buffers
    size_t pixelBufferSize = width * height * 4 * sizeof(float);  // RGBA32F
    
    cudaError_t cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_StateBuffer), m_DenoiserSizes.stateSizeInBytes);
    if (cudaRes != cudaSuccess) return false;
    
    cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_ScratchBuffer), m_DenoiserSizes.withoutOverlapScratchSizeInBytes);
    if (cudaRes != cudaSuccess) return false;
    
    cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_ColorBuffer), pixelBufferSize);
    if (cudaRes != cudaSuccess) return false;
    
    cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_AlbedoBuffer), pixelBufferSize);
    if (cudaRes != cudaSuccess) return false;
    
    cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_NormalBuffer), pixelBufferSize);
    if (cudaRes != cudaSuccess) return false;
    
    cudaRes = cudaMalloc(reinterpret_cast<void**>(&m_OutputBuffer), pixelBufferSize);
    if (cudaRes != cudaSuccess) return false;
    
    // Setup denoiser
    res = optixDenoiserSetup(
        m_Denoiser,
        m_CudaStream,
        width, height,
        m_StateBuffer, m_DenoiserSizes.stateSizeInBytes,
        m_ScratchBuffer, m_DenoiserSizes.withoutOverlapScratchSizeInBytes
    );
    
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDenoiserSetup failed");
        return false;
    }
    
    LUCENT_CORE_INFO("OptiXDenoiser: Buffers resized (state: {} KB, scratch: {} KB)",
        m_DenoiserSizes.stateSizeInBytes / 1024,
        m_DenoiserSizes.withoutOverlapScratchSizeInBytes / 1024);
    
    return true;
}

bool OptiXDenoiser::Denoise(
    Image* inputColor,
    Image* inputAlbedo,
    Image* inputNormal,
    Image* output,
    VkCommandBuffer cmd,
    VkSemaphore /*waitSemaphore*/,
    VkSemaphore /*signalSemaphore*/)
{
    if (!m_Initialized || !inputColor) return false;
    
    uint32_t width = inputColor->GetWidth();
    uint32_t height = inputColor->GetHeight();
    size_t bufferSize = static_cast<size_t>(width) * height * 4 * sizeof(float);
    
    // Resize CUDA buffers if needed
    if (width != m_Width || height != m_Height) {
        if (!Resize(width, height)) {
            return false;
        }
        // Also resize/recreate shared images
        ResizeSharedImages(width, height);
    }
    
    // Ensure shared images exist
    if (m_SharedColor.vkImage == VK_NULL_HANDLE) {
        ResizeSharedImages(width, height);
    }
    
    // Check if CUDA arrays are valid (external memory export must have succeeded)
    if (!m_SharedColor.cudaArray || !m_SharedAlbedo.cudaArray || 
        !m_SharedNormal.cudaArray || !m_SharedOutput.cudaArray) {
        LUCENT_CORE_WARN("OptiXDenoiser: CUDA arrays not available, skipping denoise");
        return false;
    }
    
    VkDevice device = m_Context->GetDevice();
    
    // ========== GPU-ONLY PATH ==========
    // Step 1: Copy Vulkan images to shared images (GPU blit)
    VkCommandBuffer copyCmd = m_Device->BeginSingleTimeCommands();
    
    // Transition shared images to transfer dst
    TransitionImageLayout(copyCmd, m_SharedColor.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    TransitionImageLayout(copyCmd, m_SharedAlbedo.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    TransitionImageLayout(copyCmd, m_SharedNormal.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Transition input images to transfer src
    inputColor->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (inputAlbedo && inputAlbedo->GetHandle()) {
        inputAlbedo->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (inputNormal && inputNormal->GetHandle()) {
        inputNormal->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    
    // Copy color
    VkImageCopy copyRegion{};
    copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.extent = {width, height, 1};
    
    vkCmdCopyImage(copyCmd, inputColor->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        m_SharedColor.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    if (inputAlbedo && inputAlbedo->GetHandle()) {
        vkCmdCopyImage(copyCmd, inputAlbedo->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_SharedAlbedo.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    }
    if (inputNormal && inputNormal->GetHandle()) {
        vkCmdCopyImage(copyCmd, inputNormal->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_SharedNormal.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    }
    
    // Transition back
    inputColor->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    if (inputAlbedo && inputAlbedo->GetHandle()) {
        inputAlbedo->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (inputNormal && inputNormal->GetHandle()) {
        inputNormal->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    
    // Transition shared images to general for CUDA access
    TransitionImageLayout(copyCmd, m_SharedColor.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    TransitionImageLayout(copyCmd, m_SharedAlbedo.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    TransitionImageLayout(copyCmd, m_SharedNormal.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    TransitionImageLayout(copyCmd, m_SharedOutput.vkImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    
    m_Device->EndSingleTimeCommands(copyCmd);
    
    // Step 2: Copy from CUDA arrays to linear buffers (GPU-to-GPU within CUDA)
    cudaMemcpy2DFromArray(reinterpret_cast<void*>(m_ColorBuffer), width * 4 * sizeof(float),
        m_SharedColor.cudaArray, 0, 0, width * 4 * sizeof(float), height, cudaMemcpyDeviceToDevice);
    cudaMemcpy2DFromArray(reinterpret_cast<void*>(m_AlbedoBuffer), width * 4 * sizeof(float),
        m_SharedAlbedo.cudaArray, 0, 0, width * 4 * sizeof(float), height, cudaMemcpyDeviceToDevice);
    cudaMemcpy2DFromArray(reinterpret_cast<void*>(m_NormalBuffer), width * 4 * sizeof(float),
        m_SharedNormal.cudaArray, 0, 0, width * 4 * sizeof(float), height, cudaMemcpyDeviceToDevice);
    
    // Step 3: Run OptiX denoiser
    OptixDenoiserParams params{};
    params.blendFactor = 0.0f;
    params.hdrIntensity = m_IntensityBuffer;
    
    unsigned int rowStride = width * 4 * sizeof(float);
    unsigned int pixelStride = 4 * sizeof(float);
    
    OptixImage2D colorImage{};
    colorImage.data = m_ColorBuffer;
    colorImage.width = width;
    colorImage.height = height;
    colorImage.rowStrideInBytes = rowStride;
    colorImage.pixelStrideInBytes = pixelStride;
    colorImage.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    optixDenoiserComputeIntensity(m_Denoiser, m_CudaStream, &colorImage,
        m_IntensityBuffer, m_ScratchBuffer, m_DenoiserSizes.withoutOverlapScratchSizeInBytes);
    
    OptixDenoiserGuideLayer guideLayer{};
    guideLayer.albedo = {m_AlbedoBuffer, width, height, rowStride, pixelStride, OPTIX_PIXEL_FORMAT_FLOAT4};
    guideLayer.normal = {m_NormalBuffer, width, height, rowStride, pixelStride, OPTIX_PIXEL_FORMAT_FLOAT4};
    
    OptixDenoiserLayer layer{};
    layer.input = colorImage;
    layer.output = {m_OutputBuffer, width, height, rowStride, pixelStride, OPTIX_PIXEL_FORMAT_FLOAT4};
    
    OptixResult res = optixDenoiserInvoke(m_Denoiser, m_CudaStream, &params,
        m_StateBuffer, m_DenoiserSizes.stateSizeInBytes,
        &guideLayer, &layer, 1, 0, 0,
        m_ScratchBuffer, m_DenoiserSizes.withoutOverlapScratchSizeInBytes);
    
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDenoiserInvoke failed with error {}", static_cast<int>(res));
        return false;
    }
    
    // Step 4: Copy output buffer back to CUDA array (GPU-to-GPU)
    cudaMemcpy2DToArray(m_SharedOutput.cudaArray, 0, 0, reinterpret_cast<void*>(m_OutputBuffer),
        width * 4 * sizeof(float), width * 4 * sizeof(float), height, cudaMemcpyDeviceToDevice);
    
    cudaStreamSynchronize(m_CudaStream);
    
    // Step 5: Blit shared output to the OUTPUT image (handles format conversion)
    // This preserves the accumulation buffer for future samples
    VkCommandBuffer finalCmd = m_Device->BeginSingleTimeCommands();
    
    TransitionImageLayout(finalCmd, m_SharedOutput.vkImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    output->TransitionLayout(finalCmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Use blit instead of copy to handle format conversion (R32G32B32A32_SFLOAT -> R16G16B16A16_SFLOAT)
    VkImageBlit blitRegion{};
    blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.srcOffsets[0] = {0, 0, 0};
    blitRegion.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blitRegion.dstOffsets[0] = {0, 0, 0};
    blitRegion.dstOffsets[1] = {static_cast<int32_t>(output->GetWidth()), static_cast<int32_t>(output->GetHeight()), 1};
    
    vkCmdBlitImage(finalCmd, m_SharedOutput.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        output->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion, VK_FILTER_LINEAR);
    
    output->TransitionLayout(finalCmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(finalCmd, m_SharedOutput.vkImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    
    m_Device->EndSingleTimeCommands(finalCmd);
    
    (void)cmd;
    (void)device;
    (void)bufferSize;
    
    m_DenoisePerformed = true;
    return true;
}

void OptiXDenoiser::TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void OptiXDenoiser::ResizeSharedImages(uint32_t width, uint32_t height) {
    // Destroy old shared images
    DestroySharedImage(m_SharedColor);
    DestroySharedImage(m_SharedAlbedo);
    DestroySharedImage(m_SharedNormal);
    DestroySharedImage(m_SharedOutput);
    
    // Create new shared images with external memory
    CreateSharedImage(m_SharedColor, width, height, "OptiXSharedColor");
    CreateSharedImage(m_SharedAlbedo, width, height, "OptiXSharedAlbedo");
    CreateSharedImage(m_SharedNormal, width, height, "OptiXSharedNormal");
    CreateSharedImage(m_SharedOutput, width, height, "OptiXSharedOutput");
}

bool OptiXDenoiser::CreateSharedImage(CudaVulkanImage& img, uint32_t width, uint32_t height, const char* debugName) {
    VkDevice device = m_Context->GetDevice();
    
    // Create Vulkan image with external memory
    VkExternalMemoryImageCreateInfo extMemInfo{};
    extMemInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
#ifdef _WIN32
    extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &extMemInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    
    if (vkCreateImage(device, &imageInfo, nullptr, &img.vkImage) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to create shared image {}", debugName);
        return false;
    }
    
    // Allocate memory with export capability
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, img.vkImage, &memReqs);
    
    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
#ifdef _WIN32
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_Device->FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (vkAllocateMemory(device, &allocInfo, nullptr, &img.vkMemory) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to allocate shared image memory");
        return false;
    }
    
    vkBindImageMemory(device, img.vkImage, img.vkMemory, 0);
    
    // Export memory handle to CUDA
#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory = img.vkMemory;
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    
    HANDLE handle;
    auto vkGetMemoryWin32HandleKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    
    if (!vkGetMemoryWin32HandleKHR || vkGetMemoryWin32HandleKHR(device, &handleInfo, &handle) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to export memory handle");
        return false;
    }
    
    // Import into CUDA
    cudaExternalMemoryHandleDesc memDesc{};
    memDesc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
    memDesc.handle.win32.handle = handle;
    memDesc.size = memReqs.size;
    
    cudaError_t err = cudaImportExternalMemory(&img.cudaExtMem, &memDesc);
    if (err != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to import memory to CUDA: {}", cudaGetErrorString(err));
        return false;
    }
    
    // Create mipmapped array from external memory
    cudaExternalMemoryMipmappedArrayDesc arrayDesc{};
    arrayDesc.offset = 0;
    arrayDesc.formatDesc = cudaCreateChannelDesc<float4>();
    arrayDesc.extent = make_cudaExtent(width, height, 0);
    arrayDesc.flags = 0;
    arrayDesc.numLevels = 1;
    
    err = cudaExternalMemoryGetMappedMipmappedArray(&img.cudaMipArray, img.cudaExtMem, &arrayDesc);
    if (err != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to get CUDA mipmapped array: {}", cudaGetErrorString(err));
        return false;
    }
    
    // Get the base level array
    err = cudaGetMipmappedArrayLevel(&img.cudaArray, img.cudaMipArray, 0);
    if (err != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to get CUDA array level: {}", cudaGetErrorString(err));
        return false;
    }
#endif
    
    img.width = width;
    img.height = height;
    
    LUCENT_CORE_DEBUG("OptiXDenoiser: Created shared image {} ({}x{})", debugName, width, height);
    
    return true;
}

void OptiXDenoiser::DestroySharedImage(CudaVulkanImage& img) {
    if (img.cudaSurface) cudaDestroySurfaceObject(img.cudaSurface);
    if (img.cudaArray) {}  // Part of mipmap array
    if (img.cudaMipArray) cudaFreeMipmappedArray(img.cudaMipArray);
    if (img.cudaExtMem) cudaDestroyExternalMemory(img.cudaExtMem);
    
    VkDevice device = m_Context ? m_Context->GetDevice() : VK_NULL_HANDLE;
    if (device) {
        if (img.vkView) vkDestroyImageView(device, img.vkView, nullptr);
        if (img.vkImage) vkDestroyImage(device, img.vkImage, nullptr);
        if (img.vkMemory) vkFreeMemory(device, img.vkMemory, nullptr);
    }
    
    img = {};
}

} // namespace lucent::gfx

#endif // LUCENT_ENABLE_OPTIX


