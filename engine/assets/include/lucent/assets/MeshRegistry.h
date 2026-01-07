#pragma once

#include "lucent/core/Core.h"
#include "lucent/assets/Mesh.h"
#include <mutex>
#include <memory>
#include <vector>

namespace lucent::assets {

// Simple runtime registry for meshes loaded at runtime (e.g., glTF import).
// Returns stable integer IDs suitable for storing in components.
class MeshRegistry : public NonCopyable {
public:
    static MeshRegistry& Get() {
        static MeshRegistry instance;
        return instance;
    }

    // Takes ownership; returns an ID you can store.
    uint32_t Register(std::unique_ptr<Mesh> mesh);

    // Returns nullptr if id invalid or mesh was removed.
    Mesh* GetMesh(uint32_t id);
    const Mesh* GetMesh(uint32_t id) const;

    void Clear();

private:
    MeshRegistry() = default;

    mutable std::mutex m_Mutex;
    std::vector<std::unique_ptr<Mesh>> m_Meshes;
};

} // namespace lucent::assets


