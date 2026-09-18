#pragma once
#include <faset/core/json.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace faset {
// Text/JSON/process arguments use UTF-8. Keep filesystem paths in native form
// internally; narrow path constructors/string() depend on the Windows code page.
std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);
std::string generic_path_to_utf8(const std::filesystem::path& path);
// Use only at file-I/O boundaries, never for serialized paths or identity comparisons.
// Windows: absolute, normalized extended-length path; other systems: unchanged.
std::filesystem::path native_io_path(const std::filesystem::path& path);
#ifdef _WIN32
// Thin wmain adapter: preserve the CRT's Unicode argument parsing and pass UTF-8
// to the shared application entry. No process/global code-page change is needed.
int run_utf8_main(int argc, wchar_t** argv, int (*entry)(int, char**)) noexcept;
#endif
std::string new_id();
std::string read_text(const std::filesystem::path& path);
Json read_json(const std::filesystem::path& path);
void atomic_write(const std::filesystem::path& path, std::string_view bytes);
void atomic_write_json(const std::filesystem::path& path, const Json& value);
// Rejects traversal and symlink escapes before project-scoped file operations.
std::filesystem::path project_path(const std::filesystem::path& root,
                                   const std::filesystem::path& relative);
} // namespace faset
