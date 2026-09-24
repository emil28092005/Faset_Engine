#pragma once

#include <faset/core/json.hpp>
#include <filesystem>
#include <string_view>

namespace faset::editor {

// Parse compiler, MSVC/clang-cl and Lua output into bounded Editor-facing rows.
// Only sources under the project Scripts directory or the verified immutable
// Scripts snapshot supplied by BuildService receive a project-relative file key.
Json parse_build_diagnostics(std::string_view raw_log, std::string_view phase,
                             const std::filesystem::path& project_root,
                             const std::filesystem::path& snapshot_scripts = {});

} // namespace faset::editor
