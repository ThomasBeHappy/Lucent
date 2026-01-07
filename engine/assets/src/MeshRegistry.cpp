#include "lucent/assets/MeshRegistry.h"

namespace lucent::assets {

uint32_t MeshRegistry::Register(std::unique_ptr<Mesh> mesh) {
    if (!mesh) return UINT32_MAX;
    std::scoped_lock lock(m_Mutex);
    m_Meshes.push_back(std::move(mesh));
    return static_cast<uint32_t>(m_Meshes.size() - 1);
}

Mesh* MeshRegistry::GetMesh(uint32_t id) {
    std::scoped_lock lock(m_Mutex);
    if (id >= m_Meshes.size()) return nullptr;
    return m_Meshes[id].get();
}

const Mesh* MeshRegistry::GetMesh(uint32_t id) const {
    std::scoped_lock lock(m_Mutex);
    if (id >= m_Meshes.size()) return nullptr;
    return m_Meshes[id].get();
}

void MeshRegistry::Clear() {
    std::scoped_lock lock(m_Mutex);
    m_Meshes.clear();
}

} // namespace lucent::assets


