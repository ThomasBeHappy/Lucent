#pragma once

#include "lucent/gfx/VulkanContext.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Image.h"
#include "lucent/gfx/RenderSettings.h"
#include "lucent/gfx/TracerCompute.h"
#include <glm/glm.hpp>
#include <string>
#include <functional>
#include <atomic>

namespace lucent::gfx {

// Forward declarations
class Renderer;
class TracerCompute;
class TracerRayKHR;

// Final render job configuration
struct FinalRenderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t samples = 128;
    uint32_t maxBounces = 4;
    float exposure = 1.0f;
    TonemapOperator tonemap = TonemapOperator::ACES;
    float gamma = 2.2f;
    std::string outputPath = "render.png";
    bool useRayTracing = true;  // Use RayTraced if available, else Traced
};

// Render progress callback
using RenderProgressCallback = std::function<void(uint32_t currentSample, uint32_t totalSamples, float timeElapsed)>;

// Final render job status
enum class FinalRenderStatus {
    Idle,
    Rendering,
    Completed,
    Cancelled,
    Failed
};

// Final render job
class FinalRender {
public:
    FinalRender() = default;
    ~FinalRender();
    
    bool Init(Renderer* renderer);
    void Shutdown();
    
    // Start a render job
    bool Start(const FinalRenderConfig& config, const GPUCamera& camera,
               const std::vector<BVHBuilder::Triangle>& triangles,
               const std::vector<GPUMaterial>& materials);
    
    // Cancel current render
    void Cancel();
    
    // Render one sample (call each frame while rendering)
    bool RenderSample();
    
    // Check status
    FinalRenderStatus GetStatus() const { return m_Status; }
    uint32_t GetCurrentSample() const { return m_CurrentSample; }
    uint32_t GetTotalSamples() const { return m_Config.samples; }
    float GetProgress() const;
    float GetElapsedTime() const;
    
    // Set progress callback
    void SetProgressCallback(RenderProgressCallback callback) { m_ProgressCallback = callback; }
    
    // Export the result
    bool ExportImage(const std::string& path);
    
    // Get the render image
    Image* GetRenderImage() { return &m_RenderImage; }
    
private:
    bool CreateRenderResources();
    void DestroyRenderResources();
    bool ApplyTonemap();
    bool SaveToPNG(const std::string& path);
    bool SaveToEXR(const std::string& path);
    
private:
    Renderer* m_Renderer = nullptr;
    
    FinalRenderConfig m_Config;
    GPUCamera m_Camera;
    FinalRenderStatus m_Status = FinalRenderStatus::Idle;
    
    // Render resources
    Image m_RenderImage;      // Tonemapped output
    Image m_AccumImage;       // HDR accumulation
    std::vector<uint8_t> m_PixelBuffer;  // For CPU readback
    
    // Progress tracking
    uint32_t m_CurrentSample = 0;
    double m_StartTime = 0.0;
    RenderProgressCallback m_ProgressCallback;
    
    // Scene data
    std::vector<BVHBuilder::Triangle> m_Triangles;
    std::vector<GPUMaterial> m_Materials;
    
    std::atomic<bool> m_CancelRequested{false};
};

} // namespace lucent::gfx

