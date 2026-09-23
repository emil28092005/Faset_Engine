#pragma once

#include <faset/editor/build_service.hpp>
#include <faset/scripting/project.hpp>
#include <filesystem>
#include <string>

namespace faset::editor {

struct BuildInputs {
    std::string source_hash;
    std::string recipe_hash;
    std::string toolchain_hash;
    std::string fingerprint() const;
};

// Hash all files in Scripts and the active Lua declaration, not only the two
// conventional gameplay files. Both Editor stale-state and builds use this key.
std::string gameplay_source_hash(const std::filesystem::path& project_root,
                                 const scripting::LuaProject& lua);
BuildInputs capture_build_inputs(const BuildConfig& config, const scripting::LuaProject& lua);

// A compiler replaced at the same path may be invisible to Ninja. Invalidate
// only this generated native tree; published build generations stay intact.
void ensure_native_toolchain_stamp(const std::filesystem::path& native_directory,
                                   const BuildInputs& inputs);

} // namespace faset::editor
