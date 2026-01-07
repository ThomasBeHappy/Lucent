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
    if (!m_Initialized || !inputColor || !output) return false;
    
    uint32_t width = inputColor->GetWidth();
    uint32_t height = inputColor->GetHeight();
    size_t pixelCount = static_cast<size_t>(width) * height;
    size_t bufferSize = pixelCount * 4 * sizeof(float);
    
    // Resize CUDA buffers if needed
    if (width != m_Width || height != m_Height) {
        if (!Resize(width, height)) {
            return false;
        }
    }
    
    // Allocate CPU staging memory if needed
    if (m_StagingBufferSize < bufferSize) {
        m_StagingBuffer.reset(new float[pixelCount * 4]);
        m_StagingBufferSize = bufferSize;
    }
    
    // We need to synchronize: the tracer has written to images on GPU
    // Ensure all prior GPU work is complete before we can read the images
    m_Context->WaitIdle();
    
    // Download color image from Vulkan to CPU, then upload to CUDA
    if (!DownloadImageToCuda(inputColor, m_ColorBuffer, width, height, cmd)) {
        LUCENT_CORE_WARN("OptiXDenoiser: Failed to download color image");
        return false;
    }
    
    // Download albedo and normal AOV guides if available
    if (inputAlbedo && inputAlbedo->GetHandle()) {
        DownloadImageToCuda(inputAlbedo, m_AlbedoBuffer, width, height, cmd);
    } else {
        // Fill with white albedo if not available
        cudaMemset(reinterpret_cast<void*>(m_AlbedoBuffer), 0xFF, bufferSize);
    }
    
    if (inputNormal && inputNormal->GetHandle()) {
        DownloadImageToCuda(inputNormal, m_NormalBuffer, width, height, cmd);
    } else {
        // Fill with up normals if not available
        cudaMemset(reinterpret_cast<void*>(m_NormalBuffer), 0, bufferSize);
    }
    
    // Setup denoiser params
    OptixDenoiserParams params{};
    params.blendFactor = 0.0f;  // 0 = fully denoised, 1 = original
    params.hdrIntensity = m_IntensityBuffer;
    
    // Create image descriptors
    unsigned int rowStride = width * 4 * sizeof(float);
    unsigned int pixelStride = 4 * sizeof(float);
    
    OptixImage2D colorImage{};
    colorImage.data = m_ColorBuffer;
    colorImage.width = width;
    colorImage.height = height;
    colorImage.rowStrideInBytes = rowStride;
    colorImage.pixelStrideInBytes = pixelStride;
    colorImage.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    // Compute HDR intensity
    optixDenoiserComputeIntensity(
        m_Denoiser,
        m_CudaStream,
        &colorImage,
        m_IntensityBuffer,
        m_ScratchBuffer,
        m_DenoiserSizes.withoutOverlapScratchSizeInBytes
    );
    
    // Setup input layers
    OptixDenoiserGuideLayer guideLayer{};
    guideLayer.albedo.data = m_AlbedoBuffer;
    guideLayer.albedo.width = width;
    guideLayer.albedo.height = height;
    guideLayer.albedo.rowStrideInBytes = rowStride;
    guideLayer.albedo.pixelStrideInBytes = pixelStride;
    guideLayer.albedo.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    guideLayer.normal.data = m_NormalBuffer;
    guideLayer.normal.width = width;
    guideLayer.normal.height = height;
    guideLayer.normal.rowStrideInBytes = rowStride;
    guideLayer.normal.pixelStrideInBytes = pixelStride;
    guideLayer.normal.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    OptixDenoiserLayer layer{};
    layer.input = colorImage;
    layer.output.data = m_OutputBuffer;
    layer.output.width = width;
    layer.output.height = height;
    layer.output.rowStrideInBytes = rowStride;
    layer.output.pixelStrideInBytes = pixelStride;
    layer.output.format = OPTIX_PIXEL_FORMAT_FLOAT4;
    
    // Invoke denoiser
    OptixResult res = optixDenoiserInvoke(
        m_Denoiser,
        m_CudaStream,
        &params,
        m_StateBuffer, m_DenoiserSizes.stateSizeInBytes,
        &guideLayer,
        &layer, 1,
        0, 0,  // Input offset
        m_ScratchBuffer, m_DenoiserSizes.withoutOverlapScratchSizeInBytes
    );
    
    if (res != OPTIX_SUCCESS) {
        LUCENT_CORE_ERROR("OptiXDenoiser: optixDenoiserInvoke failed with error {}", static_cast<int>(res));
        return false;
    }
    
    // Synchronize CUDA
    cudaStreamSynchronize(m_CudaStream);
    
    // Upload denoised result back to the INPUT color image (we modify it in place)
    // This way the subsequent blit will use the denoised data
    UploadCudaToImage(m_OutputBuffer, inputColor, width, height, cmd);
    
    return true;
}

bool OptiXDenoiser::DownloadImageToCuda(Image* image, CUdeviceptr cudaBuffer, uint32_t width, uint32_t height, VkCommandBuffer cmd) {
    (void)cmd;  // We use synchronous copies for now
    
    VkDevice device = m_Context->GetDevice();
    size_t bufferSize = static_cast<size_t>(width) * height * 4 * sizeof(float);
    
    // Create a staging buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer stagingBuffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_Device->FindMemoryType(memReqs.memoryTypeBits, 
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    VkDeviceMemory stagingMemory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
    
    // Create one-shot command buffer to copy image to buffer
    VkCommandPool cmdPool = m_Device->GetGraphicsCommandPool();
    VkCommandBuffer copyCmd;
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmdAllocInfo, &copyCmd);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(copyCmd, &beginInfo);
    
    // Transition image to transfer src
    image->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    
    // Copy image to buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    
    vkCmdCopyImageToBuffer(copyCmd, image->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        stagingBuffer, 1, &region);
    
    // Transition back to general
    image->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    
    vkEndCommandBuffer(copyCmd);
    
    // Submit and wait
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &copyCmd;
    
    vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Context->GetGraphicsQueue());
    
    vkFreeCommandBuffers(device, cmdPool, 1, &copyCmd);
    
    // Map staging buffer and copy to CUDA
    void* data;
    vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
    cudaMemcpy(reinterpret_cast<void*>(cudaBuffer), data, bufferSize, cudaMemcpyHostToDevice);
    vkUnmapMemory(device, stagingMemory);
    
    // Cleanup
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    
    return true;
}

void OptiXDenoiser::UploadCudaToImage(CUdeviceptr cudaBuffer, Image* image, uint32_t width, uint32_t height, VkCommandBuffer cmd) {
    (void)cmd;
    
    VkDevice device = m_Context->GetDevice();
    size_t bufferSize = static_cast<size_t>(width) * height * 4 * sizeof(float);
    
    // Create staging buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    VkBuffer stagingBuffer;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
        return;
    }
    
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_Device->FindMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    VkDeviceMemory stagingMemory;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        return;
    }
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
    
    // Copy CUDA buffer to staging
    void* data;
    vkMapMemory(device, stagingMemory, 0, bufferSize, 0, &data);
    cudaMemcpy(data, reinterpret_cast<void*>(cudaBuffer), bufferSize, cudaMemcpyDeviceToHost);
    vkUnmapMemory(device, stagingMemory);
    
    // Create one-shot command buffer
    VkCommandPool cmdPool = m_Device->GetGraphicsCommandPool();
    VkCommandBuffer copyCmd;
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = cmdPool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmdAllocInfo, &copyCmd);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(copyCmd, &beginInfo);
    
    // Transition image to transfer dst
    image->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    
    vkCmdCopyBufferToImage(copyCmd, stagingBuffer, image->GetHandle(), 
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    // Transition back to general
    image->TransitionLayout(copyCmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    
    vkEndCommandBuffer(copyCmd);
    
    // Submit and wait
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &copyCmd;
    
    vkQueueSubmit(m_Context->GetGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_Context->GetGraphicsQueue());
    
    vkFreeCommandBuffers(device, cmdPool, 1, &copyCmd);
    
    // Cleanup
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
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
    
    if (cudaImportExternalMemory(&img.cudaExtMem, &memDesc) != cudaSuccess) {
        LUCENT_CORE_ERROR("OptiXDenoiser: Failed to import memory to CUDA");
        return false;
    }
#endif
    
    img.width = width;
    img.height = height;
    
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


