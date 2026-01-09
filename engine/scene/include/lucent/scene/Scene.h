#pragma once

#include "lucent/scene/Entity.h"
#include "lucent/scene/Components.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <functional>

namespace lucent::scene {

// Type-erased component storage
class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void EntityDestroyed(EntityID entity) = 0;
    virtual bool Has(EntityID entity) const = 0;
};

template<typename T>
class ComponentArray : public IComponentArray {
public:
    T& Add(EntityID entity, T component = T{}) {
        LUCENT_CORE_ASSERT(m_EntityToIndex.find(entity) == m_EntityToIndex.end(), 
            "Component already exists on entity");
        
        size_t newIndex = m_Components.size();
        m_EntityToIndex[entity] = newIndex;
        m_IndexToEntity[newIndex] = entity;
        m_Components.push_back(std::move(component));
        
        return m_Components.back();
    }
    
    void Remove(EntityID entity) {
        auto it = m_EntityToIndex.find(entity);
        if (it == m_EntityToIndex.end()) return;
        
        size_t removedIndex = it->second;
        size_t lastIndex = m_Components.size() - 1;
        
        if (removedIndex != lastIndex) {
            // Move last element to removed position
            m_Components[removedIndex] = std::move(m_Components[lastIndex]);
            
            EntityID lastEntity = m_IndexToEntity[lastIndex];
            m_EntityToIndex[lastEntity] = removedIndex;
            m_IndexToEntity[removedIndex] = lastEntity;
        }
        
        m_Components.pop_back();
        m_EntityToIndex.erase(entity);
        m_IndexToEntity.erase(lastIndex);
    }
    
    T* Get(EntityID entity) {
        auto it = m_EntityToIndex.find(entity);
        if (it == m_EntityToIndex.end()) return nullptr;
        return &m_Components[it->second];
    }
    
    const T* Get(EntityID entity) const {
        auto it = m_EntityToIndex.find(entity);
        if (it == m_EntityToIndex.end()) return nullptr;
        return &m_Components[it->second];
    }
    
    bool Has(EntityID entity) const override {
        return m_EntityToIndex.find(entity) != m_EntityToIndex.end();
    }
    
    void EntityDestroyed(EntityID entity) override {
        Remove(entity);
    }
    
    // Iteration
    size_t Size() const { return m_Components.size(); }
    
    template<typename Func>
    void ForEach(Func&& func) {
        for (size_t i = 0; i < m_Components.size(); ++i) {
            func(m_IndexToEntity[i], m_Components[i]);
        }
    }
    
private:
    std::vector<T> m_Components;
    std::unordered_map<EntityID, size_t> m_EntityToIndex;
    std::unordered_map<size_t, EntityID> m_IndexToEntity;
};

class Scene {
public:
    Scene(const std::string& name = "Untitled Scene");
    ~Scene();
    
    // Entity management
    Entity CreateEntity(const std::string& name = "Entity");
    Entity CreateEntityWithID(EntityID id, const std::string& name = "Entity");
    void DestroyEntity(Entity entity);
    Entity GetEntity(EntityID id);
    
    // Component management
    template<typename T, typename... Args>
    T& AddComponent(EntityID entity, Args&&... args) {
        auto& array = GetOrCreateComponentArray<T>();
        return array.Add(entity, T{std::forward<Args>(args)...});
    }
    
    template<typename T>
    void RemoveComponent(EntityID entity) {
        auto* array = GetComponentArray<T>();
        if (array) array->Remove(entity);
    }
    
    template<typename T>
    T* GetComponent(EntityID entity) {
        auto* array = GetComponentArray<T>();
        return array ? array->Get(entity) : nullptr;
    }
    
    template<typename T>
    const T* GetComponent(EntityID entity) const {
        auto* array = GetComponentArray<T>();
        return array ? array->Get(entity) : nullptr;
    }
    
    template<typename T>
    bool HasComponent(EntityID entity) const {
        auto* array = GetComponentArray<T>();
        return array ? array->Has(entity) : false;
    }
    
    // Iteration
    template<typename T, typename Func>
    void ForEach(Func&& func) {
        auto* array = GetComponentArray<T>();
        if (array) {
            array->ForEach(std::forward<Func>(func));
        }
    }
    
    // View for iterating entities with specific components
    template<typename... Components>
    class View {
    public:
        View(Scene* scene) : m_Scene(scene) {}
        
        template<typename Func>
        void Each(Func&& func) {
            // Use first component type to iterate
            using FirstComponent = std::tuple_element_t<0, std::tuple<Components...>>;
            auto* array = m_Scene->GetComponentArray<FirstComponent>();
            if (!array) return;
            
            array->ForEach([this, &func](EntityID entity, FirstComponent&) {
                if ((m_Scene->HasComponent<Components>(entity) && ...)) {
                    func(Entity(entity, m_Scene), *m_Scene->GetComponent<Components>(entity)...);
                }
            });
        }
        
    private:
        Scene* m_Scene;
    };
    
    template<typename... Components>
    View<Components...> GetView() {
        return View<Components...>(this);
    }
    
    // Scene info
    const std::string& GetName() const { return m_Name; }
    void SetName(const std::string& name) { m_Name = name; }
    
    const std::vector<EntityID>& GetEntities() const { return m_Entities; }
    size_t GetEntityCount() const { return m_Entities.size(); }

    const std::string& GetEnvironmentMapPath() const { return m_EnvironmentMapPath; }
    void SetEnvironmentMapPath(const std::string& path) { m_EnvironmentMapPath = path; }
    
    // Clear all entities
    void Clear();
    
    // Find primary camera
    Entity GetPrimaryCamera();
    
private:
    template<typename T>
    ComponentArray<T>* GetComponentArray() {
        std::type_index typeIndex(typeid(T));
        auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) return nullptr;
        return static_cast<ComponentArray<T>*>(it->second.get());
    }
    
    template<typename T>
    const ComponentArray<T>* GetComponentArray() const {
        std::type_index typeIndex(typeid(T));
        auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) return nullptr;
        return static_cast<const ComponentArray<T>*>(it->second.get());
    }
    
    template<typename T>
    ComponentArray<T>& GetOrCreateComponentArray() {
        std::type_index typeIndex(typeid(T));
        auto it = m_ComponentArrays.find(typeIndex);
        if (it == m_ComponentArrays.end()) {
            auto newArray = std::make_unique<ComponentArray<T>>();
            auto* ptr = newArray.get();
            m_ComponentArrays[typeIndex] = std::move(newArray);
            return *ptr;
        }
        return *static_cast<ComponentArray<T>*>(it->second.get());
    }
    
private:
    std::string m_Name;
    std::vector<EntityID> m_Entities;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentArray>> m_ComponentArrays;

    std::string m_EnvironmentMapPath;
    EntityID m_NextEntityID = 0;
};

// Entity template implementations (need Scene definition)
template<typename T>
T* Entity::GetComponent() {
    return m_Scene ? m_Scene->GetComponent<T>(m_ID) : nullptr;
}

template<typename T>
const T* Entity::GetComponent() const {
    return m_Scene ? m_Scene->GetComponent<T>(m_ID) : nullptr;
}

template<typename T>
bool Entity::HasComponent() const {
    return m_Scene ? m_Scene->HasComponent<T>(m_ID) : false;
}

template<typename T, typename... Args>
T& Entity::AddComponent(Args&&... args) {
    LUCENT_CORE_ASSERT(m_Scene, "Entity has no scene");
    return m_Scene->AddComponent<T>(m_ID, std::forward<Args>(args)...);
}

template<typename T>
void Entity::RemoveComponent() {
    if (m_Scene) m_Scene->RemoveComponent<T>(m_ID);
}

} // namespace lucent::scene
