#pragma once

#include <faset/core/json.hpp>
#include <filesystem>
#include <string_view>

namespace faset::editor {

// Parse compiler, MSVC/clang-cl and Lua output into bounded Editor-facing rows.
// Only sources under the current project's Scripts directory receive a file
// key, so clients cannot offer navigation to unrelated filesystem paths.
Json parse_build_diagnostics(std::string_view raw_log, std::string_view phase,
                             const std::filesystem::path& project_root);

} // namespace faset::editor
