#include <faset/scripting/project.hpp>

#include <array>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace faset::scripting {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t max_source_bytes = 1024 * 1024;
constexpr std::size_t max_total_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_sources = 4096;
constexpr std::size_t max_directory_entries = 16384;

fs::path source_path(const std::string& name) {
    if (name.size() > 1024 || name.find('\0') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
        throw std::runtime_error("Lua source path must use project-relative forward slashes: " +
                                 name);
    const auto path = path_from_utf8(name);
    if (path.is_absolute() || path.has_root_path() || path.empty() || *path.begin() != "Scripts" ||
        path.extension() != ".lua" || generic_path_to_utf8(path.lexically_normal()) != name)
        throw std::runtime_error("Lua sources must be normalized .lua paths below Scripts/: " +
                                 name);
    for (const auto& part : path)
        if (part == "." || part == ".." || part.empty())
            throw std::runtime_error("Lua source path contains traversal: " + name);
    return path;
}

void no_symlinks(const fs::path& root, const fs::path& relative = {}) {
    auto current = root;
    if (fs::is_symlink(fs::symlink_status(current)))
        throw std::runtime_error("Lua project root must not be a symlink");
    for (const auto& part : relative) {
        current /= part;
        if (fs::is_symlink(fs::symlink_status(current)))
            throw std::runtime_error("Lua project paths must not contain symlinks: " +
                                     path_to_utf8(current));
    }
}

std::string bounded_text(const fs::path& path) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > max_source_bytes)
        throw std::runtime_error("Lua source or manifest must be a regular file at most 1 MiB: " +
                                 path_to_utf8(path));
    std::ifstream stream(native_io_path(path), std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read Lua project file: " + path_to_utf8(path));
    std::string value;
    std::array<char, 16384> buffer{};
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        const auto count = static_cast<std::size_t>(stream.gcount());
        if (value.size() + count > max_source_bytes)
            throw std::runtime_error("Lua project file exceeds 1 MiB: " + path_to_utf8(path));
        value.append(buffer.data(), count);
    }
    if (stream.bad())
        throw std::runtime_error("Cannot read Lua project file: " + path_to_utf8(path));
    return value;
}

void validate_snapshot(const LuaProject& project) {
    if (project.scripts.size() > max_sources || project.sources.size() > max_sources)
        throw std::runtime_error("Lua project exceeds 4096 source files");
    std::set<std::string> entries;
    for (const auto& name : project.scripts) {
        (void)source_path(name);
        if (!entries.insert(name).second || !project.sources.contains(name))
            throw std::runtime_error("Duplicate or missing Lua entry source: " + name);
    }
    std::size_t bytes{};
    for (const auto& [name, source] : project.sources) {
        (void)source_path(name);
        bytes += source.size();
        if (source.size() > max_source_bytes || bytes > max_total_bytes)
            throw std::runtime_error(
                "Lua project exceeds source size limits (1 MiB/file, 16 MiB total)");
    }
}
} // namespace

LuaProject loadLuaProject(const fs::path& projectRoot) {
    LuaProject result;
    no_symlinks(projectRoot);
    const auto manifest = projectRoot / "project.faset.json";
    no_symlinks(projectRoot, "project.faset.json");
    if (!fs::exists(manifest))
        return result;
    const auto project = Json::parse(bounded_text(manifest));
    if (!project.is_object())
        throw std::runtime_error("Lua project manifest must be an object");
    if (!project.contains("scripting"))
        return result;
    const auto& scripting = project.at("scripting");
    if (!scripting.is_object())
        throw std::runtime_error("Project scripting must be an object");
    if (!scripting.contains("lua"))
        return result;
    const auto& lua = scripting.at("lua");
    if (!lua.is_object() || !lua.contains("scripts") || !lua.at("scripts").is_array())
        throw std::runtime_error("Project scripting.lua.scripts must be an array of entry paths");
    if (lua.at("scripts").size() > max_sources)
        throw std::runtime_error("Lua project exceeds 4096 entry scripts");
    std::set<std::string> unique;
    for (const auto& entry : lua.at("scripts")) {
        if (!entry.is_string())
            throw std::runtime_error("Lua entry paths must be strings");
        auto name = entry.get<std::string>();
        const auto path = source_path(name);
        no_symlinks(projectRoot, path);
        if (!unique.insert(name).second)
            throw std::runtime_error("Duplicate Lua entry source: " + name);
        if (!fs::is_regular_file(projectRoot / path))
            throw std::runtime_error("Missing Lua entry source: " + name);
        result.scripts.push_back(std::move(name));
    }
    if (!result.enabled())
        return result;
    no_symlinks(projectRoot, "Scripts");
    std::size_t count{}, total{};
    for (auto it = fs::recursive_directory_iterator(projectRoot / "Scripts");
         it != fs::recursive_directory_iterator(); ++it) {
        if (++count > max_directory_entries || it.depth() > 32)
            throw std::runtime_error("Lua Scripts directory exceeds traversal limits");
        if (it->is_symlink())
            throw std::runtime_error("Lua Scripts directory contains a symlink: " +
                                     path_to_utf8(it->path()));
        if (it->path().extension() != ".lua")
            continue;
        const auto name = generic_path_to_utf8(it->path().lexically_relative(projectRoot));
        (void)source_path(name);
        auto source = bounded_text(it->path());
        total += source.size();
        if (total > max_total_bytes || result.sources.size() >= max_sources)
            throw std::runtime_error(
                "Lua project exceeds source limits (4096 files, 16 MiB total)");
        result.sources.emplace(name, std::move(source));
    }
    validate_snapshot(result);
    // JSON supplies unambiguous framing; std::map makes source ordering stable.
    result.fingerprint = sha256(Json{
        {"format", "faset.lua-sources.v1"},
        {"scripts", result.scripts},
        {"sources", result.sources}}.dump());
    return result;
}

void writeLuaSources(const LuaProject& project, const fs::path& targetRoot) {
    validate_snapshot(project);
    no_symlinks(targetRoot);
    // Validate all destinations before writing any source.
    for (const auto& [name, source] : project.sources)
        no_symlinks(targetRoot, source_path(name));
    for (const auto& [name, source] : project.sources)
        atomic_write(targetRoot / source_path(name), source);
}
} // namespace faset::scripting
