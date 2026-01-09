#include "lucent/gfx/EnvironmentMapLibrary.h"
#include "lucent/core/Log.h"

namespace lucent::gfx {

void EnvironmentMapLibrary::Init(Device* device) {
    m_Device = device;
}

void EnvironmentMapLibrary::Shutdown() {
    m_Maps.clear();
    m_PathToHandle.clear();
    m_DefaultHandle = InvalidHandle;
    m_Device = nullptr;
}

uint32_t EnvironmentMapLibrary::LoadFromFile(const std::string& path) {
    if (path.empty()) {
        return InvalidHandle;
    }

    auto existing = m_PathToHandle.find(path);
    if (existing != m_PathToHandle.end()) {
        return existing->second;
    }

    if (!m_Device) {
        LUCENT_CORE_ERROR("EnvironmentMapLibrary: device not initialized");
        return InvalidHandle;
    }

    auto envMap = std::make_unique<EnvironmentMap>();
    if (!envMap->LoadFromFile(m_Device, path)) {
        return InvalidHandle;
    }

    m_Maps.push_back(std::move(envMap));
    uint32_t handle = static_cast<uint32_t>(m_Maps.size() - 1);
    m_PathToHandle[path] = handle;
    return handle;
}

uint32_t EnvironmentMapLibrary::CreateDefaultSky() {
    if (m_DefaultHandle != InvalidHandle) {
        return m_DefaultHandle;
    }

    if (!m_Device) {
        LUCENT_CORE_ERROR("EnvironmentMapLibrary: device not initialized");
        return InvalidHandle;
    }

    auto envMap = std::make_unique<EnvironmentMap>();
    if (!envMap->CreateDefaultSky(m_Device)) {
        return InvalidHandle;
    }

    m_Maps.push_back(std::move(envMap));
    m_DefaultHandle = static_cast<uint32_t>(m_Maps.size() - 1);
    return m_DefaultHandle;
}

EnvironmentMap* EnvironmentMapLibrary::Get(uint32_t handle) {
    if (handle >= m_Maps.size()) {
        return nullptr;
    }
    return m_Maps[handle].get();
}

const EnvironmentMap* EnvironmentMapLibrary::Get(uint32_t handle) const {
    if (handle >= m_Maps.size()) {
        return nullptr;
    }
    return m_Maps[handle].get();
}

} // namespace lucent::gfx
