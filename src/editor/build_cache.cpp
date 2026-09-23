#include <faset/editor/build_cache.hpp>

#include <faset/authoring/schema.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace faset::editor {
namespace fs = std::filesystem;
namespace {
std::string normalized_hash(const Json& value) {
    return sha256(value.dump());
}
std::string configured_tool(const BuildConfig& config, std::string_view key,
                            std::string fallback) {
    const auto prefix = "-D" + std::string(key) + "=";
    for (const auto& argument : config.configure_arguments)
        if (argument.starts_with(prefix))
            fallback = argument.substr(prefix.size());
    return fallback;
}
fs::path resolve_tool(const std::string& name) {
    const auto supplied = path_from_utf8(name);
    if (supplied.has_parent_path())
        return fs::absolute(supplied).lexically_normal();
    try {
        return find_executable(name);
    } catch (const std::exception&) {
        // CMake reports a missing compiler. Keep the identity deterministic so
        // fixture build tools can exercise publication without native compiling.
        return supplied;
    }
}
Json tool_identity(const std::string& name) {
    const auto resolved = resolve_tool(name);
    Json result = {{"path", path_to_utf8(resolved)}};
    if (fs::is_regular_file(resolved))
        result["sha256"] = sha256_file(resolved);
    else
        result["missing"] = true;
    return result;
}
std::string slang_name(const BuildConfig& config) {
    auto configured = configured_tool(config, "SLANGC_EXECUTABLE", "");
    if (!configured.empty())
        return configured;
#ifdef _WIN32
    const auto local = config.engine_root / ".cache/slang/bin/slangc.exe";
#else
    const auto local = config.engine_root / ".cache/slang/bin/slangc";
#endif
    if (fs::is_regular_file(local))
        return path_to_utf8(local);
    if (const char* sdk = std::getenv("VULKAN_SDK")) {
#ifdef _WIN32
        const auto sdk_tool = path_from_utf8(sdk) / "Bin/slangc.exe";
#else
        const auto sdk_tool = path_from_utf8(sdk) / "bin/slangc";
#endif
        if (fs::is_regular_file(sdk_tool))
            return path_to_utf8(sdk_tool);
    }
    return "slangc";
}
Json recipe_files(const fs::path& engine_root) {
    Json files = Json::object();
    for (const auto& relative : {"CMakeLists.txt", "dependencies.lock.json",
                                 "tools/compile_shader.py"}) {
        const auto path = engine_root / relative;
        files[relative] = fs::is_regular_file(path) ? Json(sha256_file(path)) : Json(nullptr);
    }
    const auto cmake = engine_root / "cmake";
    if (fs::is_directory(cmake))
        for (const auto& entry : fs::directory_iterator(cmake))
            if (entry.is_regular_file() && entry.path().extension() == ".cmake")
                files[generic_path_to_utf8(entry.path().lexically_relative(engine_root))] =
                    sha256_file(entry.path());
    return files;
}
} // namespace

std::string BuildInputs::fingerprint() const {
    return normalized_hash({{"format", "faset.build-inputs.v1"},
                            {"source", source_hash},
                            {"recipe", recipe_hash},
                            {"toolchain", toolchain_hash}});
}

std::string gameplay_source_hash(const fs::path& project_root, const scripting::LuaProject& lua) {
    Json files = Json::object();
    const auto scripts = project_root / "Scripts";
    if (fs::is_symlink(fs::symlink_status(scripts)))
        throw std::runtime_error("Scripts directory must not be a symlink");
    if (fs::is_directory(scripts))
        for (const auto& entry : fs::recursive_directory_iterator(scripts)) {
            if (entry.is_symlink())
                throw std::runtime_error("Scripts source cannot be a symlink: " +
                                         path_to_utf8(entry.path()));
            if (entry.is_regular_file())
                files[generic_path_to_utf8(entry.path().lexically_relative(project_root))] =
                    sha256_file(entry.path());
        }
    return normalized_hash({{"format", "faset.gameplay-sources.v1"},
                            {"files", files},
                            {"lua", lua.fingerprint}});
}

BuildInputs capture_build_inputs(const BuildConfig& config, const scripting::LuaProject& lua) {
    BuildInputs result;
    result.source_hash = gameplay_source_hash(config.project_root, lua);
    result.recipe_hash = normalized_hash({{"format", "faset.build-recipe.v1"},
                                          {"generator", config.generator},
                                          {"configuration", config.configuration},
                                          {"export_configuration", config.export_configuration},
                                          {"configure_arguments", config.configure_arguments},
                                          {"engine_files", recipe_files(config.engine_root)}});
#ifdef _WIN32
    constexpr auto default_c = "clang-cl", default_cxx = "clang-cl";
#else
    constexpr auto default_c = "clang", default_cxx = "clang++";
#endif
    result.toolchain_hash = normalized_hash(
        {{"format", "faset.native-toolchain.v1"},
         {"cmake", tool_identity(config.cmake)},
         {"c", tool_identity(configured_tool(config, "CMAKE_C_COMPILER", default_c))},
         {"cxx", tool_identity(configured_tool(config, "CMAKE_CXX_COMPILER", default_cxx))},
         {"slang", tool_identity(slang_name(config))}});
    return result;
}

void ensure_native_toolchain_stamp(const fs::path& native_directory,
                                   const BuildInputs& inputs) {
    const auto stamp = native_directory / ".faset-toolchain.json";
    bool matching = false;
    if (fs::is_regular_file(stamp))
        try {
            const auto existing = read_json(stamp);
            matching = existing.value("format", std::string()) == "faset.toolchain-stamp" &&
                       existing.value("version", 0) == 1 &&
                       existing.value("toolchain_hash", std::string()) == inputs.toolchain_hash;
        } catch (const std::exception&) {
            matching = false;
        }
    if (!matching && fs::exists(native_directory))
        fs::remove_all(native_directory);
    fs::create_directories(native_directory);
    if (!matching)
        atomic_write_json(stamp, {{"format", "faset.toolchain-stamp"},
                                  {"version", 1},
                                  {"toolchain_hash", inputs.toolchain_hash}});
}

std::string build_package_key(const BuildInputs& inputs, const fs::path& native_directory,
                              const std::string& configuration, const fs::path& player,
                              const fs::path& exporter) {
    Json files = Json::object();
    for (const auto& [name, path] :
         {std::pair{"player", player}, std::pair{"exporter", exporter},
          std::pair{"cmake_cache", native_directory / "CMakeCache.txt"}}) {
        if (!fs::is_regular_file(path) || fs::is_symlink(path))
            throw std::runtime_error("Native build artifact is missing: " + path_to_utf8(path));
        files[name] = sha256_file(path);
    }
    const auto shaders = native_directory / "shaders";
    if (!fs::is_directory(shaders))
        throw std::runtime_error("Native shader directory is missing");
    for (const auto& entry : fs::recursive_directory_iterator(shaders)) {
        if (entry.is_symlink())
            throw std::runtime_error("Native shader is a symlink");
        if (entry.is_regular_file())
            files[generic_path_to_utf8(entry.path().lexically_relative(native_directory))] =
                sha256_file(entry.path());
    }
    // On Windows, a changed runtime DLL must also invalidate a package hit.
    for (const auto& root : {player.parent_path(), native_directory,
                             native_directory / configuration}) {
        if (!fs::is_directory(root))
            continue;
        for (const auto& entry : fs::directory_iterator(root)) {
            auto extension = path_to_utf8(entry.path().extension());
            for (auto& character : extension)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            if (extension != ".dll")
                continue;
            if (!entry.is_regular_file() || entry.is_symlink())
                throw std::runtime_error("Native runtime DLL is not a regular file");
            files["dll:" + path_to_utf8(entry.path().filename())] = sha256_file(entry.path());
        }
    }
    return normalized_hash({{"format", "faset.package-key.v1"},
                            {"inputs", inputs.fingerprint()},
                            {"configuration", configuration},
                            {"files", files}});
}

bool validate_build_generation(const fs::path& directory, const std::string& package_key) {
    try {
        if (!fs::is_directory(directory) || fs::is_symlink(directory))
            return false;
        const auto manifest_file = directory / "manifest.json";
        if (!fs::is_regular_file(manifest_file) || fs::is_symlink(manifest_file))
            return false;
        const auto manifest = read_json(manifest_file);
        if (manifest.at("format") != "faset.build" || manifest.at("version") != 2 ||
            manifest.at("package_key") != package_key || !manifest.at("files").is_array())
            return false;
        std::set<std::string> expected;
        for (const auto& record : manifest.at("files")) {
            const auto name = record.at("path").get<std::string>();
            const auto relative = path_from_utf8(name);
            if (relative.empty() || relative.is_absolute() || name == "manifest.json" ||
                generic_path_to_utf8(relative) != name)
                return false;
            for (const auto& component : relative)
                if (component == "." || component == "..")
                    return false;
            if (!expected.insert(name).second)
                return false;
            auto file = directory;
            for (const auto& component : relative) {
                file /= component;
                if (fs::is_symlink(file))
                    return false;
            }
            if (!fs::is_regular_file(file) || fs::is_symlink(file) ||
                sha256_file(file) != record.at("sha256").get<std::string>() ||
                fs::file_size(file) != record.at("size").get<std::uintmax_t>())
                return false;
        }
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_symlink())
                return false;
            if (entry.is_regular_file()) {
                const auto name = generic_path_to_utf8(entry.path().lexically_relative(directory));
                if (name != "manifest.json" && !expected.contains(name))
                    return false;
            }
        }
        const auto player_name = manifest.at("player").get<std::string>();
        const auto schema_name = manifest.at("schema").get<std::string>();
        if (!expected.contains(player_name) || !expected.contains(schema_name) ||
            !expected.contains("faset_schema_exporter" + fs::path(player_name).extension().string()))
            return false;
        for (const auto* shader : {"vertexMain", "fragmentMain", "shadowMain",
                                   "gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                   "gpuHzbMain", "gpuPostCullMain"})
            for (const auto* extension : {".spv", ".reflection.json"})
                if (!expected.contains("shaders/" + std::string(shader) + extension))
                    return false;
        const auto schema = read_json(directory / schema_name);
        if (schema.at("format") != "faset.schema" || schema.at("version") != 1 ||
            schema.at("build_fingerprint") != manifest.at("fingerprint"))
            return false;
        (void)authoring::gameplay_schemas(schema);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace faset::editor
