#pragma once

#include "lucent/core/Core.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Image.h"
#include <string>

namespace lucent::assets {

enum class TextureType {
    Albedo,
    Normal,
    Roughness,
    Metallic,
    AO,
    Emissive,
    Height,
    Generic
};

enum class TextureFormat {
    RGBA8_SRGB,      // For albedo/emissive (gamma-corrected)
    RGBA8_UNORM,     // For normal maps, roughness, metallic (linear)
    R8_UNORM,        // Single channel (roughness, metallic, AO)
    RGBA16_SFLOAT,   // HDR textures
    RGBA32_SFLOAT    // HDR high precision
};

struct TextureDesc {
    std::string path;
    TextureType type = TextureType::Generic;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    bool generateMips = true;
    bool flipVertically = true;
    const char* debugName = nullptr;
};

class Texture : public NonCopyable {
public:
    Texture() = default;
    ~Texture();
    
    // Load from file
    bool LoadFromFile(gfx::Device* device, const TextureDesc& desc);
    
    // Create from raw data
    bool CreateFromData(gfx::Device* device, const void* data, 
                        uint32_t width, uint32_t height, uint32_t channels,
                        TextureFormat format, const std::string& name = "Texture");
    
    // Create a solid color texture (1x1)
    bool CreateSolidColor(gfx::Device* device, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                          const std::string& name = "SolidColor");
    
    void Destroy();
    
    // Getters
    gfx::Image* GetImage() { return &m_Image; }
    VkImageView GetView() const { return m_Image.GetView(); }
    VkSampler GetSampler() const { return m_Sampler; }
    
    uint32_t GetWidth() const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }
    uint32_t GetMipLevels() const { return m_MipLevels; }
    const std::string& GetName() const { return m_Name; }
    const std::string& GetPath() const { return m_Path; }
    TextureType GetType() const { return m_Type; }
    
private:
    bool CreateSampler();
    void GenerateMipmaps(VkCommandBuffer cmd);
    
private:
    gfx::Device* m_Device = nullptr;
    gfx::Image m_Image;
    VkSampler m_Sampler = VK_NULL_HANDLE;
    
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_MipLevels = 1;
    
    std::string m_Name;
    std::string m_Path;
    TextureType m_Type = TextureType::Generic;
};

// Default textures for when no texture is specified
class DefaultTextures : public NonCopyable {
public:
    static DefaultTextures& Get() {
        static DefaultTextures instance;
        return instance;
    }
    
    bool Init(gfx::Device* device);
    void Shutdown();
    
    Texture* GetWhite() { return &m_White; }
    Texture* GetBlack() { return &m_Black; }
    Texture* GetNormal() { return &m_Normal; }  // Flat normal (128, 128, 255)
    Texture* GetRoughness() { return &m_Roughness; }  // Mid-gray (128)
    
private:
    DefaultTextures() = default;
    
    Texture m_White;
    Texture m_Black;
    Texture m_Normal;
    Texture m_Roughness;
    bool m_Initialized = false;
};

} // namespace lucent::assets

