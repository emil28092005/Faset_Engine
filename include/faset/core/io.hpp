#pragma once
#include <faset/core/json.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace faset {
std::string new_id();
std::string read_text(const std::filesystem::path& path);
Json read_json(const std::filesystem::path& path);
void atomic_write(const std::filesystem::path& path, std::string_view bytes);
void atomic_write_json(const std::filesystem::path& path, const Json& value);
// Rejects traversal and symlink escapes before project-scoped file operations.
std::filesystem::path project_path(const std::filesystem::path& root, const std::filesystem::path& relative);
}
