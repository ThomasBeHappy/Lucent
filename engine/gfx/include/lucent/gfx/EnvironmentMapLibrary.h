#pragma once

#include "lucent/core/Core.h"
#include "lucent/gfx/EnvironmentMap.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace lucent::gfx {

// Simple registry for runtime-loaded HDR environment maps.
// Returns stable handles that can be stored in render settings.
class EnvironmentMapLibrary : public NonCopyable {
public:
    static constexpr uint32_t InvalidHandle = UINT32_MAX;

    static EnvironmentMapLibrary& Get() {
        static EnvironmentMapLibrary instance;
        return instance;
    }

    void Init(Device* device);
    void Shutdown();

    uint32_t LoadFromFile(const std::string& path);
    uint32_t CreateDefaultSky();

    uint32_t GetDefaultHandle() const { return m_DefaultHandle; }

    EnvironmentMap* Get(uint32_t handle);
    const EnvironmentMap* Get(uint32_t handle) const;

private:
    EnvironmentMapLibrary() = default;

    Device* m_Device = nullptr;
    std::vector<std::unique_ptr<EnvironmentMap>> m_Maps;
    std::unordered_map<std::string, uint32_t> m_PathToHandle;
    uint32_t m_DefaultHandle = InvalidHandle;
};

} // namespace lucent::gfx
