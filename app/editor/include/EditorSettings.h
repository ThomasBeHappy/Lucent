#pragma once

#include <string>

namespace lucent {

struct EditorSettings {
    // If empty: auto (prefer discrete)
    std::string preferredGpuName;

    static EditorSettings Load(const std::string& path = "lucent_editor.ini");
    bool Save(const std::string& path = "lucent_editor.ini") const;
};

} // namespace lucent


