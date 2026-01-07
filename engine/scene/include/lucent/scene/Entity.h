#pragma once

#include "lucent/core/Core.h"
#include <cstdint>
#include <limits>

namespace lucent::scene {

// Entity is just an ID - components are stored separately
using EntityID = uint32_t;
constexpr EntityID INVALID_ENTITY = std::numeric_limits<EntityID>::max();

class Scene;

// Entity handle - lightweight wrapper around EntityID with Scene reference
class Entity {
public:
    Entity() = default;
    Entity(EntityID id, Scene* scene) : m_ID(id), m_Scene(scene) {}
    
    EntityID GetID() const { return m_ID; }
    Scene* GetScene() const { return m_Scene; }
    
    bool IsValid() const { return m_ID != INVALID_ENTITY && m_Scene != nullptr; }
    
    operator bool() const { return IsValid(); }
    operator EntityID() const { return m_ID; }
    
    bool operator==(const Entity& other) const { return m_ID == other.m_ID && m_Scene == other.m_Scene; }
    bool operator!=(const Entity& other) const { return !(*this == other); }
    
    // Component access (implemented in Scene.h after Scene is defined)
    template<typename T>
    T* GetComponent();
    
    template<typename T>
    const T* GetComponent() const;
    
    template<typename T>
    bool HasComponent() const;
    
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args);
    
    template<typename T>
    void RemoveComponent();
    
private:
    EntityID m_ID = INVALID_ENTITY;
    Scene* m_Scene = nullptr;
};

} // namespace lucent::scene

