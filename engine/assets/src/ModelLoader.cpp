#include "lucent/assets/ModelLoader.h"
#include "lucent/core/Log.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE        // We already have stb_image in Texture.cpp
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <filesystem>

namespace lucent::assets {

// Helper to load image data for tinygltf using our own stb_image
static bool LoadImageData(tinygltf::Image* image, const int image_idx, std::string* err,
                          std::string* warn, int req_width, int req_height,
                          const unsigned char* bytes, int size, void* user_data) {
    (void)image_idx;
    (void)warn;
    (void)req_width;
    (void)req_height;
    (void)user_data;
    
    int width, height, channels;
    unsigned char* data = stbi_load_from_memory(bytes, size, &width, &height, &channels, 4);
    
    if (!data) {
        if (err) {
            *err = "Failed to load image: " + std::string(stbi_failure_reason());
        }
        return false;
    }
    
    image->width = width;
    image->height = height;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.resize(width * height * 4);
    memcpy(image->image.data(), data, width * height * 4);
    
    stbi_image_free(data);
    return true;
}

std::unique_ptr<Model> ModelLoader::LoadGLTF(gfx::Device* device, const std::string& path) {
    tinygltf::Model gltfModel;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    
    // Set custom image loader
    loader.SetImageLoader(LoadImageData, nullptr);
    
    std::filesystem::path filePath(path);
    std::string extension = filePath.extension().string();
    
    bool success = false;
    if (extension == ".glb" || extension == ".GLB") {
        success = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, path);
    } else {
        success = loader.LoadASCIIFromFile(&gltfModel, &err, &warn, path);
    }
    
    if (!warn.empty()) {
        LUCENT_CORE_WARN("glTF warning: {}", warn);
    }
    
    if (!success) {
        m_LastError = "Failed to load glTF: " + err;
        LUCENT_CORE_ERROR("{}", m_LastError);
        return nullptr;
    }
    
    auto model = std::make_unique<Model>();
    model->name = filePath.stem().string();
    model->sourcePath = path;
    
    std::string baseDir = filePath.parent_path().string();
    if (!baseDir.empty()) baseDir += "/";
    
    // Load textures
    for (size_t i = 0; i < gltfModel.textures.size(); i++) {
        const auto& gltfTex = gltfModel.textures[i];
        
        if (gltfTex.source < 0 || gltfTex.source >= static_cast<int>(gltfModel.images.size())) {
            model->textures.push_back(nullptr);
            continue;
        }
        
        const auto& gltfImage = gltfModel.images[gltfTex.source];
        
        auto texture = std::make_unique<Texture>();
        
        if (!gltfImage.image.empty()) {
            // Image data is embedded
            if (texture->CreateFromData(device, gltfImage.image.data(),
                                        gltfImage.width, gltfImage.height, 4,
                                        TextureFormat::RGBA8_SRGB,
                                        model->name + "_tex" + std::to_string(i))) {
                model->textures.push_back(std::move(texture));
            } else {
                model->textures.push_back(nullptr);
            }
        } else if (!gltfImage.uri.empty()) {
            // Load from file
            TextureDesc desc{};
            desc.path = baseDir + gltfImage.uri;
            desc.format = TextureFormat::RGBA8_SRGB;
            desc.generateMips = true;
            desc.debugName = (model->name + "_tex" + std::to_string(i)).c_str();
            
            if (texture->LoadFromFile(device, desc)) {
                model->textures.push_back(std::move(texture));
            } else {
                model->textures.push_back(nullptr);
            }
        } else {
            model->textures.push_back(nullptr);
        }
    }
    
    // Load materials
    for (size_t i = 0; i < gltfModel.materials.size(); i++) {
        const auto& gltfMat = gltfModel.materials[i];
        MaterialData mat;
        
        mat.name = gltfMat.name.empty() ? "Material_" + std::to_string(i) : gltfMat.name;
        
        // PBR Metallic-Roughness
        const auto& pbr = gltfMat.pbrMetallicRoughness;
        mat.baseColorFactor = glm::vec4(
            pbr.baseColorFactor[0],
            pbr.baseColorFactor[1],
            pbr.baseColorFactor[2],
            pbr.baseColorFactor[3]
        );
        mat.metallicFactor = static_cast<float>(pbr.metallicFactor);
        mat.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        
        if (pbr.baseColorTexture.index >= 0) {
            mat.baseColorTexture = pbr.baseColorTexture.index;
        }
        if (pbr.metallicRoughnessTexture.index >= 0) {
            mat.metallicRoughnessTexture = pbr.metallicRoughnessTexture.index;
        }
        
        // Normal map
        if (gltfMat.normalTexture.index >= 0) {
            mat.normalTexture = gltfMat.normalTexture.index;
        }
        
        // Occlusion
        if (gltfMat.occlusionTexture.index >= 0) {
            mat.occlusionTexture = gltfMat.occlusionTexture.index;
        }
        
        // Emissive
        if (gltfMat.emissiveTexture.index >= 0) {
            mat.emissiveTexture = gltfMat.emissiveTexture.index;
        }
        mat.emissiveFactor = glm::vec3(
            gltfMat.emissiveFactor[0],
            gltfMat.emissiveFactor[1],
            gltfMat.emissiveFactor[2]
        );
        
        // Alpha mode
        if (gltfMat.alphaMode == "OPAQUE") {
            mat.alphaMode = MaterialData::AlphaMode::Opaque;
        } else if (gltfMat.alphaMode == "MASK") {
            mat.alphaMode = MaterialData::AlphaMode::Mask;
            mat.alphaCutoff = static_cast<float>(gltfMat.alphaCutoff);
        } else if (gltfMat.alphaMode == "BLEND") {
            mat.alphaMode = MaterialData::AlphaMode::Blend;
        }
        
        mat.doubleSided = gltfMat.doubleSided;
        
        model->materials.push_back(mat);
    }
    
    // Add default material if none exist
    if (model->materials.empty()) {
        MaterialData defaultMat;
        defaultMat.name = "Default";
        model->materials.push_back(defaultMat);
    }
    
    // Load meshes
    for (size_t meshIdx = 0; meshIdx < gltfModel.meshes.size(); meshIdx++) {
        const auto& gltfMesh = gltfModel.meshes[meshIdx];
        
        std::vector<Vertex> allVertices;
        std::vector<uint32_t> allIndices;
        
        auto mesh = std::make_unique<Mesh>();
        
        for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); primIdx++) {
            const auto& prim = gltfMesh.primitives[primIdx];
            
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
                LUCENT_CORE_WARN("Skipping non-triangle primitive in mesh '{}'", gltfMesh.name);
                continue;
            }
            
            // Get accessors
            const float* positions = nullptr;
            const float* normals = nullptr;
            const float* texcoords = nullptr;
            const float* tangents = nullptr;
            size_t vertexCount = 0;
            
            // Position (required)
            auto posIt = prim.attributes.find("POSITION");
            if (posIt != prim.attributes.end()) {
                const auto& accessor = gltfModel.accessors[posIt->second];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                positions = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
                vertexCount = accessor.count;
            }
            
            // Normal
            auto normIt = prim.attributes.find("NORMAL");
            if (normIt != prim.attributes.end()) {
                const auto& accessor = gltfModel.accessors[normIt->second];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                normals = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            }
            
            // Texcoord
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end()) {
                const auto& accessor = gltfModel.accessors[uvIt->second];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                texcoords = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            }
            
            // Tangent
            auto tanIt = prim.attributes.find("TANGENT");
            if (tanIt != prim.attributes.end()) {
                const auto& accessor = gltfModel.accessors[tanIt->second];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                tangents = reinterpret_cast<const float*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            }
            
            // Build vertices
            uint32_t baseVertex = static_cast<uint32_t>(allVertices.size());
            
            for (size_t v = 0; v < vertexCount; v++) {
                Vertex vertex{};
                
                vertex.position = glm::vec3(
                    positions[v * 3 + 0],
                    positions[v * 3 + 1],
                    positions[v * 3 + 2]
                );
                
                if (normals) {
                    vertex.normal = glm::vec3(
                        normals[v * 3 + 0],
                        normals[v * 3 + 1],
                        normals[v * 3 + 2]
                    );
                } else {
                    vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                
                if (texcoords) {
                    vertex.uv = glm::vec2(
                        texcoords[v * 2 + 0],
                        texcoords[v * 2 + 1]
                    );
                }
                
                if (tangents) {
                    vertex.tangent = glm::vec4(
                        tangents[v * 4 + 0],
                        tangents[v * 4 + 1],
                        tangents[v * 4 + 2],
                        tangents[v * 4 + 3]
                    );
                } else {
                    vertex.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                }
                
                allVertices.push_back(vertex);
            }
            
            // Get indices
            uint32_t indexOffset = static_cast<uint32_t>(allIndices.size());
            
            if (prim.indices >= 0) {
                const auto& accessor = gltfModel.accessors[prim.indices];
                const auto& bufferView = gltfModel.bufferViews[accessor.bufferView];
                const auto& buffer = gltfModel.buffers[bufferView.buffer];
                const uint8_t* indexData = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
                
                for (size_t i = 0; i < accessor.count; i++) {
                    uint32_t index = 0;
                    
                    switch (accessor.componentType) {
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                            index = reinterpret_cast<const uint16_t*>(indexData)[i];
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                            index = reinterpret_cast<const uint32_t*>(indexData)[i];
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            index = indexData[i];
                            break;
                    }
                    
                    allIndices.push_back(baseVertex + index);
                }
                
                // Add submesh
                int32_t materialIndex = prim.material >= 0 ? prim.material : 0;
                mesh->AddSubmesh(indexOffset, static_cast<uint32_t>(accessor.count), materialIndex);
            }
        }
        
        // Create the mesh
        std::string meshName = gltfMesh.name.empty() ? 
            model->name + "_mesh" + std::to_string(meshIdx) : gltfMesh.name;
            
        if (mesh->Create(device, allVertices, allIndices, meshName)) {
            model->bounds.Expand(mesh->GetBounds().min);
            model->bounds.Expand(mesh->GetBounds().max);
            model->meshes.push_back(std::move(mesh));
        }
    }
    
    // Load nodes
    for (size_t nodeIdx = 0; nodeIdx < gltfModel.nodes.size(); nodeIdx++) {
        const auto& gltfNode = gltfModel.nodes[nodeIdx];
        NodeData node;
        
        node.name = gltfNode.name.empty() ? "Node_" + std::to_string(nodeIdx) : gltfNode.name;
        node.meshIndex = gltfNode.mesh;
        
        // Build local transform
        if (!gltfNode.matrix.empty()) {
            // Direct matrix
            for (int i = 0; i < 16; i++) {
                node.localTransform[i / 4][i % 4] = static_cast<float>(gltfNode.matrix[i]);
            }
        } else {
            // TRS
            glm::vec3 translation(0.0f);
            glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
            glm::vec3 scale(1.0f);
            
            if (!gltfNode.translation.empty()) {
                translation = glm::vec3(
                    gltfNode.translation[0],
                    gltfNode.translation[1],
                    gltfNode.translation[2]
                );
            }
            if (!gltfNode.rotation.empty()) {
                rotation = glm::quat(
                    static_cast<float>(gltfNode.rotation[3]),  // w
                    static_cast<float>(gltfNode.rotation[0]),  // x
                    static_cast<float>(gltfNode.rotation[1]),  // y
                    static_cast<float>(gltfNode.rotation[2])   // z
                );
            }
            if (!gltfNode.scale.empty()) {
                scale = glm::vec3(
                    gltfNode.scale[0],
                    gltfNode.scale[1],
                    gltfNode.scale[2]
                );
            }
            
            node.localTransform = glm::translate(glm::mat4(1.0f), translation)
                                * glm::mat4_cast(rotation)
                                * glm::scale(glm::mat4(1.0f), scale);
        }
        
        // Children
        for (int childIdx : gltfNode.children) {
            node.children.push_back(static_cast<uint32_t>(childIdx));
        }
        
        model->nodes.push_back(node);
    }
    
    // Find root nodes (from default scene or all nodes without parents)
    if (!gltfModel.scenes.empty()) {
        int sceneIdx = gltfModel.defaultScene >= 0 ? gltfModel.defaultScene : 0;
        const auto& scene = gltfModel.scenes[sceneIdx];
        for (int nodeIdx : scene.nodes) {
            model->rootNodes.push_back(static_cast<uint32_t>(nodeIdx));
        }
    } else {
        // Find nodes that aren't children of any other node
        std::vector<bool> isChild(model->nodes.size(), false);
        for (const auto& node : model->nodes) {
            for (uint32_t child : node.children) {
                if (child < isChild.size()) {
                    isChild[child] = true;
                }
            }
        }
        for (size_t i = 0; i < model->nodes.size(); i++) {
            if (!isChild[i]) {
                model->rootNodes.push_back(static_cast<uint32_t>(i));
            }
        }
    }
    
    LUCENT_CORE_INFO("Loaded glTF '{}': {} meshes, {} materials, {} textures, {} nodes",
        model->name, model->meshes.size(), model->materials.size(), 
        model->textures.size(), model->nodes.size());
    
    return model;
}

std::unique_ptr<Model> ModelLoader::LoadOBJ(gfx::Device* device, const std::string& path) {
    // TODO: Implement OBJ loading if needed
    (void)device;
    m_LastError = "OBJ loading not yet implemented: " + path;
    LUCENT_CORE_ERROR("{}", m_LastError);
    return nullptr;
}

} // namespace lucent::assets

