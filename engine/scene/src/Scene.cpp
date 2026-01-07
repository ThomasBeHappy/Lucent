#include "lucent/scene/Scene.h"

namespace lucent::scene {

Scene::Scene(const std::string& name) 
    : m_Name(name) {
}

Scene::~Scene() {
    // Components are automatically cleaned up via unique_ptr
    m_Entities.clear();
    m_ComponentArrays.clear();
}

Entity Scene::CreateEntity(const std::string& name) {
    EntityID id = m_NextEntityID++;
    m_Entities.push_back(id);
    
    Entity entity(id, this);
    
    // All entities get a TagComponent and TransformComponent by default
    AddComponent<TagComponent>(id, name);
    AddComponent<TransformComponent>(id);
    
    LUCENT_CORE_DEBUG("Created entity '{}' (ID: {})", name, id);
    return entity;
}

Entity Scene::CreateEntityWithID(EntityID id, const std::string& name) {
    m_Entities.push_back(id);
    if (id >= m_NextEntityID) {
        m_NextEntityID = id + 1;
    }
    
    Entity entity(id, this);
    
    AddComponent<TagComponent>(id, name);
    AddComponent<TransformComponent>(id);
    
    return entity;
}

void Scene::DestroyEntity(Entity entity) {
    if (!entity.IsValid()) return;
    
    EntityID id = entity.GetID();
    
    // Remove from entity list
    auto it = std::find(m_Entities.begin(), m_Entities.end(), id);
    if (it != m_Entities.end()) {
        m_Entities.erase(it);
    }
    
    // Notify all component arrays
    for (auto& [typeIndex, array] : m_ComponentArrays) {
        array->EntityDestroyed(id);
    }
    
    LUCENT_CORE_DEBUG("Destroyed entity (ID: {})", id);
}

Entity Scene::GetEntity(EntityID id) {
    auto it = std::find(m_Entities.begin(), m_Entities.end(), id);
    if (it != m_Entities.end()) {
        return Entity(id, this);
    }
    return Entity(INVALID_ENTITY, nullptr);
}

void Scene::Clear() {
    // Clear all component arrays
    m_ComponentArrays.clear();
    m_Entities.clear();
    m_NextEntityID = 0;
    LUCENT_CORE_DEBUG("Scene cleared");
}

Entity Scene::GetPrimaryCamera() {
    auto* cameraArray = GetComponentArray<CameraComponent>();
    if (!cameraArray) return Entity();
    
    Entity result;
    cameraArray->ForEach([this, &result](EntityID id, CameraComponent& cam) {
        if (cam.primary) {
            result = Entity(id, this);
        }
    });
    
    return result;
}

} // namespace lucent::scene

