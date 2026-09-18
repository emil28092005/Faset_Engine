#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace faset::scripting {

// Immutable source snapshot. Keys and entries are project-relative UTF-8 paths.
// Only Lua files below Scripts are accepted; loading never follows symlinks.
struct LuaProject {
    std::vector<std::string> scripts;
    std::map<std::string, std::string> sources;
    std::string fingerprint;
    bool enabled() const noexcept {
        return !scripts.empty();
    }
};

// project.faset.json: scripting.lua.scripts = ["Scripts/player.lua", ...].
// An absent Lua declaration is a C++-only project.
LuaProject loadLuaProject(const std::filesystem::path& projectRoot);

// Writes the captured sources, not live files. The caller owns the target manifest.
void writeLuaSources(const LuaProject& project, const std::filesystem::path& targetRoot);

} // namespace faset::scripting
