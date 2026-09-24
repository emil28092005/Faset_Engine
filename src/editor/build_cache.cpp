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
std::pair<std::string, std::string> parse_define(const std::string& argument) {
    if (!argument.starts_with("-D"))
        throw std::invalid_argument("Gameplay configure arguments must be -DNAME=value options");
    const auto equal = argument.find('=', 2);
    if (equal == std::string::npos || equal == 2)
        throw std::invalid_argument("Invalid gameplay configure definition");
    const auto colon = argument.find(':', 2);
    const auto key = argument.substr(2, colon < equal ? colon - 2 : equal - 2);
    if (key.empty())
        throw std::invalid_argument("Invalid gameplay configure definition");
    return {key, argument.substr(equal + 1)};
}
void validate_configure_arguments(const BuildConfig& config) {
    static const std::set<std::string> controlled = {
        "FASET_GAMEPLAY_SOURCE_DIR", "FASET_ENABLE_LUA", "CMAKE_BUILD_TYPE", "BUILD_TESTING",
        "FASET_BUILD_EDITOR", "FASET_BUILD_RENDERER", "FASET_BUILD_RUNTIME", "FASET_BUILD_ASSETS"};
    for (const auto& argument : config.configure_arguments)
        if (controlled.contains(parse_define(argument).first))
            throw std::invalid_argument("Cannot override an engine-controlled CMake definition");
}
std::string configured_tool(const BuildConfig& config, std::string_view key,
                            std::string fallback) {
    if (auto value = configured_cmake_value(config, key))
        return *value;
    return fallback;
}
fs::path resolve_tool(const std::string& name, const fs::path& working_directory) {
    const auto supplied = path_from_utf8(name);
    if (supplied.has_parent_path())
        return (supplied.is_absolute() ? supplied : working_directory / supplied).lexically_normal();
    try {
        return find_executable(name);
    } catch (const std::exception&) {
        // CMake reports a missing compiler. Keep the identity deterministic so
        // fixture build tools can exercise publication without native compiling.
        return supplied;
    }
}
Json tool_identity(const std::string& name, const fs::path& working_directory) {
    const auto resolved = resolve_tool(name, working_directory);
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
bool within(const fs::path& path, const fs::path& directory) {
    const auto relative = path.lexically_relative(directory);
    if (relative.empty() || relative.is_absolute())
        return false;
    for (const auto& component : relative)
        if (component == "..")
            return false;
    return true;
}
void refuse_symlink_ancestors(const fs::path& path) {
    fs::path prefix;
    for (const auto& component : path) {
        prefix /= component;
        std::error_code error;
        if (fs::is_symlink(fs::symlink_status(prefix, error)))
            throw std::runtime_error("Native build path contains a symlink: " +
                                     path_to_utf8(prefix));
    }
}
} // namespace

std::optional<std::string> configured_cmake_value(const BuildConfig& config,
                                                   std::string_view key) {
    std::optional<std::string> result;
    for (const auto& argument : config.configure_arguments) {
        const auto [name, value] = parse_define(argument);
        if (name == key)
            result = value;
    }
    return result;
}

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
    validate_configure_arguments(config);
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
         {"cmake", tool_identity(config.cmake, config.project_root)},
         {"c", tool_identity(configured_tool(config, "CMAKE_C_COMPILER", default_c),
                             config.project_root)},
         {"cxx", tool_identity(configured_tool(config, "CMAKE_CXX_COMPILER", default_cxx),
                               config.project_root)},
         {"slang", tool_identity(slang_name(config), config.project_root)},
         {"toolchain_file", tool_identity(configured_tool(config, "CMAKE_TOOLCHAIN_FILE", ""),
                                          config.project_root)}});
    return result;
}

fs::path stage_gameplay_sources(const BuildConfig& config, const BuildInputs& inputs,
                                const scripting::LuaProject& lua, std::string_view job_id) {
    const auto cache = config.cache_root.empty() ? config.project_root / ".faset" / "cache"
                                                  : config.cache_root;
    const auto snapshots = cache / "source-snapshots";
    const auto destination = snapshots / inputs.source_hash;
    const auto verified = [&](const fs::path& root) {
        return fs::is_directory(root / "Scripts") && !fs::is_symlink(root) &&
               gameplay_source_hash(root, lua) == inputs.source_hash;
    };
    if (fs::exists(destination)) {
        if (!verified(destination))
            throw std::runtime_error("Cached gameplay source snapshot is corrupt");
        return destination / "Scripts";
    }
    const auto source = config.project_root / "Scripts";
    if (!fs::is_directory(source) || fs::is_symlink(source))
        throw std::runtime_error("Gameplay Scripts directory is missing or a symlink");
    const auto staging = snapshots / (".staging-" + std::string(job_id));
    if (fs::exists(staging))
        throw std::runtime_error("Gameplay source staging directory already exists");
    fs::create_directories(staging / "Scripts");
    try {
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            if (entry.is_symlink())
                throw std::runtime_error("Gameplay source snapshot contains a symlink");
            const auto target = staging / "Scripts" / entry.path().lexically_relative(source);
            if (entry.is_directory())
                fs::create_directories(target);
            else if (entry.is_regular_file()) {
                fs::create_directories(target.parent_path());
                fs::copy_file(entry.path(), target);
            }
        }
        if (!verified(staging))
            throw std::runtime_error("Gameplay sources changed while creating the build snapshot");
        fs::rename(staging, destination);
        return destination / "Scripts";
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        if (fs::exists(destination) && verified(destination))
            return destination / "Scripts";
        throw;
    }
}

void ensure_native_toolchain_stamp(const BuildConfig& config, const fs::path& native_directory,
                                   const BuildInputs& inputs) {
    const auto project = fs::absolute(config.project_root).lexically_normal();
    const auto base = fs::absolute(config.build_directory).lexically_normal();
    const auto native = fs::absolute(native_directory).lexically_normal();
    const auto cache = fs::absolute(config.cache_root.empty()
                                        ? project / ".faset" / "cache"
                                        : config.cache_root).lexically_normal();
    const auto engine = fs::absolute(config.engine_root).lexically_normal();
    if (native.parent_path() != base ||
        (native.filename() != "Debug" && native.filename() != "Release" &&
         native.filename() != "RelWithDebInfo"))
        throw std::runtime_error("Native build path is outside its configured managed root");
    const auto default_base = project / ".faset" / "build";
    if ((within(base, project) && base != default_base) || within(project, base) ||
        within(base, cache) || within(cache, base) || within(base, engine) ||
        within(engine, base))
        throw std::runtime_error("Native build root overlaps project, cache or engine files");
    refuse_symlink_ancestors(native);
    const auto stamp = native_directory / ".faset-toolchain.json";
    bool matching = false, owned = false;
    if (fs::is_regular_file(stamp) && !fs::is_symlink(stamp))
        try {
            const auto existing = read_json(stamp);
            const auto format = existing.value("format", std::string()) ==
                                "faset.toolchain-stamp";
            const auto version = existing.value("version", 0);
            owned = format &&
                    ((version == 2 &&
                      existing.value("project_root", std::string()) == path_to_utf8(project) &&
                      existing.value("native_directory", std::string()) == path_to_utf8(native)) ||
                     (version == 1 && base == default_base));
            matching = owned &&
                       existing.value("toolchain_hash", std::string()) == inputs.toolchain_hash;
        } catch (const std::exception&) {
            owned = false;
        }
    if (fs::exists(native_directory) && !owned)
        throw std::runtime_error("Refusing to clear an unowned native build directory: " +
                                 path_to_utf8(native));
    if (!matching && fs::exists(native_directory))
        fs::remove_all(native_directory);
    fs::create_directories(native_directory);
    atomic_write_json(stamp, {{"format", "faset.toolchain-stamp"},
                              {"version", 2},
                              {"project_root", path_to_utf8(project)},
                              {"native_directory", path_to_utf8(native)},
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
                                   "gpuHzbMain", "gpuPostCullMain", "lightTileMain",
                                   "temporalResolveMain", "temporalCompositeVertexMain",
                                   "temporalCompositeFragmentMain", "temporalVertexMain",
                                   "temporalFragmentMain", "gpuTemporalVertexMain"})
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
