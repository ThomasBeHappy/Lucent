#pragma once

#include "lucent/gfx/Device.h"
#include "lucent/gfx/Image.h"
#include "lucent/gfx/Buffer.h"
#include <string>
#include <vector>

namespace lucent::gfx {

// Environment map for HDR IBL lighting with importance sampling
// Uses equirectangular projection
class EnvironmentMap : public NonCopyable {
public:
    EnvironmentMap() = default;
    ~EnvironmentMap();
    
    // Load HDR environment from file
    bool LoadFromFile(Device* device, const std::string& path);
    
    // Create a default procedural sky (gradient)
    bool CreateDefaultSky(Device* device);
    
    void Shutdown();
    
    // Getters
    bool IsLoaded() const { return m_Loaded; }
    VkImageView GetEnvView() const { return m_EnvImage.GetView(); }
    VkSampler GetSampler() const { return m_Sampler; }
    
    // CDF textures for importance sampling
    VkImageView GetMarginalCDFView() const { return m_MarginalCDF.GetView(); }
    VkImageView GetConditionalCDFView() const { return m_ConditionalCDF.GetView(); }
    
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    const std::string& GetPath() const { return m_Path; }
    
    // Environment settings (applied in shader)
    float GetIntensity() const { return m_Intensity; }
    void SetIntensity(float v) { m_Intensity = v; }
    
    float GetRotation() const { return m_Rotation; }  // Rotation around Y axis (radians)
    void SetRotation(float r) { m_Rotation = r; }
    
private:
    bool BuildImportanceSamplingTables(const std::vector<float>& luminance);
    bool CreateSampler();
    
private:
    Device* m_Device = nullptr;
    
    Image m_EnvImage;                    // HDR environment texture
    Image m_MarginalCDF;                 // 1D CDF for rows (height x 1)
    Image m_ConditionalCDF;              // 2D CDF for columns per row (width x height)
    VkSampler m_Sampler = VK_NULL_HANDLE;
    
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    std::string m_Path;
    
    float m_Intensity = 1.0f;
    float m_Rotation = 0.0f;
    
    bool m_Loaded = false;
};

} // namespace lucent::gfx

