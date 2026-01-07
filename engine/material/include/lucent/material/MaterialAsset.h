#pragma once

#include "lucent/material/MaterialGraph.h"
#include "lucent/material/MaterialCompiler.h"
#include "lucent/gfx/Device.h"
#include <vulkan/vulkan.h>
#include <string>
#include <memory>
#include <unordered_map>

namespace lucent::material {

// Forward declaration
class MaterialAssetManager;

// A compiled material ready for rendering
class MaterialAsset {
public:
    MaterialAsset() = default;
    ~MaterialAsset();
    
    // Initialize with a device
    bool Init(gfx::Device* device);
    void Shutdown();
    
    // Access the graph for editing
    MaterialGraph& GetGraph() { return m_Graph; }
    const MaterialGraph& GetGraph() const { return m_Graph; }
    
    // Recompile the material (call after editing the graph)
    bool Recompile();
    
    // Check if the material is valid (compiled successfully)
    bool IsValid() const { return m_Valid; }
    
    // Get compile error message
    const std::string& GetCompileError() const { return m_CompileError; }
    
    // Get the compiled fragment shader
    VkShaderModule GetFragmentShaderModule() const { return m_FragmentShaderModule; }
    
    // Get the pipeline for this material
    VkPipeline GetPipeline() const { return m_Pipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
    
    // Get descriptor set for material textures
    VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }
    
    // Graph hash for cache lookup
    uint64_t GetGraphHash() const { return m_GraphHash; }
    
    // File path for asset management
    const std::string& GetFilePath() const { return m_FilePath; }
    void SetFilePath(const std::string& path) { m_FilePath = path; }
    
    // Dirty flag (needs recompile or save)
    bool IsDirty() const { return m_Dirty; }
    void MarkDirty() { m_Dirty = true; }
    void ClearDirty() { m_Dirty = false; }
    
private:
    bool CreatePipeline(const std::vector<uint32_t>& fragmentSpirv);
    void DestroyPipeline();
    
    gfx::Device* m_Device = nullptr;
    MaterialGraph m_Graph;
    MaterialCompiler m_Compiler;
    
    bool m_Valid = false;
    bool m_Dirty = true;
    std::string m_CompileError;
    std::string m_FilePath;
    uint64_t m_GraphHash = 0;
    
    // Vulkan resources
    VkShaderModule m_VertexShaderModule = VK_NULL_HANDLE;
    VkShaderModule m_FragmentShaderModule = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
};

// Manager for material assets (caching, loading, saving)
class MaterialAssetManager {
public:
    static MaterialAssetManager& Get() {
        static MaterialAssetManager instance;
        return instance;
    }
    
    bool Init(gfx::Device* device);
    void Shutdown();
    
    // Create a new material
    MaterialAsset* CreateMaterial(const std::string& name = "New Material");
    
    // Load material from file
    MaterialAsset* LoadMaterial(const std::string& path);
    
    // Save material to file
    bool SaveMaterial(MaterialAsset* material, const std::string& path);
    
    // Get material by path (loads if not cached)
    MaterialAsset* GetMaterial(const std::string& path);
    
    // Get default material (fallback)
    MaterialAsset* GetDefaultMaterial() { return m_DefaultMaterial.get(); }
    
    // Reload all materials (after shader changes)
    void RecompileAll();
    
private:
    MaterialAssetManager() = default;
    
    gfx::Device* m_Device = nullptr;
    std::unordered_map<std::string, std::unique_ptr<MaterialAsset>> m_Materials;
    std::unique_ptr<MaterialAsset> m_DefaultMaterial;
};

} // namespace lucent::material

