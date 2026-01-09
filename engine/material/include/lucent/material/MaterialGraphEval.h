#pragma once

#include "lucent/material/MaterialGraph.h"
#include <glm/glm.hpp>
#include <string>
#include <cstdint>

namespace lucent::material {

// Minimal material constants used by traced / raytraced renderers today.
struct TracerMaterialConstants {
    glm::vec4 baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    glm::vec4 emissive = glm::vec4(0.0f); // rgb + intensity in w (or 1.0)
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ior = 1.5f;
    uint32_t flags = 0;
};

// Evaluate a MaterialGraph into constant channels for the tracer backends.
// NOTE: The tracer backends currently consume constant parameters only (no textures/uv-varying evaluation).
// This evaluator supports a large subset of math nodes + CustomCode (expression-like Out assignment).
bool EvaluateTracerConstants(const MaterialGraph& graph, TracerMaterialConstants& out, std::string& outError);

} // namespace lucent::material

