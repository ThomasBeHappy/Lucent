#include "lucent/gfx/FinalRender.h"
#include "lucent/gfx/Renderer.h"
#include "lucent/gfx/TracerCompute.h"
#include "lucent/gfx/TracerRayKHR.h"
#include "lucent/core/Log.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <fstream>
#include <cmath>
#include <vector>

// stb_image_write for PNG export
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace lucent::gfx {

namespace {

float ComputeLuminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

void BoxDenoise(const float* src, float* dst, uint32_t width, uint32_t height, uint32_t radius) {
    const int32_t r = static_cast<int32_t>(radius);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float sumR = 0.0f;
            float sumG = 0.0f;
            float sumB = 0.0f;
            float sumA = 0.0f;
            uint32_t count = 0;

            for (int32_t dy = -r; dy <= r; ++dy) {
                int32_t yy = std::clamp(static_cast<int32_t>(y) + dy, 0, static_cast<int32_t>(height - 1));
                for (int32_t dx = -r; dx <= r; ++dx) {
                    int32_t xx = std::clamp(static_cast<int32_t>(x) + dx, 0, static_cast<int32_t>(width - 1));
                    uint32_t idx = (yy * width + xx) * 4;
                    sumR += src[idx + 0];
                    sumG += src[idx + 1];
                    sumB += src[idx + 2];
                    sumA += src[idx + 3];
                    count++;
                }
            }

            uint32_t outIdx = (y * width + x) * 4;
            float inv = count > 0 ? (1.0f / static_cast<float>(count)) : 1.0f;
            dst[outIdx + 0] = sumR * inv;
            dst[outIdx + 1] = sumG * inv;
            dst[outIdx + 2] = sumB * inv;
            dst[outIdx + 3] = sumA * inv;
        }
    }
}

void EdgeAwareDenoise(const float* src, float* dst, uint32_t width, uint32_t height, uint32_t radius) {
    const int32_t r = static_cast<int32_t>(radius);
    const float sigmaSpatial = std::max(1.0f, radius * 0.5f);
    const float sigmaColor = 0.1f;
    const float invSpatial = 1.0f / (2.0f * sigmaSpatial * sigmaSpatial);
    const float invColor = 1.0f / (2.0f * sigmaColor * sigmaColor);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t centerIdx = (y * width + x) * 4;
            float centerR = src[centerIdx + 0];
            float centerG = src[centerIdx + 1];
            float centerB = src[centerIdx + 2];
            float centerLuma = ComputeLuminance(centerR, centerG, centerB);

            float sumR = 0.0f;
            float sumG = 0.0f;
            float sumB = 0.0f;
            float sumA = 0.0f;
            float weightSum = 0.0f;

            for (int32_t dy = -r; dy <= r; ++dy) {
                int32_t yy = std::clamp(static_cast<int32_t>(y) + dy, 0, static_cast<int32_t>(height - 1));
                for (int32_t dx = -r; dx <= r; ++dx) {
                    int32_t xx = std::clamp(static_cast<int32_t>(x) + dx, 0, static_cast<int32_t>(width - 1));
                    uint32_t idx = (yy * width + xx) * 4;

                    float sampleR = src[idx + 0];
                    float sampleG = src[idx + 1];
                    float sampleB = src[idx + 2];
                    float sampleA = src[idx + 3];
                    float sampleLuma = ComputeLuminance(sampleR, sampleG, sampleB);

                    float spatialWeight = std::exp(-(dx * dx + dy * dy) * invSpatial);
                    float colorDelta = sampleLuma - centerLuma;
                    float colorWeight = std::exp(-(colorDelta * colorDelta) * invColor);
                    float weight = spatialWeight * colorWeight;

                    sumR += sampleR * weight;
                    sumG += sampleG * weight;
                    sumB += sampleB * weight;
                    sumA += sampleA * weight;
                    weightSum += weight;
                }
            }

            if (weightSum <= 0.0f) {
                weightSum = 1.0f;
            }

            dst[centerIdx + 0] = sumR / weightSum;
            dst[centerIdx + 1] = sumG / weightSum;
            dst[centerIdx + 2] = sumB / weightSum;
            dst[centerIdx + 3] = sumA / weightSum;
        }
    }
}

} // namespace

FinalRender::~FinalRender() {
    Shutdown();
}

bool FinalRender::Init(Renderer* renderer) {
    m_Renderer = renderer;
    LUCENT_CORE_INFO("FinalRender initialized");
    return true;
}

void FinalRender::Shutdown() {
    Cancel();
    // Ensure we never leave the compute tracer bound to our accumulation image.
    if (m_Renderer) {
        if (auto* compute = m_Renderer->GetTracerCompute()) {
            compute->SetExternalAccumulationImage(nullptr);
        }
    }
    DestroyRenderResources();
    m_Renderer = nullptr;
}

bool FinalRender::Start(const FinalRenderConfig& config, const GPUCamera& camera,
                         const std::vector<BVHBuilder::Triangle>& triangles,
                         const std::vector<GPUMaterial>& materials,
                         const std::vector<GPULight>& lights,
                         const std::vector<GPUVolume>& volumes) {
    if (m_Status == FinalRenderStatus::Rendering) {
        LUCENT_CORE_WARN("FinalRender: Already rendering");
        return false;
    }
    
    m_Config = config;
    m_Camera = camera;
    m_Camera.resolution = glm::vec2(config.width, config.height);
    m_Triangles = triangles;
    m_Materials = materials;
    
    // Create render resources
    if (!CreateRenderResources()) {
        LUCENT_CORE_ERROR("FinalRender: Failed to create render resources");
        m_Status = FinalRenderStatus::Failed;
        return false;
    }
    
    // Update tracer scene
    m_UsingRayTracing = (m_Config.useRayTracing &&
                         m_Renderer->GetTracerRayKHR() &&
                         m_Renderer->GetTracerRayKHR()->IsSupported());

    if (m_UsingRayTracing) {
        m_Renderer->GetTracerRayKHR()->UpdateScene(triangles, materials, lights, volumes);
        m_Renderer->GetTracerRayKHR()->ResetAccumulation();
    } else if (m_Renderer->GetTracerCompute()) {
        m_Renderer->GetTracerCompute()->UpdateScene(triangles, materials, lights, volumes);
        // FinalRender uses a dedicated accumulation image. Bind it explicitly as the tracer's storage target.
        m_Renderer->GetTracerCompute()->SetExternalAccumulationImage(&m_AccumImage);
        m_Renderer->GetTracerCompute()->ResetAccumulation();
    } else {
        LUCENT_CORE_ERROR("FinalRender: No tracer available");
        m_Status = FinalRenderStatus::Failed;
        return false;
    }
    
    m_CurrentSample = 0;
    // Init tiling (dispatch one tile per frame to avoid GPU TDR on slow devices)
    // KHR ray tracing path is not tiled yet, so force a single tile (one sample per frame).
    if (m_UsingRayTracing) {
        m_TileSize = std::max(config.width, config.height);
        m_TilesX = 1;
        m_TilesY = 1;
    } else {
        m_TileSize = 256;
        m_TilesX = std::max(1u, (config.width + m_TileSize - 1) / m_TileSize);
        m_TilesY = std::max(1u, (config.height + m_TileSize - 1) / m_TileSize);
    }
    m_CurrentTile = 0;
    m_StartTime = glfwGetTime();
    m_CancelRequested = false;
    m_Status = FinalRenderStatus::Rendering;
    
    LUCENT_CORE_INFO("FinalRender: Started {}x{}, {} samples", 
        config.width, config.height, config.samples);
    
    return true;
}

void FinalRender::Cancel() {
    if (m_Status == FinalRenderStatus::Rendering) {
        m_CancelRequested = true;
        m_Status = FinalRenderStatus::Cancelled;
        if (!m_UsingRayTracing && m_Renderer) {
            if (auto* compute = m_Renderer->GetTracerCompute()) {
                compute->SetExternalAccumulationImage(nullptr);
            }
        }
        LUCENT_CORE_INFO("FinalRender: Cancelled");
    }
}

bool FinalRender::RenderSample() {
    if (m_Status != FinalRenderStatus::Rendering) {
        return false;
    }
    
    if (m_CancelRequested) {
        m_Status = FinalRenderStatus::Cancelled;
        return false;
    }
    
    if (m_CurrentSample >= m_Config.samples) {
        // Apply tonemapping and finalize
        ApplyTonemap();
        m_Status = FinalRenderStatus::Completed;
        if (!m_UsingRayTracing && m_Renderer) {
            if (auto* compute = m_Renderer->GetTracerCompute()) {
                compute->SetExternalAccumulationImage(nullptr);
            }
        }
        
        float elapsed = GetElapsedTime();
        LUCENT_CORE_INFO("FinalRender: Completed in {:.2f}s ({:.2f}ms/sample)", 
            elapsed, elapsed * 1000.0f / m_Config.samples);
        
        // Auto-save if path is set
        if (!m_Config.outputPath.empty()) {
            ExportImage(m_Config.outputPath);
        }
        
        return false;
    }
    
    // Create render settings for this sample (tile-based)
    RenderSettings settings;
    settings.activeMode = m_UsingRayTracing ? RenderMode::RayTraced : RenderMode::Traced;
    settings.maxBounces = m_Config.maxBounces;
    settings.clampIndirect = 10.0f;
    settings.accumulatedSamples = m_CurrentSample;
    settings.viewportSamples = m_Config.samples;
    settings.transparentBackground = m_Config.transparentBackground;
    
    // Record command buffer
    VkCommandBuffer cmd = m_Renderer->GetDevice()->BeginSingleTimeCommands();

    bool completedSampleThisCall = false;
    if (m_UsingRayTracing && m_Renderer->GetTracerRayKHR() && m_Renderer->GetTracerRayKHR()->IsSupported()) {
        // Ray tracing path: full dispatch each call (no tiling) -> one sample per call
        m_Renderer->GetTracerRayKHR()->Trace(cmd, m_Camera, settings, &m_AccumImage /* used for sizing */);
        completedSampleThisCall = true;
    } else if (m_Renderer->GetTracerCompute()) {
        // Compute current tile rect
        const uint32_t totalTiles = std::max(1u, m_TilesX * m_TilesY);
        const uint32_t tileIdx = std::min(m_CurrentTile, totalTiles - 1);
        const uint32_t tileX = tileIdx % m_TilesX;
        const uint32_t tileY = tileIdx / m_TilesX;
        const uint32_t offsetX = tileX * m_TileSize;
        const uint32_t offsetY = tileY * m_TileSize;
        const uint32_t tileW = std::min(m_TileSize, m_Config.width - offsetX);
        const uint32_t tileH = std::min(m_TileSize, m_Config.height - offsetY);

        // Trace one tile of the current sample (accum target already set via SetExternalAccumulationImage())
        m_Renderer->GetTracerCompute()->TraceRegion(cmd, m_Camera, settings, nullptr, offsetX, offsetY, tileW, tileH);

        // Advance tile/sample
        m_CurrentTile++;
        if (m_CurrentTile >= totalTiles) {
            m_CurrentTile = 0;
            completedSampleThisCall = true;
        }
    }

    m_Renderer->GetDevice()->EndSingleTimeCommands(cmd);

    if (completedSampleThisCall) {
        m_CurrentSample++;

        // If we just finished the last sample, finalize immediately.
        if (m_CurrentSample >= m_Config.samples) {
            ApplyTonemap();
            m_Status = FinalRenderStatus::Completed;
            if (!m_UsingRayTracing && m_Renderer) {
                if (auto* compute = m_Renderer->GetTracerCompute()) {
                    compute->SetExternalAccumulationImage(nullptr);
                }
            }

            float elapsed = GetElapsedTime();
            LUCENT_CORE_INFO("FinalRender: Completed in {:.2f}s ({:.2f}ms/sample)",
                elapsed, elapsed * 1000.0f / m_Config.samples);

            // Auto-save if path is set
            if (!m_Config.outputPath.empty()) {
                ExportImage(m_Config.outputPath);
            }

            return false;
        }

        // Update preview image for the editor (progressive feedback)
        UpdatePreviewTonemap(/*finalPass=*/false);
    }
    
    // Call progress callback
    if (m_ProgressCallback) {
        m_ProgressCallback(m_CurrentSample, m_Config.samples, GetElapsedTime());
    }
    
    return true;
}

float FinalRender::GetProgress() const {
    if (m_Config.samples == 0) return 0.0f;
    const float base = static_cast<float>(m_CurrentSample);
    float tileFrac = 0.0f;
    if (m_Status == FinalRenderStatus::Rendering) {
        const uint32_t totalTiles = std::max(1u, m_TilesX * m_TilesY);
        tileFrac = static_cast<float>(std::min(m_CurrentTile, totalTiles)) / static_cast<float>(totalTiles);
    }
    return std::clamp((base + tileFrac) / static_cast<float>(m_Config.samples), 0.0f, 1.0f);
}

float FinalRender::GetElapsedTime() const {
    return static_cast<float>(glfwGetTime() - m_StartTime);
}

bool FinalRender::CreateRenderResources() {
    Device* device = m_Renderer->GetDevice();
    
    // Create accumulation image
    ImageDesc accumDesc{};
    accumDesc.width = m_Config.width;
    accumDesc.height = m_Config.height;
    accumDesc.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    accumDesc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    accumDesc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    accumDesc.debugName = "FinalRenderAccum";
    
    m_AccumImage.Shutdown();
    if (!m_AccumImage.Init(device, accumDesc)) {
        return false;
    }
    
    // Transition to general layout
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();
    m_AccumImage.TransitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    device->EndSingleTimeCommands(cmd);
    
    // Create tonemapped output image
    ImageDesc renderDesc{};
    renderDesc.width = m_Config.width;
    renderDesc.height = m_Config.height;
    renderDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    renderDesc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    renderDesc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    renderDesc.debugName = "FinalRenderOutput";
    
    m_RenderImage.Shutdown();
    if (!m_RenderImage.Init(device, renderDesc)) {
        return false;
    }

    // Initialize output image to a valid sampled layout immediately (ImGui expects SHADER_READ_ONLY_OPTIMAL)
    {
        VkCommandBuffer cmd2 = device->BeginSingleTimeCommands();
        m_RenderImage.TransitionLayout(cmd2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkClearColorValue clearColor = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd2, m_RenderImage.GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

        m_RenderImage.TransitionLayout(cmd2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        device->EndSingleTimeCommands(cmd2);
    }
    
    // Allocate CPU buffer
    m_PixelBuffer.resize(m_Config.width * m_Config.height * 4);
    
    return true;
}

void FinalRender::DestroyRenderResources() {
    m_AccumImage.Shutdown();
    m_RenderImage.Shutdown();
    m_PixelBuffer.clear();
}

bool FinalRender::ApplyTonemap() {
    return UpdatePreviewTonemap(/*finalPass=*/true);
}

Image* FinalRender::GetAccumulationSource() {
    if (!m_Renderer) return &m_AccumImage;
    if (m_UsingRayTracing) {
        if (auto* rt = m_Renderer->GetTracerRayKHR()) {
            if (auto* img = rt->GetAccumulationImage(); img && img->GetHandle() != VK_NULL_HANDLE) {
                return img;
            }
        }
    }
    return &m_AccumImage;
}

bool FinalRender::UpdatePreviewTonemap(bool finalPass) {
    // For simplicity, read back the HDR accumulation and tonemap on CPU.
    // This is used for both progressive preview updates and final output.
    Device* device = m_Renderer->GetDevice();
    Image* srcImage = GetAccumulationSource();
    if (!srcImage || srcImage->GetHandle() == VK_NULL_HANDLE) {
        return false;
    }

    // Create staging buffer for readback
    VkDeviceSize imageSize = m_Config.width * m_Config.height * sizeof(float) * 4;

    BufferDesc stagingDesc{};
    stagingDesc.size = imageSize;
    stagingDesc.usage = BufferUsage::Staging;
    stagingDesc.hostVisible = true;
    stagingDesc.debugName = finalPass ? "FinalRenderStaging_Final" : "FinalRenderStaging_Preview";

    Buffer stagingBuffer;
    if (!stagingBuffer.Init(device, stagingDesc)) {
        return false;
    }

    // Copy image to staging buffer
    VkCommandBuffer cmd = device->BeginSingleTimeCommands();

    // Transition accumulation image for transfer (and restore for continued tracing)
    VkImageLayout oldLayout = srcImage->GetCurrentLayout();
    VkImageLayout restoreLayout = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_IMAGE_LAYOUT_GENERAL : oldLayout;
    if (restoreLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        srcImage->TransitionLayout(cmd, restoreLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_Config.width, m_Config.height, 1};

    vkCmdCopyImageToBuffer(cmd, srcImage->GetHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuffer.GetHandle(), 1, &region);

    // Always restore layout so the tracer images remain usable after the final render completes.
    if (restoreLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restoreLayout);
    } else {
        // Sensible default for accumulation images
        srcImage->TransitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    device->EndSingleTimeCommands(cmd);

    // Read back and denoise/tonemap
    float* hdrData = static_cast<float*>(stagingBuffer.Map());
    float strength = std::clamp(m_Config.denoiseStrength, 0.0f, 1.0f);
    uint32_t radius = std::max(1u, m_Config.denoiseRadius);
    bool useDenoiser = m_Config.denoiser != DenoiserType::None && strength > 0.0f;
    bool denoiseSupported = m_Config.denoiser == DenoiserType::Box || m_Config.denoiser == DenoiserType::EdgeAware;
    std::vector<float> denoised;

    if (useDenoiser && denoiseSupported) {
        denoised.resize(static_cast<size_t>(m_Config.width) * m_Config.height * 4);
        if (m_Config.denoiser == DenoiserType::EdgeAware) {
            EdgeAwareDenoise(hdrData, denoised.data(), m_Config.width, m_Config.height, radius);
        } else {
            BoxDenoise(hdrData, denoised.data(), m_Config.width, m_Config.height, radius);
        }
    } else {
        useDenoiser = false;
    }

    for (uint32_t i = 0; i < m_Config.width * m_Config.height; i++) {
        float r = hdrData[i * 4 + 0];
        float g = hdrData[i * 4 + 1];
        float b = hdrData[i * 4 + 2];

        if (useDenoiser) {
            r = r * (1.0f - strength) + denoised[i * 4 + 0] * strength;
            g = g * (1.0f - strength) + denoised[i * 4 + 1] * strength;
            b = b * (1.0f - strength) + denoised[i * 4 + 2] * strength;
        }

        // Apply exposure
        r *= m_Config.exposure;
        g *= m_Config.exposure;
        b *= m_Config.exposure;

        // Apply tonemapping
        switch (m_Config.tonemap) {
            case TonemapOperator::Reinhard:
                r = r / (1.0f + r);
                g = g / (1.0f + g);
                b = b / (1.0f + b);
                break;

            case TonemapOperator::ACES: {
                // ACES filmic tonemap
                auto aces = [](float x) {
                    const float a = 2.51f;
                    const float b = 0.03f;
                    const float c = 2.43f;
                    const float d = 0.59f;
                    const float e = 0.14f;
                    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
                };
                r = aces(r);
                g = aces(g);
                b = aces(b);
                break;
            }

            case TonemapOperator::None:
            default:
                r = std::clamp(r, 0.0f, 1.0f);
                g = std::clamp(g, 0.0f, 1.0f);
                b = std::clamp(b, 0.0f, 1.0f);
                break;
        }

        // Apply gamma
        r = std::pow(r, 1.0f / m_Config.gamma);
        g = std::pow(g, 1.0f / m_Config.gamma);
        b = std::pow(b, 1.0f / m_Config.gamma);

        // Convert to 8-bit
        m_PixelBuffer[i * 4 + 0] = static_cast<uint8_t>(r * 255.0f);
        m_PixelBuffer[i * 4 + 1] = static_cast<uint8_t>(g * 255.0f);
        m_PixelBuffer[i * 4 + 2] = static_cast<uint8_t>(b * 255.0f);
        float a = std::clamp(hdrData[i * 4 + 3], 0.0f, 1.0f);
        m_PixelBuffer[i * 4 + 3] = static_cast<uint8_t>(a * 255.0f);
    }

    stagingBuffer.Unmap();
    stagingBuffer.Shutdown();

    // Upload tonemapped 8-bit RGBA to GPU output image for in-editor preview
    VkDeviceSize ldrSize = static_cast<VkDeviceSize>(m_PixelBuffer.size());

    BufferDesc uploadDesc{};
    uploadDesc.size = static_cast<size_t>(ldrSize);
    uploadDesc.usage = BufferUsage::Staging;
    uploadDesc.hostVisible = true;
    uploadDesc.debugName = finalPass ? "FinalRenderUpload_Final" : "FinalRenderUpload_Preview";

    Buffer uploadBuffer;
    if (!uploadBuffer.Init(device, uploadDesc)) {
        LUCENT_CORE_ERROR("FinalRender: Failed to create upload buffer for preview");
        return false;
    }
    uploadBuffer.Upload(m_PixelBuffer.data(), static_cast<size_t>(ldrSize));

    VkCommandBuffer uploadCmd = device->BeginSingleTimeCommands();

    // Transition output image for copy
    VkImageLayout curLayout = m_RenderImage.GetCurrentLayout();
    if (curLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        m_RenderImage.TransitionLayout(uploadCmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    } else if (curLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        m_RenderImage.TransitionLayout(uploadCmd, curLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {m_Config.width, m_Config.height, 1};

    vkCmdCopyBufferToImage(
        uploadCmd,
        uploadBuffer.GetHandle(),
        m_RenderImage.GetHandle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copyRegion
    );

    // Transition for sampling in ImGui
    m_RenderImage.TransitionLayout(uploadCmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    device->EndSingleTimeCommands(uploadCmd);
    uploadBuffer.Shutdown();

    return true;
}

bool FinalRender::ExportImage(const std::string& path) {
    if (m_PixelBuffer.empty()) {
        LUCENT_CORE_ERROR("FinalRender: No image to export");
        return false;
    }
    
    // Determine format from extension
    std::string ext = path.substr(path.find_last_of('.') + 1);
    
    int result = 0;
    
    if (ext == "png" || ext == "PNG") {
        result = stbi_write_png(path.c_str(), m_Config.width, m_Config.height, 4, 
                                 m_PixelBuffer.data(), m_Config.width * 4);
    } else if (ext == "jpg" || ext == "jpeg" || ext == "JPG" || ext == "JPEG") {
        result = stbi_write_jpg(path.c_str(), m_Config.width, m_Config.height, 4,
                                 m_PixelBuffer.data(), 95);
    } else if (ext == "bmp" || ext == "BMP") {
        result = stbi_write_bmp(path.c_str(), m_Config.width, m_Config.height, 4,
                                 m_PixelBuffer.data());
    } else {
        // Default to PNG
        result = stbi_write_png(path.c_str(), m_Config.width, m_Config.height, 4,
                                 m_PixelBuffer.data(), m_Config.width * 4);
    }
    
    if (result) {
        LUCENT_CORE_INFO("FinalRender: Exported to {}", path);
        return true;
    } else {
        LUCENT_CORE_ERROR("FinalRender: Failed to export to {}", path);
        return false;
    }
}

bool FinalRender::SaveToPNG(const std::string& path) {
    return stbi_write_png(path.c_str(), m_Config.width, m_Config.height, 4,
                           m_PixelBuffer.data(), m_Config.width * 4) != 0;
}

bool FinalRender::SaveToEXR(const std::string& path) {
    (void)path;  // Suppress unused parameter warning
    // EXR export would require a library like tinyexr
    LUCENT_CORE_WARN("FinalRender: EXR export not yet implemented");
    return false;
}

} // namespace lucent::gfx
