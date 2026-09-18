#pragma once
#include <faset/authoring/schema.hpp>
#include <functional>
#include <string>

namespace faset::authoring {
struct ResolvedScene {
    Json scene;
    Json conflicts = Json::array();
};
using SceneLoader = std::function<Json(const std::string&)>;
// Source documents are immutable inputs. Conflicting records stay in the authoring file.
ResolvedScene resolve_templates(const Json& scene, const SchemaRegistry& schemas,
                                const SceneLoader& loader);
} // namespace faset::authoring
