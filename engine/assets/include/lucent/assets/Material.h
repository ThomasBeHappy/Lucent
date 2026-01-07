#pragma once

#include "lucent/core/Core.h"
#include "lucent/gfx/Device.h"
#include "lucent/gfx/Buffer.h"
#include "lucent/assets/Texture.h"
#include <glm/glm.hpp>
#include <string>
#include <memory>

namespace lucent::assets {

// GPU-compatible material data (must match shader layout)
struct MaterialData {
    glm::vec4 baseColor = glm::vec4(1.0f);       // RGB + alpha
    glm::vec4 emissive = glm::vec4(0.0f);        // RGB + intensity
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    float normalScale = 1.0f;
    
    // Texture flags (1 = has texture, 0 = use constant)
    uint32_t hasAlbedoTex = 0;
    uint32_t hasNormalTex = 0;
    uint32_t hasMetallicRoughnessTex = 0;
    uint32_t hasEmissiveTex = 0;
};

class Material : public NonCopyable {
public:
    Material() = default;
    ~Material();
    
    bool Create(gfx::Device* device, const std::string& name = "Material");
    void Destroy();
    
    // Update GPU buffer with current parameters
    void UpdateBuffer();
    
    // Parameters
    void SetBaseColor(const glm::vec3& color) { m_Data.baseColor = glm::vec4(color, m_Data.baseColor.a); m_Dirty = true; }
    void SetBaseColor(const glm::vec4& color) { m_Data.baseColor = color; m_Dirty = true; }
    void SetMetallic(float metallic) { m_Data.metallic = metallic; m_Dirty = true; }
    void SetRoughness(float roughness) { m_Data.roughness = roughness; m_Dirty = true; }
    void SetEmissive(const glm::vec3& color, float intensity = 1.0f) { 
        m_Data.emissive = glm::vec4(color, intensity); 
        m_Dirty = true; 
    }
    void SetNormalScale(float scale) { m_Data.normalScale = scale; m_Dirty = true; }
    void SetAO(float ao) { m_Data.ao = ao; m_Dirty = true; }
    
    // Getters
    glm::vec4 GetBaseColor() const { return m_Data.baseColor; }
    float GetMetallic() const { return m_Data.metallic; }
    float GetRoughness() const { return m_Data.roughness; }
    glm::vec4 GetEmissive() const { return m_Data.emissive; }
    
    const MaterialData& GetData() const { return m_Data; }
    gfx::Buffer* GetBuffer() { return &m_Buffer; }
    const std::string& GetName() const { return m_Name; }
    
    // Textures (optional)
    void SetAlbedoTexture(Texture* tex) { m_AlbedoTex = tex; m_Data.hasAlbedoTex = tex ? 1 : 0; m_Dirty = true; }
    void SetNormalTexture(Texture* tex) { m_NormalTex = tex; m_Data.hasNormalTex = tex ? 1 : 0; m_Dirty = true; }
    void SetMetallicRoughnessTexture(Texture* tex) { m_MetallicRoughnessTex = tex; m_Data.hasMetallicRoughnessTex = tex ? 1 : 0; m_Dirty = true; }
    void SetEmissiveTexture(Texture* tex) { m_EmissiveTex = tex; m_Data.hasEmissiveTex = tex ? 1 : 0; m_Dirty = true; }
    
    Texture* GetAlbedoTexture() const { return m_AlbedoTex; }
    Texture* GetNormalTexture() const { return m_NormalTex; }
    Texture* GetMetallicRoughnessTexture() const { return m_MetallicRoughnessTex; }
    Texture* GetEmissiveTexture() const { return m_EmissiveTex; }
    
private:
    gfx::Device* m_Device = nullptr;
    gfx::Buffer m_Buffer;
    
    MaterialData m_Data;
    std::string m_Name;
    bool m_Dirty = true;
    
    // Texture references (not owned)
    Texture* m_AlbedoTex = nullptr;
    Texture* m_NormalTex = nullptr;
    Texture* m_MetallicRoughnessTex = nullptr;
    Texture* m_EmissiveTex = nullptr;
};

// Material library for managing named materials
class MaterialLibrary : public NonCopyable {
public:
    static MaterialLibrary& Get() {
        static MaterialLibrary instance;
        return instance;
    }
    
    bool Init(gfx::Device* device);
    void Shutdown();
    
    Material* CreateMaterial(const std::string& name);
    Material* GetMaterial(const std::string& name);
    Material* GetDefaultMaterial() { return m_DefaultMaterial.get(); }
    
    const auto& GetAllMaterials() const { return m_Materials; }
    
private:
    MaterialLibrary() = default;
    
    gfx::Device* m_Device = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Material>> m_Materials;
    std::unique_ptr<Material> m_DefaultMaterial;
};

} // namespace lucent::assets

