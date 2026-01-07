#include "lucent/assets/Material.h"
#include "lucent/core/Log.h"

namespace lucent::assets {

Material::~Material() {
    Destroy();
}

bool Material::Create(gfx::Device* device, const std::string& name) {
    m_Device = device;
    m_Name = name;
    
    // Create uniform buffer for material data
    gfx::BufferDesc bufferDesc{};
    bufferDesc.size = sizeof(MaterialData);
    bufferDesc.usage = gfx::BufferUsage::Uniform;
    bufferDesc.hostVisible = true;
    bufferDesc.debugName = (name + "_MaterialBuffer").c_str();
    
    if (!m_Buffer.Init(device, bufferDesc)) {
        LUCENT_CORE_ERROR("Failed to create material buffer: {}", name);
        return false;
    }
    
    // Upload initial data
    UpdateBuffer();
    
    LUCENT_CORE_DEBUG("Created material: {}", name);
    return true;
}

void Material::Destroy() {
    m_Buffer.Shutdown();
}

void Material::UpdateBuffer() {
    if (!m_Dirty) return;
    
    m_Buffer.Upload(&m_Data, sizeof(MaterialData));
    m_Dirty = false;
}

// ============================================================================
// MaterialLibrary
// ============================================================================

bool MaterialLibrary::Init(gfx::Device* device) {
    m_Device = device;
    
    // Create default material
    m_DefaultMaterial = std::make_unique<Material>();
    if (!m_DefaultMaterial->Create(device, "Default")) {
        return false;
    }
    
    // Set default material properties (neutral gray, slightly rough)
    m_DefaultMaterial->SetBaseColor(glm::vec3(0.8f));
    m_DefaultMaterial->SetMetallic(0.0f);
    m_DefaultMaterial->SetRoughness(0.5f);
    m_DefaultMaterial->UpdateBuffer();
    
    LUCENT_CORE_INFO("Material library initialized");
    return true;
}

void MaterialLibrary::Shutdown() {
    m_Materials.clear();
    m_DefaultMaterial.reset();
}

Material* MaterialLibrary::CreateMaterial(const std::string& name) {
    auto it = m_Materials.find(name);
    if (it != m_Materials.end()) {
        LUCENT_CORE_WARN("Material '{}' already exists", name);
        return it->second.get();
    }
    
    auto material = std::make_unique<Material>();
    if (!material->Create(m_Device, name)) {
        return nullptr;
    }
    
    Material* ptr = material.get();
    m_Materials[name] = std::move(material);
    return ptr;
}

Material* MaterialLibrary::GetMaterial(const std::string& name) {
    auto it = m_Materials.find(name);
    if (it != m_Materials.end()) {
        return it->second.get();
    }
    return nullptr;
}

} // namespace lucent::assets

