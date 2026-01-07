#include "EditorSettings.h"
#include "lucent/core/Log.h"

#include <fstream>
#include <sstream>

namespace lucent {

static std::string Trim(std::string s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && isSpace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && isSpace((unsigned char)s.back())) s.pop_back();
    return s;
}

EditorSettings EditorSettings::Load(const std::string& path) {
    EditorSettings s{};
    std::ifstream f(path);
    if (!f.is_open()) return s;

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (key == "PreferredGPU") {
            s.preferredGpuName = val;
        }
    }

    LUCENT_CORE_DEBUG("EditorSettings loaded (PreferredGPU='{}')", s.preferredGpuName);
    return s;
}

bool EditorSettings::Save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) {
        LUCENT_CORE_ERROR("Failed to save EditorSettings to {}", path);
        return false;
    }

    f << "# Lucent Editor settings\n";
    f << "# PreferredGPU is matched as a substring against Vulkan physical device names.\n";
    f << "PreferredGPU=" << preferredGpuName << "\n";
    f.close();

    LUCENT_CORE_INFO("EditorSettings saved to {}", path);
    return true;
}

} // namespace lucent


