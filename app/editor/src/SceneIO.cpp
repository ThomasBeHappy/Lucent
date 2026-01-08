#include "SceneIO.h"
#include "lucent/scene/Components.h"
#include "lucent/mesh/EditableMesh.h"
#include "lucent/assets/ModelLoader.h"
#include "lucent/assets/MeshRegistry.h"
#include "lucent/core/Log.h"

#include <fstream>
#include <sstream>
#include <iomanip>

namespace lucent {
namespace SceneIO {

static std::string s_LastError;

const std::string& GetLastError() {
    return s_LastError;
}

static void WriteVec3(std::ostream& out, const glm::vec3& v) {
    out << std::fixed << std::setprecision(6) << v.x << " " << v.y << " " << v.z;
}

static glm::vec3 ReadVec3(std::istream& in) {
    glm::vec3 v;
    in >> v.x >> v.y >> v.z;
    return v;
}

bool SaveScene(scene::Scene* scene, const std::string& filepath) {
    if (!scene) {
        s_LastError = "Scene is null";
        return false;
    }
    
    std::ofstream file(filepath);
    if (!file.is_open()) {
        s_LastError = "Failed to open file for writing: " + filepath;
        return false;
    }
    
    // Header (V2 supports editable meshes)
    file << "LUCENT_SCENE_V2\n";
    file << "SCENE_NAME: " << scene->GetName() << "\n";
    file << "\n";
    
    // Entities
    for (scene::EntityID id : scene->GetEntities()) {
        scene::Entity entity = scene->GetEntity(id);
        
        file << "ENTITY_BEGIN\n";
        
        // Tag
        auto* tag = entity.GetComponent<scene::TagComponent>();
        if (tag) {
            file << "  NAME: " << tag->name << "\n";
        }
        
        // Transform
        auto* transform = entity.GetComponent<scene::TransformComponent>();
        if (transform) {
            file << "  TRANSFORM: ";
            WriteVec3(file, transform->position);
            file << " ";
            WriteVec3(file, transform->rotation);
            file << " ";
            WriteVec3(file, transform->scale);
            file << "\n";
        }
        
        // Camera
        auto* camera = entity.GetComponent<scene::CameraComponent>();
        if (camera) {
            file << "  CAMERA: " 
                 << static_cast<int>(camera->projectionType) << " "
                 << camera->fov << " "
                 << camera->orthoSize << " "
                 << camera->nearClip << " "
                 << camera->farClip << " "
                 << (camera->primary ? 1 : 0) << "\n";
        }
        
        // Light
        auto* light = entity.GetComponent<scene::LightComponent>();
        if (light) {
            file << "  LIGHT: " 
                 << static_cast<int>(light->type) << " ";
            WriteVec3(file, light->color);
            file << " " << light->intensity 
                 << " " << light->range
                 << " " << light->innerAngle
                 << " " << light->outerAngle
                 << " " << (light->castShadows ? 1 : 0) << "\n";
        }
        
        // Mesh Renderer
        auto* mesh = entity.GetComponent<scene::MeshRendererComponent>();
        if (mesh) {
            file << "  MESH_RENDERER: " 
                 << static_cast<int>(mesh->primitiveType) << " "
                 << (mesh->visible ? 1 : 0) << " "
                 << (mesh->castShadows ? 1 : 0) << " "
                 << (mesh->receiveShadows ? 1 : 0) << " ";
            WriteVec3(file, mesh->baseColor);
            file << " " << mesh->metallic 
                 << " " << mesh->roughness << " ";
            WriteVec3(file, mesh->emissive);
            file << " " << mesh->emissiveIntensity << "\n";
        }
        
        // Editable Mesh (V2 feature)
        auto* editMesh = entity.GetComponent<scene::EditableMeshComponent>();
        if (editMesh && editMesh->HasMesh()) {
            auto data = editMesh->mesh->Serialize();
            
            file << "  EDITABLE_MESH_BEGIN\n";
            file << "    VERTS: " << data.positions.size() << "\n";
            for (size_t i = 0; i < data.positions.size(); ++i) {
                file << "      ";
                WriteVec3(file, data.positions[i]);
                if (i < data.uvs.size()) {
                    file << " " << data.uvs[i].x << " " << data.uvs[i].y;
                } else {
                    file << " 0 0";
                }
                file << "\n";
            }
            
            file << "    FACES: " << data.faceVertexIndices.size() << "\n";
            for (const auto& face : data.faceVertexIndices) {
                file << "      " << face.size();
                for (uint32_t idx : face) {
                    file << " " << idx;
                }
                file << "\n";
            }
            file << "  EDITABLE_MESH_END\n";
        }
        
        file << "ENTITY_END\n\n";
    }
    
    file.close();
    LUCENT_CORE_INFO("Scene saved to: {}", filepath);
    return true;
}

bool LoadScene(scene::Scene* scene, const std::string& filepath) {
    if (!scene) {
        s_LastError = "Scene is null";
        return false;
    }
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        s_LastError = "Failed to open file: " + filepath;
        return false;
    }
    
    // Clear existing scene
    scene->Clear();
    
    std::string line;
    
    // Read header (support V1 and V2)
    std::getline(file, line);
    bool isV2 = (line == "LUCENT_SCENE_V2");
    if (line != "LUCENT_SCENE_V1" && line != "LUCENT_SCENE_V2") {
        s_LastError = "Invalid scene file format: " + line;
        return false;
    }
    
    if (isV2) {
        LUCENT_CORE_DEBUG("Loading scene format V2");
    } else {
        LUCENT_CORE_DEBUG("Loading scene format V1 (legacy)");
    }
    
    // Read scene name
    std::getline(file, line);
    if (line.substr(0, 12) == "SCENE_NAME: ") {
        scene->SetName(line.substr(12));
    }
    
    scene::Entity currentEntity;
    
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        
        if (line.empty()) continue;
        
        if (line == "ENTITY_BEGIN") {
            currentEntity = scene::Entity(); // Will be created when we read NAME
        }
        else if (line == "ENTITY_END") {
            currentEntity = scene::Entity();
        }
        else if (line.substr(0, 6) == "NAME: ") {
            std::string name = line.substr(6);
            currentEntity = scene->CreateEntity(name);
        }
        else if (line.substr(0, 11) == "TRANSFORM: " && currentEntity.IsValid()) {
            std::istringstream iss(line.substr(11));
            auto* transform = currentEntity.GetComponent<scene::TransformComponent>();
            if (transform) {
                transform->position = ReadVec3(iss);
                transform->rotation = ReadVec3(iss);
                transform->scale = ReadVec3(iss);
            }
        }
        else if (line.substr(0, 8) == "CAMERA: " && currentEntity.IsValid()) {
            std::istringstream iss(line.substr(8));
            int projType, primary;
            float fov, orthoSize, nearClip, farClip;
            iss >> projType >> fov >> orthoSize >> nearClip >> farClip >> primary;
            
            auto& cam = currentEntity.AddComponent<scene::CameraComponent>();
            cam.projectionType = static_cast<scene::CameraComponent::ProjectionType>(projType);
            cam.fov = fov;
            cam.orthoSize = orthoSize;
            cam.nearClip = nearClip;
            cam.farClip = farClip;
            cam.primary = (primary != 0);
        }
        else if (line.substr(0, 7) == "LIGHT: " && currentEntity.IsValid()) {
            std::istringstream iss(line.substr(7));
            int type, castShadows;
            float intensity, range, innerAngle, outerAngle;
            
            iss >> type;
            glm::vec3 color = ReadVec3(iss);
            iss >> intensity >> range >> innerAngle >> outerAngle >> castShadows;
            
            auto& light = currentEntity.AddComponent<scene::LightComponent>();
            light.type = static_cast<scene::LightType>(type);
            light.color = color;
            light.intensity = intensity;
            light.range = range;
            light.innerAngle = innerAngle;
            light.outerAngle = outerAngle;
            light.castShadows = (castShadows != 0);
        }
        else if (line.substr(0, 15) == "MESH_RENDERER: " && currentEntity.IsValid()) {
            std::istringstream iss(line.substr(15));
            int primType, visible, castShadows, receiveShadows;
            float metallic, roughness, emissiveIntensity;
            
            iss >> primType >> visible >> castShadows >> receiveShadows;
            glm::vec3 baseColor = ReadVec3(iss);
            iss >> metallic >> roughness;
            glm::vec3 emissive = ReadVec3(iss);
            iss >> emissiveIntensity;
            
            auto& mesh = currentEntity.AddComponent<scene::MeshRendererComponent>();
            mesh.primitiveType = static_cast<scene::MeshRendererComponent::PrimitiveType>(primType);
            mesh.visible = (visible != 0);
            mesh.castShadows = (castShadows != 0);
            mesh.receiveShadows = (receiveShadows != 0);
            mesh.baseColor = baseColor;
            mesh.metallic = metallic;
            mesh.roughness = roughness;
            mesh.emissive = emissive;
            mesh.emissiveIntensity = emissiveIntensity;
        }
        else if (line == "EDITABLE_MESH_BEGIN" && currentEntity.IsValid() && isV2) {
            // Parse editable mesh data
            mesh::EditableMesh::SerializedData meshData;
            
            while (std::getline(file, line)) {
                size_t meshLineStart = line.find_first_not_of(" \t");
                if (meshLineStart != std::string::npos) line = line.substr(meshLineStart);
                
                if (line == "EDITABLE_MESH_END") break;
                
                if (line.substr(0, 7) == "VERTS: ") {
                    size_t vertCount = std::stoul(line.substr(7));
                    meshData.positions.reserve(vertCount);
                    meshData.uvs.reserve(vertCount);
                    
                    for (size_t i = 0; i < vertCount; ++i) {
                        if (!std::getline(file, line)) break;
                        std::istringstream vss(line);
                        glm::vec3 pos = ReadVec3(vss);
                        glm::vec2 uv;
                        vss >> uv.x >> uv.y;
                        meshData.positions.push_back(pos);
                        meshData.uvs.push_back(uv);
                    }
                }
                else if (line.substr(0, 7) == "FACES: ") {
                    size_t faceCount = std::stoul(line.substr(7));
                    meshData.faceVertexIndices.reserve(faceCount);
                    
                    for (size_t i = 0; i < faceCount; ++i) {
                        if (!std::getline(file, line)) break;
                        std::istringstream fss(line);
                        size_t vertInFace;
                        fss >> vertInFace;
                        
                        std::vector<uint32_t> faceIndices;
                        faceIndices.reserve(vertInFace);
                        for (size_t j = 0; j < vertInFace; ++j) {
                            uint32_t idx;
                            fss >> idx;
                            faceIndices.push_back(idx);
                        }
                        meshData.faceVertexIndices.push_back(faceIndices);
                    }
                }
            }
            
            // Create the EditableMeshComponent
            if (!meshData.positions.empty()) {
                auto& editMesh = currentEntity.AddComponent<scene::EditableMeshComponent>();
                editMesh.mesh = std::make_unique<mesh::EditableMesh>(
                    mesh::EditableMesh::Deserialize(meshData)
                );
                editMesh.MarkDirty();
                LUCENT_CORE_DEBUG("Loaded editable mesh: {} verts, {} faces", 
                    meshData.positions.size(), meshData.faceVertexIndices.size());
            }
        }
    }
    
    file.close();
    LUCENT_CORE_INFO("Scene loaded from: {}", filepath);
    return true;
}

int ImportGLTF(scene::Scene* scene, gfx::Device* device, const std::string& filepath) {
    if (!scene || !device) {
        s_LastError = "Scene or device is null";
        return -1;
    }
    
    assets::ModelLoader loader;
    auto model = loader.LoadGLTF(device, filepath);
    
    if (!model) {
        s_LastError = loader.GetLastError();
        return -1;
    }
    
    int entitiesCreated = 0;
    
    // Helper to decompose matrix into TRS
    auto decomposeMatrix = [](const glm::mat4& m, glm::vec3& pos, glm::vec3& rot, glm::vec3& scale) {
        pos = glm::vec3(m[3]);
        
        scale.x = glm::length(glm::vec3(m[0]));
        scale.y = glm::length(glm::vec3(m[1]));
        scale.z = glm::length(glm::vec3(m[2]));
        
        glm::mat3 rotMat(
            glm::vec3(m[0]) / scale.x,
            glm::vec3(m[1]) / scale.y,
            glm::vec3(m[2]) / scale.z
        );
        
        // Extract Euler angles (YXZ order to match editor)
        rot.x = glm::degrees(asin(-rotMat[1][2]));
        if (cos(glm::radians(rot.x)) > 0.0001f) {
            rot.y = glm::degrees(atan2(rotMat[0][2], rotMat[2][2]));
            rot.z = glm::degrees(atan2(rotMat[1][0], rotMat[1][1]));
        } else {
            rot.y = glm::degrees(atan2(-rotMat[2][0], rotMat[0][0]));
            rot.z = 0.0f;
        }
    };
    
    // Process nodes recursively to compute world transforms
    std::function<void(uint32_t, const glm::mat4&)> processNode;
    processNode = [&](uint32_t nodeIdx, const glm::mat4& parentTransform) {
        if (nodeIdx >= model->nodes.size()) return;
        
        const auto& node = model->nodes[nodeIdx];
        glm::mat4 worldTransform = parentTransform * node.localTransform;
        
        glm::vec3 pos, rot, scale;
        decomposeMatrix(worldTransform, pos, rot, scale);
        
        // Create entity for this node if it has content
        bool hasContent = (node.meshIndex >= 0) || (node.cameraIndex >= 0) || (node.lightIndex >= 0);
        
        if (hasContent) {
            scene::Entity entity = scene->CreateEntity(node.name);
            auto* transform = entity.GetComponent<scene::TransformComponent>();
            transform->position = pos;
            transform->rotation = rot;
            transform->scale = scale;
            
            // Add mesh renderer if this node has a mesh
            if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(model->meshes.size())) {
                // For now, we only set base material properties from the first material
                // TODO: Support multiple submeshes with different materials
                auto& meshRenderer = entity.AddComponent<scene::MeshRendererComponent>();
                meshRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::None;
                
                // Register mesh in runtime registry and store ID in component
                if (model->meshes[node.meshIndex]) {
                    meshRenderer.meshAssetID = assets::MeshRegistry::Get().Register(std::move(model->meshes[node.meshIndex]));
                }
                
                // Get material from first submesh if available
                const auto* mesh = assets::MeshRegistry::Get().GetMesh(meshRenderer.meshAssetID);
                if (mesh && !mesh->GetSubmeshes().empty()) {
                    uint32_t matIdx = mesh->GetSubmeshes()[0].materialIndex;
                    if (matIdx < model->materials.size()) {
                        const auto& mat = model->materials[matIdx];
                        meshRenderer.baseColor = glm::vec3(mat.baseColorFactor);
                        meshRenderer.metallic = mat.metallicFactor;
                        meshRenderer.roughness = mat.roughnessFactor;
                        meshRenderer.emissive = mat.emissiveFactor;
                        meshRenderer.emissiveIntensity = glm::length(mat.emissiveFactor) > 0.01f ? 1.0f : 0.0f;
                    }
                }
            }
            
            // Add camera if this node has one
            if (node.cameraIndex >= 0 && node.cameraIndex < static_cast<int>(model->cameras.size())) {
                const auto& camData = model->cameras[node.cameraIndex];
                auto& cam = entity.AddComponent<scene::CameraComponent>();
                cam.projectionType = camData.perspective ? 
                    scene::CameraComponent::ProjectionType::Perspective :
                    scene::CameraComponent::ProjectionType::Orthographic;
                cam.fov = camData.fov;
                cam.orthoSize = camData.orthoSize;
                cam.nearClip = camData.nearClip;
                cam.farClip = camData.farClip;
                cam.primary = false;  // Don't override existing primary camera
            }
            
            // Add light if this node has one
            if (node.lightIndex >= 0 && node.lightIndex < static_cast<int>(model->lights.size())) {
                const auto& lightData = model->lights[node.lightIndex];
                auto& light = entity.AddComponent<scene::LightComponent>();
                
                switch (lightData.type) {
                    case assets::LightData::Type::Directional:
                        light.type = scene::LightType::Directional;
                        break;
                    case assets::LightData::Type::Point:
                        light.type = scene::LightType::Point;
                        break;
                    case assets::LightData::Type::Spot:
                        light.type = scene::LightType::Spot;
                        break;
                }
                
                light.color = lightData.color;
                light.intensity = lightData.intensity;
                light.range = lightData.range;
                light.innerAngle = glm::degrees(lightData.innerAngle);
                light.outerAngle = glm::degrees(lightData.outerAngle);
            }
            
            entitiesCreated++;
        }
        
        // Process children
        for (uint32_t childIdx : node.children) {
            processNode(childIdx, worldTransform);
        }
    };
    
    // Process all root nodes
    for (uint32_t rootIdx : model->rootNodes) {
        processNode(rootIdx, glm::mat4(1.0f));
    }
    
    LUCENT_CORE_INFO("Imported {} entities from glTF: {}", entitiesCreated, filepath);
    return entitiesCreated;
}

int ImportModel(scene::Scene* scene, gfx::Device* device, const std::string& filepath) {
    // Dispatch based on extension
    std::filesystem::path p(filepath);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".gltf" || ext == ".glb") {
        return ImportGLTF(scene, device, filepath);
    }

    if (!scene || !device) {
        s_LastError = "Scene or device is null";
        return -1;
    }

    assets::ModelLoader loader;
    auto model = loader.LoadAssimp(device, filepath);
    if (!model) {
        s_LastError = loader.GetLastError();
        return -1;
    }

    // Reuse the glTF import path by temporarily swapping loader result into glTF-style import:
    // We replicate the node traversal logic here (same as ImportGLTF).
    int entitiesCreated = 0;

    auto decomposeMatrix = [](const glm::mat4& m, glm::vec3& pos, glm::vec3& rot, glm::vec3& scale) {
        pos = glm::vec3(m[3]);
        scale.x = glm::length(glm::vec3(m[0]));
        scale.y = glm::length(glm::vec3(m[1]));
        scale.z = glm::length(glm::vec3(m[2]));
        glm::mat3 rotMat(
            glm::vec3(m[0]) / scale.x,
            glm::vec3(m[1]) / scale.y,
            glm::vec3(m[2]) / scale.z
        );
        rot.x = glm::degrees(asin(-rotMat[1][2]));
        if (cos(glm::radians(rot.x)) > 0.0001f) {
            rot.y = glm::degrees(atan2(rotMat[0][2], rotMat[2][2]));
            rot.z = glm::degrees(atan2(rotMat[1][0], rotMat[1][1]));
        } else {
            rot.y = glm::degrees(atan2(-rotMat[2][0], rotMat[0][0]));
            rot.z = 0.0f;
        }
    };

    std::function<void(uint32_t, const glm::mat4&)> processNode;
    processNode = [&](uint32_t nodeIdx, const glm::mat4& parentTransform) {
        if (nodeIdx >= model->nodes.size()) return;

        const auto& node = model->nodes[nodeIdx];
        glm::mat4 worldTransform = parentTransform * node.localTransform;

        glm::vec3 pos, rot, sc;
        decomposeMatrix(worldTransform, pos, rot, sc);

        bool hasContent = (node.meshIndex >= 0) || (node.cameraIndex >= 0) || (node.lightIndex >= 0);
        if (hasContent) {
            scene::Entity entity = scene->CreateEntity(node.name);
            auto* transform = entity.GetComponent<scene::TransformComponent>();
            transform->position = pos;
            transform->rotation = rot;
            transform->scale = sc;

            if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(model->meshes.size())) {
                auto& meshRenderer = entity.AddComponent<scene::MeshRendererComponent>();
                meshRenderer.primitiveType = scene::MeshRendererComponent::PrimitiveType::None;

                if (model->meshes[node.meshIndex]) {
                    meshRenderer.meshAssetID = assets::MeshRegistry::Get().Register(std::move(model->meshes[node.meshIndex]));
                }

                const auto* mesh = assets::MeshRegistry::Get().GetMesh(meshRenderer.meshAssetID);
                if (mesh && !mesh->GetSubmeshes().empty()) {
                    uint32_t matIdx = mesh->GetSubmeshes()[0].materialIndex;
                    if (matIdx < model->materials.size()) {
                        const auto& mat = model->materials[matIdx];
                        meshRenderer.baseColor = glm::vec3(mat.baseColorFactor);
                        meshRenderer.metallic = mat.metallicFactor;
                        meshRenderer.roughness = mat.roughnessFactor;
                        meshRenderer.emissive = mat.emissiveFactor;
                        meshRenderer.emissiveIntensity = glm::length(mat.emissiveFactor) > 0.01f ? 1.0f : 0.0f;
                    }
                }
            }

            if (node.cameraIndex >= 0 && node.cameraIndex < static_cast<int>(model->cameras.size())) {
                const auto& camData = model->cameras[node.cameraIndex];
                auto& cam = entity.AddComponent<scene::CameraComponent>();
                cam.projectionType = camData.perspective ?
                    scene::CameraComponent::ProjectionType::Perspective :
                    scene::CameraComponent::ProjectionType::Orthographic;
                cam.fov = camData.fov;
                cam.orthoSize = camData.orthoSize;
                cam.nearClip = camData.nearClip;
                cam.farClip = camData.farClip;
                cam.primary = false;
            }

            if (node.lightIndex >= 0 && node.lightIndex < static_cast<int>(model->lights.size())) {
                const auto& lightData = model->lights[node.lightIndex];
                auto& light = entity.AddComponent<scene::LightComponent>();
                switch (lightData.type) {
                    case assets::LightData::Type::Directional: light.type = scene::LightType::Directional; break;
                    case assets::LightData::Type::Point:       light.type = scene::LightType::Point; break;
                    case assets::LightData::Type::Spot:        light.type = scene::LightType::Spot; break;
                }
                light.color = lightData.color;
                light.intensity = lightData.intensity;
                light.range = lightData.range;
                light.innerAngle = glm::degrees(lightData.innerAngle);
                light.outerAngle = glm::degrees(lightData.outerAngle);
            }

            entitiesCreated++;
        }

        for (uint32_t childIdx : node.children) {
            processNode(childIdx, worldTransform);
        }
    };

    for (uint32_t rootIdx : model->rootNodes) {
        processNode(rootIdx, glm::mat4(1.0f));
    }

    LUCENT_CORE_INFO("Imported {} entities from model: {}", entitiesCreated, filepath);
    return entitiesCreated;
}

} // namespace SceneIO
} // namespace lucent

