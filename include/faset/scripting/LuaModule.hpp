#pragma once

#include <cstddef>
#include <faset/runtime/Runtime.hpp>
#include <faset/scripting/project.hpp>
#include <memory>
#include <string>
#include <vector>

namespace faset::scripting {

struct LuaLimits {
    std::size_t memoryBytes{16 * 1024 * 1024};
    unsigned instructions{1'000'000};
};

// One sandboxed VM per module, isolated instance tables per entity/component.
// Behavior callbacks retain shared ownership of the VM until Runtime is destroyed.
class LuaModule {
  public:
    explicit LuaModule(const LuaProject& project, LuaLimits limits = {});
    ~LuaModule();
    LuaModule(const LuaModule&) = delete;
    LuaModule& operator=(const LuaModule&) = delete;
    nlohmann::json schema() const;
    // Validates Lua component configuration without creating instances or running callbacks.
    // Pair with runtime::validate_scene_schemas for cross-language IDs and versions.
    void validateScene(const nlohmann::json& scene) const;
    void registerBehaviors(runtime::Runtime& runtime);
    std::vector<std::string> takeLogs();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace faset::scripting
