#include "SceneIO.h"
#include "lucent/scene/Components.h"
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
    
    // Header
    file << "LUCENT_SCENE_V1\n";
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
    
    // Read header
    std::getline(file, line);
    if (line != "LUCENT_SCENE_V1") {
        s_LastError = "Invalid scene file format";
        return false;
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
    }
    
    file.close();
    LUCENT_CORE_INFO("Scene loaded from: {}", filepath);
    return true;
}

} // namespace SceneIO
} // namespace lucent

