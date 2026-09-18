#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace faset::runtime {
// Exact native TypeIds. A prefix alone does not make a custom type native.
bool is_builtin_component(std::string_view type) noexcept;

// Player boundary: require every custom TypeId and its exact positive integer
// version in the linked gameplay schema. Accepts its array or a {types:[...]}
// manifest. Native components always require version 1. A missing version means
// 1 for existing development scenes. Gameplay may not redeclare native TypeIds.
// Does not migrate or change source data.
// Structural/native field checks remain Runtime::load's responsibility; this
// function has no authoring dependency and runs no gameplay callbacks.
void validate_scene_schemas(const nlohmann::json& scene, const nlohmann::json& gameplay_schema);
} // namespace faset::runtime
