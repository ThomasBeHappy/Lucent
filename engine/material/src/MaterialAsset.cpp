#include "lucent/material/MaterialAsset.h"
#include "lucent/gfx/PipelineBuilder.h"
#include "lucent/core/Log.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>
#include <algorithm>

namespace lucent::material {

MaterialAsset::~MaterialAsset() {
    Shutdown();
}

bool MaterialAsset::Init(gfx::Device* device) {
    m_Device = device;
    m_Graph.CreateDefault();
    return true;
}

void MaterialAsset::Shutdown() {
    DestroyPipeline();
    m_Device = nullptr;
}

bool MaterialAsset::Recompile() {
    if (!m_Device) {
        m_CompileError = "No device";
        m_Valid = false;
        return false;
    }
    
    // Compile the graph
    CompileResult result = m_Compiler.Compile(m_Graph);
    
    if (!result.success) {
        m_CompileError = result.errorMessage;
        m_Valid = false;
        LUCENT_CORE_ERROR("Material compile failed: {}", m_CompileError);
        return false;
    }
    
    // Check if hash changed
    if (result.graphHash == m_GraphHash && m_Pipeline != VK_NULL_HANDLE) {
        // No change, skip pipeline creation
        return true;
    }
    
    m_GraphHash = result.graphHash;
    
    // Create pipeline
    if (!CreatePipeline(result.fragmentShaderSPIRV)) {
        m_CompileError = "Failed to create pipeline";
        m_Valid = false;
        return false;
    }
    
    m_Valid = true;
    m_CompileError.clear();
    m_Dirty = false;
    
    LUCENT_CORE_INFO("Material compiled successfully: {}", m_Graph.GetName());
    return true;
}

bool MaterialAsset::CreatePipeline(const std::vector<uint32_t>& fragmentSpirv) {
    VkDevice device = m_Device->GetHandle();
    
    // Destroy old pipeline if exists
    DestroyPipeline();
    
    // Get standard vertex shader
    const auto& vertexSpirv = MaterialCompiler::GetStandardVertexShaderSPIRV();
    if (vertexSpirv.empty()) {
        LUCENT_CORE_ERROR("Failed to get standard vertex shader");
        return false;
    }
    
    // Create shader modules
    VkShaderModuleCreateInfo vertModuleInfo{};
    vertModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertModuleInfo.codeSize = vertexSpirv.size() * sizeof(uint32_t);
    vertModuleInfo.pCode = vertexSpirv.data();
    
    if (vkCreateShaderModule(device, &vertModuleInfo, nullptr, &m_VertexShaderModule) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create vertex shader module");
        return false;
    }
    
    VkShaderModuleCreateInfo fragModuleInfo{};
    fragModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragModuleInfo.codeSize = fragmentSpirv.size() * sizeof(uint32_t);
    fragModuleInfo.pCode = fragmentSpirv.data();
    
    if (vkCreateShaderModule(device, &fragModuleInfo, nullptr, &m_FragmentShaderModule) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create fragment shader module");
        vkDestroyShaderModule(device, m_VertexShaderModule, nullptr);
        m_VertexShaderModule = VK_NULL_HANDLE;
        return false;
    }
    
    // Create descriptor set layout for textures (if material uses textures)
    const auto& textureSlots = m_Graph.GetTextureSlots();
    if (!textureSlots.empty()) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = static_cast<uint32_t>(textureSlots.size());
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout) != VK_SUCCESS) {
            LUCENT_CORE_WARN("Failed to create material descriptor set layout");
        }
    }
    
    // Create pipeline layout with push constants (same as mesh pipeline)
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(float) * 48; // 2 mat4 + 4 vec4
    
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    
    if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_DescriptorSetLayout;
    }
    
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS) {
        LUCENT_CORE_ERROR("Failed to create material pipeline layout");
        return false;
    }
    
    // Vertex input (same as mesh pipeline)
    VkVertexInputBindingDescription meshBinding{};
    meshBinding.binding = 0;
    meshBinding.stride = sizeof(float) * 12; // position(3) + normal(3) + uv(2) + tangent(4)
    meshBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    
    std::vector<VkVertexInputAttributeDescription> meshAttributes = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },              // position
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 },  // normal
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6 },     // uv
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8 } // tangent
    };
    
    // Build pipeline using PipelineBuilder
    gfx::PipelineBuilder builder;
    builder
        .AddShaderStage(VK_SHADER_STAGE_VERTEX_BIT, m_VertexShaderModule)
        .AddShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, m_FragmentShaderModule)
        .SetVertexInput({meshBinding}, meshAttributes)
        .SetColorAttachmentFormat(VK_FORMAT_R16G16B16A16_SFLOAT)
        .SetDepthAttachmentFormat(VK_FORMAT_D32_SFLOAT)
        .SetDepthStencil(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .SetRasterizer(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .SetLayout(m_PipelineLayout);
    
    // Set render pass for legacy mode (Vulkan 1.1/1.2)
    if (m_RenderPass != VK_NULL_HANDLE) {
        builder.SetRenderPass(m_RenderPass);
    }
    
    m_Pipeline = builder.Build(device);
    
    if (m_Pipeline == VK_NULL_HANDLE) {
        LUCENT_CORE_ERROR("Failed to create material pipeline");
        return false;
    }
    
    return true;
}

void MaterialAsset::DestroyPipeline() {
    if (!m_Device) return;
    
    VkDevice device = m_Device->GetHandle();
    
    if (m_Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_Pipeline, nullptr);
        m_Pipeline = VK_NULL_HANDLE;
    }
    if (m_PipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
        m_PipelineLayout = VK_NULL_HANDLE;
    }
    if (m_DescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);
        m_DescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_VertexShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_VertexShaderModule, nullptr);
        m_VertexShaderModule = VK_NULL_HANDLE;
    }
    if (m_FragmentShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_FragmentShaderModule, nullptr);
        m_FragmentShaderModule = VK_NULL_HANDLE;
    }
}

// ============================================================================
// MaterialAssetManager
// ============================================================================

bool MaterialAssetManager::Init(gfx::Device* device, const std::string& assetsPath) {
    m_Device = device;
    
    // Set up materials directory
    m_MaterialsPath = assetsPath + "/materials";
    
    // Create materials directory if it doesn't exist
    try {
        std::filesystem::create_directories(m_MaterialsPath);
    } catch (const std::exception& e) {
        LUCENT_CORE_WARN("Could not create materials directory: {}", e.what());
    }
    
    // Create default material
    m_DefaultMaterial = std::make_unique<MaterialAsset>();
    if (!m_DefaultMaterial->Init(device)) {
        return false;
    }
    
    // Set render pass for legacy mode
    m_DefaultMaterial->SetRenderPass(m_RenderPass);
    
    // Compile default material
    if (!m_DefaultMaterial->Recompile()) {
        LUCENT_CORE_WARN("Default material failed to compile, using fallback");
    }
    
    LUCENT_CORE_INFO("Material asset manager initialized");
    return true;
}

void MaterialAssetManager::Shutdown() {
    m_Materials.clear();
    m_DefaultMaterial.reset();
    m_Device = nullptr;
}

std::string MaterialAssetManager::GenerateUniquePath(const std::string& baseName) {
    // Sanitize the name (remove special characters)
    std::string sanitized;
    for (char c : baseName) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == ' ') {
            sanitized += c;
        }
    }
    if (sanitized.empty()) sanitized = "Material";
    
    // Replace spaces with underscores
    std::replace(sanitized.begin(), sanitized.end(), ' ', '_');
    
    // Find a unique filename
    std::string basePath = m_MaterialsPath + "/" + sanitized;
    std::string path = basePath + ".lmat";
    
    int counter = 1;
    while (std::filesystem::exists(path)) {
        path = basePath + "_" + std::to_string(counter++) + ".lmat";
    }
    
    return path;
}

MaterialAsset* MaterialAssetManager::CreateMaterial(const std::string& name) {
    auto material = std::make_unique<MaterialAsset>();
    if (!material->Init(m_Device)) {
        return nullptr;
    }
    
    material->GetGraph().SetName(name);
    material->SetRenderPass(m_RenderPass);
    
    // Create default graph
    material->GetGraph().CreateDefault();
    
    // Generate unique file path and save immediately
    std::string filePath = GenerateUniquePath(name);
    material->SetFilePath(filePath);
    
    // Compile the material
    if (!material->Recompile()) {
        LUCENT_CORE_WARN("New material failed to compile");
    }
    
    // Save to disk
    MaterialAsset* ptr = material.get();
    if (SaveMaterial(ptr, filePath)) {
        LUCENT_CORE_INFO("Created material: {}", filePath);
    } else {
        LUCENT_CORE_WARN("Failed to save new material to: {}", filePath);
    }
    
    // Store in cache using file path as key
    m_Materials[filePath] = std::move(material);
    
    return ptr;
}

MaterialAsset* MaterialAssetManager::LoadMaterial(const std::string& path) {
    // Check if already loaded
    auto it = m_Materials.find(path);
    if (it != m_Materials.end()) {
        return it->second.get();
    }
    
    // Load from file
    std::ifstream file(path);
    if (!file.is_open()) {
        LUCENT_CORE_ERROR("Failed to open material file: {}", path);
        return nullptr;
    }
    
    auto material = std::make_unique<MaterialAsset>();
    if (!material->Init(m_Device)) {
        return nullptr;
    }
    
    material->SetFilePath(path);
    material->SetRenderPass(m_RenderPass);
    
    // Parse .lmat file
    MaterialGraph& graph = material->GetGraph();
    graph.Clear();
    
    std::string line;
    
    // Read header
    std::getline(file, line);
    if (line != "LUCENT_MATERIAL_V1") {
        LUCENT_CORE_ERROR("Invalid material file format");
        return nullptr;
    }
    
    // Read name
    std::getline(file, line);
    if (line.substr(0, 6) == "NAME: ") {
        graph.SetName(line.substr(6));
    }
    
    // Read nodes, links, textures
    // TODO: Full serialization
    
    file.close();
    
    // Create default graph for now
    graph.CreateDefault();
    
    // Compile
    material->Recompile();
    
    MaterialAsset* ptr = material.get();
    m_Materials[path] = std::move(material);
    
    return ptr;
}

bool MaterialAssetManager::SaveMaterial(MaterialAsset* material, const std::string& path) {
    if (!material) return false;
    
    std::ofstream file(path);
    if (!file.is_open()) {
        LUCENT_CORE_ERROR("Failed to open file for writing: {}", path);
        return false;
    }
    
    const MaterialGraph& graph = material->GetGraph();
    
    // Write header
    file << "LUCENT_MATERIAL_V1\n";
    file << "NAME: " << graph.GetName() << "\n\n";
    
    // Write nodes
    for (const auto& [id, node] : graph.GetNodes()) {
        file << "NODE_BEGIN\n";
        file << "  ID: " << id << "\n";
        file << "  TYPE: " << static_cast<int>(node.type) << "\n";
        file << "  POS: " << node.position.x << " " << node.position.y << "\n";
        
        // Write parameter based on type
        if (std::holds_alternative<float>(node.parameter)) {
            file << "  PARAM_FLOAT: " << std::get<float>(node.parameter) << "\n";
        } else if (std::holds_alternative<glm::vec3>(node.parameter)) {
            const auto& v = std::get<glm::vec3>(node.parameter);
            file << "  PARAM_VEC3: " << v.x << " " << v.y << " " << v.z << "\n";
        } else if (std::holds_alternative<std::string>(node.parameter)) {
            file << "  PARAM_STRING: " << std::get<std::string>(node.parameter) << "\n";
        }
        
        file << "NODE_END\n\n";
    }
    
    // Write links
    for (const auto& [id, link] : graph.GetLinks()) {
        file << "LINK: " << link.startPinId << " " << link.endPinId << "\n";
    }
    
    // Write texture slots
    for (size_t i = 0; i < graph.GetTextureSlots().size(); ++i) {
        const auto& slot = graph.GetTextureSlots()[i];
        file << "TEXTURE: " << i << " " << (slot.sRGB ? 1 : 0) << " " << slot.path << "\n";
    }
    
    file.close();
    
    material->SetFilePath(path);
    material->ClearDirty();
    
    LUCENT_CORE_INFO("Material saved: {}", path);
    return true;
}

MaterialAsset* MaterialAssetManager::GetMaterial(const std::string& path) {
    auto it = m_Materials.find(path);
    if (it != m_Materials.end()) {
        return it->second.get();
    }
    return LoadMaterial(path);
}

void MaterialAssetManager::RecompileAll() {
    if (m_DefaultMaterial) {
        m_DefaultMaterial->Recompile();
    }
    
    for (auto& [path, material] : m_Materials) {
        material->Recompile();
    }
    
    LUCENT_CORE_INFO("Recompiled all materials");
}

} // namespace lucent::material

