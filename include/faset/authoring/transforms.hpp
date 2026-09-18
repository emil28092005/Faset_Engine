#pragma once
#include <faset/core/json.hpp>
#include <string>
namespace faset::authoring {
// Reparents authored TRS while optionally preserving the complete world transform.
// Non-invertible parents and transforms requiring shear are rejected atomically.
void reparent_entity(Json& scene, const std::string& entity, const Json& parent, bool keep_world);
} // namespace faset::authoring
