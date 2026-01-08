#include "lucent/material/MaterialAsset.h"
#include "lucent/gfx/PipelineBuilder.h"
#include "lucent/core/Log.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>
#include <algorithm>
#include <chrono>

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

void MaterialAsset::RequestRecompileAsync() {
    if (!m_Device) {
        m_CompileError = "No device";
        m_Valid = false;
        return;
    }
    
    // If a compile is already running, just queue another pass (we'll snapshot the latest graph later).
    if (m_AsyncCompiling.load()) {
        m_AsyncRecompileQueued.store(true);
        return;
    }
    
    // Snapshot graph for background compilation so the UI can keep editing safely.
    // (MaterialGraph is value-copyable.)
    MaterialGraph snapshot = m_Graph;
    
    m_AsyncCompiling.store(true);
    m_AsyncRecompileQueued.store(false);
    
    m_AsyncCompileFuture = std::async(std::launch::async, [snapshot]() mutable {
        MaterialCompiler compiler;
        return compiler.Compile(snapshot);
    });
}

void MaterialAsset::PumpAsyncRecompile() {
    if (!m_AsyncCompiling.load()) return;
    if (!m_AsyncCompileFuture.valid()) {
        m_AsyncCompiling.store(false);
        return;
    }
    
    using namespace std::chrono_literals;
    if (m_AsyncCompileFuture.wait_for(0ms) != std::future_status::ready) {
        return;
    }
    
    CompileResult result{};
    try {
        result = m_AsyncCompileFuture.get();
    } catch (const std::exception& e) {
        m_CompileError = std::string("Async compile exception: ") + e.what();
        m_Valid = false;
        m_AsyncCompiling.store(false);
        return;
    } catch (...) {
        m_CompileError = "Async compile exception: unknown";
        m_Valid = false;
        m_AsyncCompiling.store(false);
        return;
    }
    m_AsyncCompiling.store(false);
    
    if (!result.success) {
        // Keep old pipeline alive; just report error.
        m_CompileError = result.errorMessage;
        m_Valid = false;
    } else {
        // If unchanged, still clear dirty (important for params that previously didn't affect hash)
        if (result.graphHash == m_GraphHash && m_Pipeline != VK_NULL_HANDLE) {
            m_Valid = true;
            m_CompileError.clear();
            m_Dirty = false;
        } else {
            m_GraphHash = result.graphHash;
            
            if (!CreatePipeline(result.fragmentShaderSPIRV)) {
                m_CompileError = "Failed to create pipeline";
                m_Valid = false;
            } else {
                m_Valid = true;
                m_CompileError.clear();
                m_Dirty = false;
            }
        }
    }
    
    // If edits happened while compiling (or graph diverged), run one more pass.
    const uint64_t currentHash = m_Graph.ComputeHash();
    if (m_AsyncRecompileQueued.load() || (m_Valid && currentHash != m_GraphHash)) {
        m_AsyncRecompileQueued.store(false);
        RequestRecompileAsync();
    }
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
    
    // Allocate + write descriptor set for textures
    if (m_DescriptorSetLayout != VK_NULL_HANDLE && !textureSlots.empty()) {
        // Create a small descriptor pool for this material
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = static_cast<uint32_t>(textureSlots.size());
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to create material descriptor pool");
            return false;
        }
        
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_DescriptorSetLayout;
        
        if (vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet) != VK_SUCCESS) {
            LUCENT_CORE_ERROR("Failed to allocate material descriptor set");
            return false;
        }
        
        // Load textures and write descriptors
        m_Textures.clear();
        m_Textures.reserve(textureSlots.size());
        
        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(textureSlots.size());
        
        for (size_t i = 0; i < textureSlots.size(); ++i) {
            const auto& slot = textureSlots[i];
            
            auto tex = std::make_unique<assets::Texture>();
            assets::TextureDesc desc{};
            desc.path = slot.path;
            desc.type = assets::TextureType::Generic;
            desc.format = slot.sRGB ? assets::TextureFormat::RGBA8_SRGB : assets::TextureFormat::RGBA8_UNORM;
            desc.generateMips = true;
            desc.flipVertically = true;
            desc.debugName = slot.path.c_str();
            
            if (!slot.path.empty() && tex->LoadFromFile(m_Device, desc)) {
                // ok
            } else {
                // Fallback: solid magenta to make missing textures obvious
                tex->CreateSolidColor(m_Device, 255, 0, 255, 255, "MissingTexture");
            }
            
            VkDescriptorImageInfo info{};
            info.sampler = tex->GetSampler();
            info.imageView = tex->GetView();
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos.push_back(info);
            
            m_Textures.push_back(std::move(tex));
        }
        
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = static_cast<uint32_t>(imageInfos.size());
        write.pImageInfo = imageInfos.data();
        
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    
    // Create pipeline layout with push constants (same as mesh pipeline)
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    // Match Renderer mesh pipeline push constant range (256 bytes).
    // Renderer pushes extra settings (shadow/tonemap/etc) even for material pipelines.
    pushConstant.size = sizeof(float) * 64; // 256 bytes
    
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

    // Safety-first: materials can recompile while the main renderer is still using the old
    // pipeline / descriptor set on in-flight command buffers. Waiting avoids DEVICE_LOST.
    if (m_Pipeline != VK_NULL_HANDLE ||
        m_PipelineLayout != VK_NULL_HANDLE ||
        m_DescriptorPool != VK_NULL_HANDLE ||
        m_DescriptorSetLayout != VK_NULL_HANDLE ||
        m_VertexShaderModule != VK_NULL_HANDLE ||
        m_FragmentShaderModule != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }
    
    // Destroy material textures + descriptor pool
    m_Textures.clear();
    if (m_DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
        m_DescriptorPool = VK_NULL_HANDLE;
    }
    m_DescriptorSet = VK_NULL_HANDLE;
    
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

void MaterialAssetManager::PumpAsyncCompiles() {
    if (m_DefaultMaterial) {
        m_DefaultMaterial->PumpAsyncRecompile();
    }
    for (auto& [path, material] : m_Materials) {
        if (material) {
            material->PumpAsyncRecompile();
        }
    }
}

} // namespace lucent::material

