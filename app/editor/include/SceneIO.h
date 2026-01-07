#pragma once

#include "lucent/scene/Scene.h"
#include <string>

namespace lucent {

// Simple text-based scene serialization (.lucent format)
// Format is line-based for easy debugging:
//   LUCENT_SCENE_V1
//   SCENE_NAME: <name>
//   ENTITY_BEGIN
//     NAME: <name>
//     TRANSFORM: px py pz rx ry rz sx sy sz
//     CAMERA: projType fov/size near far primary
//     LIGHT: type r g b intensity range innerAngle outerAngle castShadows
//     MESH_RENDERER: primType visible castShadows receiveShadows br bg bb metallic roughness er eg eb emissiveIntensity
//   ENTITY_END

namespace SceneIO {

// Save scene to file
// Returns true on success
bool SaveScene(scene::Scene* scene, const std::string& filepath);

// Load scene from file
// Returns true on success (scene is cleared first)
bool LoadScene(scene::Scene* scene, const std::string& filepath);

// Get last error message
const std::string& GetLastError();

} // namespace SceneIO

} // namespace lucent

