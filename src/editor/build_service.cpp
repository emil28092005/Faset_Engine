#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <faset/assets/asset_data.hpp>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/authoring/schema.hpp>
#include <faset/authoring/service.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <faset/editor/build_cache.hpp>
#include <faset/editor/build_diagnostics.hpp>
#include <faset/editor/build_service.hpp>
#include <faset/scripting/project.hpp>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace faset::editor {
namespace fs = std::filesystem;
namespace {
struct Cancelled {};
constexpr std::size_t max_log_bytes = 8 * 1024 * 1024;
void validate_scene(const Json& scene) {
    if (!scene.is_object() || scene.value("format", "") != "faset.scene" ||
        scene.value("version", 0) != 1 || !scene.contains("entities") ||
        !scene["entities"].is_array())
        throw std::runtime_error("Cook requires a version 1 Faset scene");
    if (!scene.value("instances", Json::array()).empty())
        throw std::runtime_error("Resolve template instances before cooking");
    int dimension = scene.value("dimension", 0);
    if (dimension != 2 && dimension != 3)
        throw std::runtime_error("Scene dimension must be 2 or 3");
}
void validate_component_types(const Json& scene, const Json& schema) {
    const auto registry =
        schema.is_null() ? authoring::builtin_schemas() : authoring::gameplay_schemas(schema);
    for (const auto& entity : scene.at("entities"))
        for (const auto& component : entity.value("components", Json::array())) {
            const auto id = component.at("type").get<std::string>();
            if (!registry.contains(id))
                throw std::runtime_error("Cannot cook unresolved component type: " + id);
            if (component.value("version", 1) != registry.schema(id).value("version", 1))
                throw std::runtime_error("Migrate component '" + id +
                                         "' to the current gameplay schema before cooking");
        }
}
std::set<std::string> asset_references(const Json& scene) {
    std::set<std::string> result;
    for (const auto& entity : scene.at("entities"))
        for (const auto& component : entity.value("components", Json::array())) {
            const auto type = component.value("type", "");
            if (type != "faset.mesh" && type != "faset.sprite")
                continue;
            const auto fields = component.value("fields", Json::object());
            auto asset = fields.value(type == "faset.sprite" ? "texture" : "asset", "");
            if (asset.empty() || asset == "builtin:cube" || asset == "builtin:plane")
                continue;
            if (asset.starts_with("builtin:"))
                throw std::runtime_error("Unknown builtin asset: " + asset);
            result.insert(asset.substr(0, asset.find('#')));
        }
    return result;
}
void copy_required_file(const fs::path& source, const fs::path& target) {
    if (!fs::is_regular_file(source) || fs::is_symlink(source))
        throw std::runtime_error("Required package file missing or not a regular file: " +
                                 path_to_utf8(source));
    fs::create_directories(target.parent_path());
    fs::copy_file(source, target, fs::copy_options::overwrite_existing);
}
std::string executable_suffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}
fs::path build_executable(const fs::path& build, const std::string& configuration,
                          const std::string& target) {
    for (const auto& root :
         {build, build / configuration, build / "bin", build / "bin" / configuration}) {
        auto file = root / (target + executable_suffix());
        if (fs::is_regular_file(file))
            return file;
    }
    throw std::runtime_error("Build did not produce " + target);
}
Json starter_scene(const std::string& name, int dimension, std::string_view language) {
    const auto schemas = authoring::builtin_schemas();
    auto scene = authoring::make_scene(name + " — Starter", dimension);
    scene["simulation"] = authoring::default_simulation_settings();
    auto add_component = [&](Json& entity, std::string type, Json fields) {
        entity["components"].push_back({{"id", new_id()},
                                        {"type", std::move(type)},
                                        {"version", 1},
                                        {"fields", std::move(fields)}});
    };
    auto ground = authoring::make_entity(schemas, "Ground");
    auto& ground_pose = ground["components"][0]["fields"];
    ground_pose["position"] = {0, -1, 0};
    ground_pose["scale"] = dimension == 2 ? Json::array({12, 1, 1}) : Json::array({12, 1, 12});
    if (dimension == 2) {
        add_component(ground, "faset.sprite", {{"size", {1, 1}},
                                                {"color", {0.20, 0.25, 0.31, 1}}});
    } else {
        add_component(ground, "faset.mesh", {{"asset", "builtin:cube"},
                                              {"color", {0.20, 0.25, 0.31, 1}}});
    }
    auto ground_body = schemas.default_fields("faset.rigid_body_" + std::to_string(dimension) +
                                              "d");
    ground_body["body_type"] = "static";
    add_component(ground, "faset.rigid_body_" + std::to_string(dimension) + "d",
                  std::move(ground_body));
    scene["entities"].push_back(std::move(ground));

    auto actor = authoring::make_entity(schemas, "Player");
    actor["components"][0]["fields"]["position"] = {0, 1, 0};
    if (dimension == 2) {
        add_component(actor, "faset.sprite", {{"size", {0.8, 0.8}},
                                               {"color", {0.25, 0.72, 0.85, 1}}});
    } else {
        actor["components"][0]["fields"]["scale"] = {0.8, 0.8, 0.8};
        add_component(actor, "faset.mesh", {{"asset", "builtin:cube"},
                                             {"color", {0.25, 0.72, 0.85, 1}}});
    }
    add_component(actor, "faset.rigid_body_" + std::to_string(dimension) + "d",
                  schemas.default_fields("faset.rigid_body_" + std::to_string(dimension) + "d"));
    add_component(actor, language == "lua" ? "starter.player" : "gameplay.character",
                  {{"speed", 4.0}, {"jump_speed", 5.0}});
    scene["entities"].push_back(std::move(actor));
    if (dimension == 3) {
        auto camera = authoring::make_entity(schemas, "Camera");
        camera["components"][0]["fields"]["position"] = {0, 2.5, 8};
        camera["components"][0]["fields"]["rotation"] = {-0.18, 0, 0};
        add_component(camera, "faset.camera", schemas.default_fields("faset.camera"));
        scene["entities"].push_back(std::move(camera));
        auto sun = authoring::make_entity(schemas, "Sun");
        add_component(sun, "faset.light", schemas.default_fields("faset.light"));
        scene["entities"].push_back(std::move(sun));
    }
    // Validate every built-in component now; the behavior component is validated by
    // SchemaExporter from the selected C++/Lua module during the first build.
    auto builtins = scene;
    for (auto& entity : builtins["entities"]) {
        auto& components = entity["components"].get_ref<Json::array_t&>();
        std::erase_if(components, [](const Json& component) {
            return !component.at("type").get<std::string>().starts_with("faset.");
        });
    }
    authoring::validate_scene(builtins, schemas);
    return scene;
}
struct StarterCreateLock {
    fs::path path;
    explicit StarterCreateLock(const fs::path& root) : path(root / ".faset/starter-create.lock") {
        if (!fs::create_directory(path))
            throw std::runtime_error("Another project creation is in progress; remove a stale " +
                                     path_to_utf8(path) + " only after closing that Editor");
    }
    ~StarterCreateLock() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    StarterCreateLock(const StarterCreateLock&) = delete;
    StarterCreateLock& operator=(const StarterCreateLock&) = delete;
};
void require_new_starter_directory(const fs::path& root) {
    // BuildService creates .faset/cache in its constructor. Any other content is
    // user-owned, including a previous starter, and must be left intact.
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.path().filename() != ".faset" || !entry.is_directory() ||
            entry.is_symlink())
            throw std::runtime_error("Create requires a new or empty project directory");
        for (const auto& internal : fs::directory_iterator(entry.path())) {
            if (internal.path().filename() == "starter-create.lock" &&
                internal.is_directory() && !internal.is_symlink())
                continue;
            if (internal.path().filename() != "cache" || !internal.is_directory() ||
                internal.is_symlink() || !fs::is_empty(internal.path()))
                throw std::runtime_error("Create requires a new or empty project directory");
        }
    }
}
} // namespace
Json JobStatus::json() const {
    return {{"id", id},       {"kind", kind},         {"state", state},
            {"stage", stage}, {"progress", progress}, {"log", log},
            {"error", error}, {"diagnostics", diagnostics}, {"result", result}};
}
void write_cooked_scene(const fs::path& path, const Json& scene) {
    validate_scene(scene);
    const auto payload = Json::to_cbor(scene);
    std::string bytes = "FASETSCN";
    bytes.reserve(20 + payload.size());
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>((std::uint32_t(1) >> (i * 8)) & 255));
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<char>((std::uint64_t(payload.size()) >> (i * 8)) & 255));
    bytes.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    atomic_write(path, bytes);
}
struct BuildService::Impl {
    struct Job {
        mutable std::mutex mutex;
        JobStatus status;
        std::atomic<bool> cancelled{};
        std::condition_variable finished;
        Json scene;
        Json asset_manifests = Json::object();
        scripting::LuaProject lua;
        fs::path output;
    };
    BuildConfig config;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    bool stopping{};
    std::thread worker;
    explicit Impl(BuildConfig c) : config(std::move(c)) {
        if (config.project_root.empty() || config.engine_root.empty())
            throw std::invalid_argument("Build service requires project and engine directories");
        config.project_root = fs::absolute(config.project_root);
        config.engine_root = fs::absolute(config.engine_root);
        if (config.cache_root.empty())
            config.cache_root = config.project_root / ".faset" / "cache";
        else
            config.cache_root = fs::absolute(config.cache_root);
        if (config.build_directory.empty())
            config.build_directory = config.project_root / ".faset" / "build";
        else
            config.build_directory = fs::absolute(config.build_directory);
        if (config.configuration != "Debug" && config.configuration != "Release" &&
            config.configuration != "RelWithDebInfo")
            throw std::invalid_argument("Unsupported build configuration");
        if (config.export_configuration != "Release" &&
            config.export_configuration != "RelWithDebInfo")
            throw std::invalid_argument("Export configuration must be Release or RelWithDebInfo");
        fs::create_directories(config.project_root);
        fs::create_directories(config.cache_root);
        worker = std::thread([this] { work(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (auto& [_, job] : jobs)
                job->cancelled = true;
        }
        condition.notify_all();
        if (worker.joinable())
            worker.join();
    }
    std::shared_ptr<Job> lookup(const std::string& id) const {
        std::lock_guard lock(mutex);
        auto it = jobs.find(id);
        if (it == jobs.end())
            throw std::out_of_range("Unknown build job: " + id);
        return it->second;
    }
    std::string enqueue(std::string kind, Json scene = {}, fs::path output = {}) {
        auto job = std::make_shared<Job>();
        job->status.id = new_id();
        job->status.kind = std::move(kind);
        job->scene = std::move(scene);
        job->output = std::move(output);
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw std::runtime_error("Build service is stopping");
            jobs.emplace(job->status.id, job);
            queue.push_back(job);
        }
        condition.notify_all();
        return job->status.id;
    }
    void checkpoint(Job& job, std::string stage, double progress) {
        if (job.cancelled)
            throw Cancelled{};
        std::lock_guard lock(job.mutex);
        job.status.stage = std::move(stage);
        job.status.progress = progress;
    }
    void log(Job& job, std::string_view text) {
        std::lock_guard lock(job.mutex);
        job.status.log.append(text);
        if (job.status.log.size() > max_log_bytes)
            job.status.log.erase(0, job.status.log.size() - max_log_bytes);
    }
    std::string run(Job& job, std::vector<std::string> arguments, const fs::path& cwd,
                    const fs::path& snapshot_scripts = {}) {
        if (job.cancelled)
            throw Cancelled{};
        std::string phase;
        {
            std::lock_guard lock(job.mutex);
            phase = job.status.stage;
        }
        std::string description = "$";
        for (const auto& argument : arguments)
            description += " " + Json(argument).dump();
        description += '\n';
        log(job, description);
        Process process({std::move(arguments), cwd, {}});
        std::string output;
        std::string pending;
        bool saw_error = false;
        const auto append_diagnostic = [&](const Json& row) {
            const bool error = row.value("severity", "") == "error";
            saw_error |= error;
            std::lock_guard lock(job.mutex);
            if (job.status.diagnostics.size() >= 200) {
                if (!error)
                    return;
                auto previous = std::find_if(job.status.diagnostics.begin(),
                                             job.status.diagnostics.end(), [](const Json& entry) {
                                                 return entry.value("severity", "") != "error";
                                             });
                if (previous == job.status.diagnostics.end())
                    previous = job.status.diagnostics.begin();
                job.status.diagnostics.erase(previous);
            }
            job.status.diagnostics.push_back(row);
        };
        const auto collect_diagnostics = [&](std::string_view chunk, bool final) {
            pending.append(chunk);
            std::size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const auto line = pending.substr(0, newline + 1);
                pending.erase(0, newline + 1);
                const auto found = parse_build_diagnostics(
                    line, phase, config.project_root, snapshot_scripts);
                for (const auto& row : found)
                    append_diagnostic(row);
            }
            if (final && !pending.empty()) {
                const auto found = parse_build_diagnostics(
                    pending, phase, config.project_root, snapshot_scripts);
                for (const auto& row : found)
                    append_diagnostic(row);
                pending.clear();
            } else if (pending.size() > 8192)
                pending.erase(0, pending.size() - 8192);
        };
        while (true) {
            if (job.cancelled) {
                process.cancel();
                auto final = process.poll();
                log(job, final.output);
                throw Cancelled{};
            }
            auto poll = process.poll();
            log(job, poll.output);
            collect_diagnostics(poll.output, !poll.running);
            output += poll.output;
            if (output.size() > max_log_bytes)
                output.erase(0, output.size() - max_log_bytes);
            if (!poll.running) {
                if (poll.exit_code.value_or(1) != 0) {
                    if (!saw_error)
                        append_diagnostic(
                            {{"severity", "error"},
                             {"phase", phase},
                             {"message", "Process exited with code " +
                                             std::to_string(poll.exit_code.value_or(1)) +
                                             "; see job log"}});
                    throw std::runtime_error("Process exited with code " +
                                             std::to_string(poll.exit_code.value_or(1)) +
                                             "; see job log");
                }
                return output;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    void validate_assets(const Json& scene) {
        assets::AssetPipeline pipeline(config.cache_root);
        for (const auto& id : asset_references(scene)) {
            pipeline.load_asset(id);
            const auto freshness = pipeline.freshness(id);
            if (freshness.value("state", std::string("unavailable")) != "current")
                throw std::runtime_error("Asset " + id +
                                         " is stale or unavailable. Reimport before cooking or "
                                         "exporting. Details: " +
                                         freshness.dump());
        }
    }
    Json build(Job& job, bool exporting = false) {
        const auto started = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
        };
        const auto& configuration = exporting ? config.export_configuration : config.configuration;
        const auto native_directory = config.build_directory / configuration;
        checkpoint(job, "Configuring gameplay", .05);
        job.lua = scripting::loadLuaProject(config.project_root);
        const auto inputs = capture_build_inputs(config, job.lua);
        const auto staged_scripts = stage_gameplay_sources(config, inputs, job.lua, job.status.id);
        const auto cpp = staged_scripts / "Gameplay.cpp";
        const auto hpp = staged_scripts / "Gameplay.hpp";
        const bool has_cpp = fs::is_regular_file(cpp), has_hpp = fs::is_regular_file(hpp);
        if (has_cpp != has_hpp || (!has_cpp && !job.lua.enabled()))
            throw std::runtime_error("Project requires Scripts/Gameplay.cpp and Gameplay.hpp, "
                                     "or Lua entry scripts declared in project.faset.json");
        ensure_native_toolchain_stamp(config, native_directory, inputs);
        std::vector<std::string> arguments = {config.cmake,
                                              "-S",
                                              path_to_utf8(config.engine_root),
                                              "-B",
                                              path_to_utf8(native_directory),
                                              "-G",
                                              config.generator,
                                              "-DCMAKE_BUILD_TYPE=" + configuration,
                                              "-DBUILD_TESTING=OFF",
                                              "-DFASET_BUILD_EDITOR=OFF",
                                              "-DFASET_BUILD_RENDERER=ON",
                                              "-DFASET_BUILD_RUNTIME=ON",
                                              "-DFASET_BUILD_ASSETS=ON",
                                              "-DFASET_GAMEPLAY_SOURCE_DIR=" +
                                                  path_to_utf8(staged_scripts)};
        if (!configured_cmake_value(config, "CMAKE_C_COMPILER")) {
#ifdef _WIN32
            auto compiler = find_executable("clang-cl");
            arguments.push_back("-DCMAKE_C_COMPILER=" + path_to_utf8(compiler));
#else
            arguments.push_back("-DCMAKE_C_COMPILER=" + path_to_utf8(find_executable("clang")));
#endif
        }
        if (!configured_cmake_value(config, "CMAKE_CXX_COMPILER")) {
#ifdef _WIN32
            arguments.push_back("-DCMAKE_CXX_COMPILER=" +
                                path_to_utf8(find_executable("clang-cl")));
#else
            arguments.push_back("-DCMAKE_CXX_COMPILER=" + path_to_utf8(find_executable("clang++")));
#endif
        }
        for (const auto& argument : config.configure_arguments) {
            const auto equal = argument.find('=');
            const auto colon = argument.find(':', 2);
            const auto key = argument.substr(2, colon < equal ? colon - 2 : equal - 2);
            if (key == "CMAKE_C_COMPILER" || key == "CMAKE_CXX_COMPILER" ||
                key == "SLANGC_EXECUTABLE" || key == "CMAKE_TOOLCHAIN_FILE") {
                const auto value = path_from_utf8(argument.substr(equal + 1));
                if (value.has_parent_path() && value.is_relative()) {
                    arguments.push_back(argument.substr(0, equal + 1) +
                                        path_to_utf8((config.project_root / value).lexically_normal()));
                    continue;
                }
            }
            arguments.push_back(argument);
        }
        arguments.push_back("-DCMAKE_BUILD_TYPE=" + configuration);
        // Project declarations, not a stale cache or a user-supplied override, determine
        // whether the packaged game has a Lua VM linked into it.
        arguments.push_back(std::string("-DFASET_ENABLE_LUA=") +
                            (job.lua.enabled() ? "ON" : "OFF"));
        run(job, std::move(arguments), config.project_root, staged_scripts);
        const auto configured = std::chrono::steady_clock::now();
        checkpoint(job, "Compiling and linking Player", .25);
        run(job,
            {config.cmake, "--build", path_to_utf8(native_directory), "--config", configuration,
             "--parallel", "4", "--target", "faset_player", "faset_schema_exporter"},
            config.project_root, staged_scripts);
        const auto compiled = std::chrono::steady_clock::now();
        auto player = build_executable(native_directory, configuration, "faset_player");
        auto exporter = build_executable(native_directory, configuration, "faset_schema_exporter");
        const auto source_unchanged = [&] {
            const auto snapshot_root = staged_scripts.parent_path();
            if (fs::is_symlink(fs::symlink_status(snapshot_root)) ||
                !fs::is_directory(staged_scripts) ||
                gameplay_source_hash(snapshot_root, job.lua) != inputs.source_hash)
                throw std::runtime_error(
                    "Gameplay source snapshot changed during the build; build again");
            if (gameplay_source_hash(config.project_root,
                                     scripting::loadLuaProject(config.project_root)) !=
                inputs.source_hash)
                throw std::runtime_error("Gameplay sources changed during the build; build again");
        };
        source_unchanged();
        const auto package_key =
            build_package_key(inputs, native_directory, configuration, player, exporter);
        const auto result_for = [&](const std::string& id, const std::string& fingerprint,
                                    bool reused) -> Json {
            const auto directory = config.cache_root / "builds" / id;
            const auto finished = std::chrono::steady_clock::now();
            return {{"generation", id},
                    {"directory", path_to_utf8(directory)},
                    {"build_directory", path_to_utf8(native_directory)},
                    {"configuration", configuration},
                    {"player", path_to_utf8(directory / ("faset_player" + executable_suffix()))},
                    {"schema", path_to_utf8(directory / "schema.json")},
                    {"lua_enabled", job.lua.enabled()},
                    {"lua_fingerprint", job.lua.fingerprint},
                    {"fingerprint", fingerprint},
                    {"schema_cache_hit", reused},
                    {"generation_reused", reused},
                    {"phase_times_ms",
                     {{"configure", milliseconds(started, configured)},
                      {"native_build", milliseconds(configured, compiled)},
                      {"schema_package", reused ? 0 : milliseconds(compiled, finished)},
                      {"total", milliseconds(started, finished)}}}};
        };
        const auto pointer_file = config.cache_root / "last_build.json";
        std::optional<std::pair<std::string, std::string>> cached_generation;
        if (fs::is_regular_file(pointer_file))
            try {
                const auto pointer = read_json(pointer_file);
                const auto id = pointer.at("generation").get<std::string>();
                const auto candidate =
                    project_path(config.cache_root, fs::path("builds") / id);
                if (validate_build_generation(candidate, package_key)) {
                    const auto manifest = read_json(candidate / "manifest.json");
                    cached_generation =
                        {id, manifest.at("fingerprint").get<std::string>()};
                }
            } catch (const std::exception&) {
                // A bad pointer or old/corrupt generation is a cache miss.
            }
        if (cached_generation) {
            source_unchanged();
            checkpoint(job, "Reusing verified build generation", .68);
            log(job, "Verified schema/package cache hit: " + cached_generation->first + "\n");
            return result_for(cached_generation->first, cached_generation->second, true);
        }
        checkpoint(job, "Exporting gameplay schema", .58);
        const auto staging = config.cache_root / "builds" / (".staging-" + job.status.id);
        const auto generation = config.cache_root / "builds" / job.status.id;
        fs::create_directories(staging);
        try {
            const auto schema_file = staging / "schema.json";
            std::vector<std::string> export_arguments = {path_to_utf8(exporter), "--output",
                                                         path_to_utf8(schema_file)};
            if (job.lua.enabled()) {
                scripting::writeLuaSources(job.lua, staging);
                atomic_write_json(staging / "project.faset.json",
                                  {{"format", "faset.project"},
                                   {"version", 1},
                                   {"scripting", {{"lua", {{"scripts", job.lua.scripts}}}}}});
                export_arguments.insert(export_arguments.end(),
                                        {"--project", path_to_utf8(staging)});
            }
            run(job, std::move(export_arguments), config.project_root,
                job.lua.enabled() ? staging / "Scripts" : staged_scripts);
            auto schema = read_json(schema_file);
            if (schema.value("format", "") != "faset.schema" || schema.value("version", 0) != 1 ||
                !schema.contains("types") || !schema.at("types").is_array())
                throw std::runtime_error("SchemaExporter returned an invalid manifest");
            // Validate the complete metadata before publishing either the schema or
            // its Player generation. Session uses this same authoring contract.
            (void)authoring::gameplay_schemas(schema);
            const auto fingerprint = package_key;
            schema["build_fingerprint"] = fingerprint;
            if (job.lua.enabled())
                schema["lua_fingerprint"] = job.lua.fingerprint;
            atomic_write_json(schema_file, schema);
            copy_required_file(player, staging / ("faset_player" + executable_suffix()));
            copy_required_file(exporter, staging / ("faset_schema_exporter" + executable_suffix()));
            for (const auto* file : {"vertexMain.spv", "fragmentMain.spv", "shadowMain.spv",
                                     "lightTileMain.spv", "lightTileMain.reflection.json",
                                     "vertexMain.reflection.json", "fragmentMain.reflection.json",
                                     "shadowMain.reflection.json", "gpuVertexMain.spv",
                                     "gpuShadowMain.spv", "gpuCullMain.spv", "gpuHzbMain.spv",
                                     "gpuPostCullMain.spv", "gpuVertexMain.reflection.json",
                                     "gpuShadowMain.reflection.json", "gpuCullMain.reflection.json",
                                     "gpuHzbMain.reflection.json", "gpuPostCullMain.reflection.json",
                                     "temporalResolveMain.spv", "temporalResolveMain.reflection.json",
                                     "temporalCompositeVertexMain.spv",
                                     "temporalCompositeVertexMain.reflection.json",
                                     "temporalCompositeFragmentMain.spv",
                                     "temporalCompositeFragmentMain.reflection.json",
                                     "temporalVertexMain.spv", "temporalVertexMain.reflection.json",
                                     "temporalFragmentMain.spv", "temporalFragmentMain.reflection.json",
                                     "gpuTemporalVertexMain.spv",
                                     "gpuTemporalVertexMain.reflection.json"})
                copy_required_file(native_directory / "shaders" / file, staging / "shaders" / file);
            copy_runtime_libraries(job, player, staging, native_directory, configuration);
            Json files = Json::array();
            for (const auto& entry : fs::recursive_directory_iterator(staging)) {
                if (entry.is_symlink())
                    throw std::runtime_error("Build generation contains a symlink");
                if (entry.is_regular_file())
                    files.push_back(
                        {{"path", generic_path_to_utf8(entry.path().lexically_relative(staging))},
                         {"sha256", sha256_file(entry.path())},
                         {"size", entry.file_size()}});
            }
            Json manifest{{"format", "faset.build"},
                          {"version", 2},
                          {"id", job.status.id},
                          {"fingerprint", fingerprint},
                          {"package_key", package_key},
                          {"configuration", configuration},
                          {"lua_enabled", job.lua.enabled()},
                          {"lua_fingerprint", job.lua.fingerprint},
                          {"player", "faset_player" + executable_suffix()},
                          {"schema", "schema.json"},
                          {"files", files}};
            atomic_write_json(staging / "manifest.json", manifest);
            checkpoint(job, "Publishing build generation", .68);
            if (job.lua.enabled() &&
                scripting::loadLuaProject(staging).fingerprint != job.lua.fingerprint)
                throw std::runtime_error("Lua build snapshot changed during schema export");
            source_unchanged();
            if (!validate_build_generation(staging, package_key))
                throw std::runtime_error("Build generation failed final integrity validation");
            fs::rename(staging, generation);
            atomic_write_json(config.cache_root / "last_build.json",
                              {{"generation", job.status.id}, {"fingerprint", fingerprint}});
            return result_for(job.status.id, fingerprint, false);
        } catch (...) {
            std::error_code error;
            fs::remove_all(staging, error);
            throw;
        }
    }
    void copy_runtime_libraries(Job& job, const fs::path& executable, const fs::path& destination,
                                const fs::path& native_directory,
                                const std::string& configuration) {
#ifdef _WIN32
        // Libraries produced by the selected toolchain are copied beside the executable.
        // System DLLs (including Vulkan) remain platform prerequisites.
        for (const auto& root :
             {executable.parent_path(), native_directory, native_directory / configuration})
            if (fs::is_directory(root))
                for (const auto& entry : fs::directory_iterator(root)) {
                    auto extension = path_to_utf8(entry.path().extension());
                    for (auto& c : extension)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (entry.is_regular_file() && extension == ".dll")
                        copy_required_file(entry.path(), destination / entry.path().filename());
                }
        // The actual packaged executable is launched before publication to reject missing imports.
        (void)job;
#else
        const auto output =
            run(job, {path_to_utf8(find_executable("ldd")), path_to_utf8(executable)},
                config.project_root);
        if (output.find("not found") != std::string::npos)
            throw std::runtime_error("Player has unresolved shared library dependencies");
        // SDL/physics/gameplay are linked statically. glibc/libstdc++/Vulkan are the host baseline.
        // Refuse an unexpected private DSO rather than silently publish a non-portable package.
        std::size_t begin{};
        while (begin < output.size()) {
            auto end = output.find('\n', begin);
            auto line = output.substr(begin, end == std::string::npos ? end : end - begin);
            auto arrow = line.find("=> ");
            if (arrow != std::string::npos) {
                auto name = line.substr(0, arrow);
                name.erase(0, name.find_first_not_of(" \t"));
                auto path_start = arrow + 3;
                auto path_end = line.find(" (", path_start);
                auto path = line.substr(path_start, path_end - path_start);
                if (!path.empty() && path[0] == '/' && !path.starts_with("/lib/") &&
                    !path.starts_with("/lib64/") && !path.starts_with("/usr/lib/") &&
                    !path.starts_with("/usr/lib64/"))
                    throw std::runtime_error(
                        "Private shared library needs an explicit package rule: " + path);
            }
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        (void)destination;
        (void)native_directory;
        (void)configuration;
#endif
    }
    Json cook(Job& job) {
        checkpoint(job, "Validating scene and assets", .1);
        validate_scene(job.scene);
        validate_assets(job.scene);
        Json schema;
        if (fs::exists(config.cache_root / "last_build.json")) {
            auto pointer = read_json(config.cache_root / "last_build.json");
            auto schema_path = project_path(
                config.cache_root,
                fs::path("builds") / pointer.at("generation").get<std::string>() / "schema.json");
            schema = read_json(schema_path);
        }
        validate_component_types(job.scene, schema);
        auto source_hash = sha256(job.scene.dump());
        auto schema_fingerprint =
            schema.is_null() ? std::string("builtin-v1")
                             : schema.value("build_fingerprint", std::string("builtin-v1"));
        auto digest = sha256(source_hash + schema_fingerprint);
        auto directory = config.cache_root / "cooked" / digest;
        auto staging = config.cache_root / "cooked" / (".staging-" + job.status.id);
        fs::create_directories(staging);
        try {
            checkpoint(job, "Writing cooked scene", .6);
            write_cooked_scene(staging / "scene.fscene", job.scene);
            Json manifest{{"format", "faset.cooked-scene"},
                          {"version", 1},
                          {"scene", job.scene.value("id", "")},
                          {"source_hash", source_hash},
                          {"schema_fingerprint", schema_fingerprint},
                          {"sha256", sha256_file(staging / "scene.fscene")}};
            atomic_write_json(staging / "manifest.json", manifest);
            checkpoint(job, "Publishing cooked scene", .9);
            if (fs::exists(directory)) {
                if (read_json(directory / "manifest.json") != manifest ||
                    sha256_file(directory / "scene.fscene") !=
                        manifest.at("sha256").get<std::string>())
                    throw std::runtime_error("Existing cooked generation is corrupt");
                fs::remove_all(staging);
            } else
                fs::rename(staging, directory);
            atomic_write_json(config.cache_root / "last_cook.json", {{"generation", digest}});
            return {{"generation", digest},
                    {"scene", path_to_utf8(directory / "scene.fscene")},
                    {"directory", path_to_utf8(directory)}};
        } catch (...) {
            std::error_code error;
            fs::remove_all(staging, error);
            throw;
        }
    }
    void package_notices(const fs::path& destination, const fs::path& native_directory,
                         bool lua_enabled) {
        fs::create_directories(destination);
        auto lock = read_json(config.engine_root / "dependencies.lock.json");
        const std::vector<std::string> runtime_dependencies = {"sdl3",  "entt", "box2d",
                                                               "box3d", "json", "stb"};
        Json used = Json::object();
        for (const auto& name : runtime_dependencies) {
            std::vector<fs::path> roots = {native_directory / "_deps" / (name + "-src"),
                                           config.engine_root / ".cache" / "deps-src" / name};
            bool copied{};
            for (const auto& source : roots) {
                if (!fs::is_directory(source))
                    continue;
                for (const auto& entry : fs::directory_iterator(source)) {
                    if (!entry.is_regular_file())
                        continue;
                    auto filename = path_to_utf8(entry.path().filename());
                    std::string upper = filename;
                    for (auto& c : upper)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (upper.starts_with("LICENSE") || upper.starts_with("COPYING") ||
                        upper.starts_with("NOTICE")) {
                        copy_required_file(entry.path(),
                                           destination / name / entry.path().filename());
                        copied = true;
                    }
                }
                if (copied)
                    break;
            }
            if (!copied)
                throw std::runtime_error("Cannot package required license notices for " + name);
            used[name] = lock.at("dependencies").at(name);
        }
        if (lua_enabled) {
            copy_required_file(config.engine_root / "docs" / "licenses" / "Lua.txt",
                               destination / "lua" / "LICENSE.txt");
            used["lua"] = lock.at("dependencies").at("lua");
        }
        atomic_write_json(destination / "dependencies.json", used);
        if (fs::is_regular_file(config.engine_root / "LICENSE"))
            copy_required_file(config.engine_root / "LICENSE", destination / "Faset-LICENSE");
        atomic_write(destination / "Faset-NOTICE.txt",
                     "Faset Engine\nhttps://github.com/emil28092005/Faset_Engine\nSee the source "
                     "repository for the current license status of Faset's own code.\nThird-party "
                     "license texts are included in the adjacent directories.\n");
    }
    void package_assets(Job& job, const fs::path& destination) {
        assets::AssetStore packaged(destination);
        for (const auto& id : asset_references(job.scene)) {
            if (job.cancelled)
                throw Cancelled{};
            auto manifest = job.asset_manifests.at(id);
            auto generation = manifest.at("generation").get<std::string>();
            auto source_directory = project_path(config.cache_root, fs::path("assets") / id /
                                                                        "generations" / generation);
            auto target = destination / "assets" / id / "generations" / generation;
            for (const auto& file : manifest.at("files")) {
                auto relative = path_from_utf8(file.at("path").get<std::string>());
                copy_required_file(project_path(source_directory, relative),
                                   project_path(target, relative));
            }
            manifest["source"] = "<cooked>";
            if (manifest.contains("payload_source"))
                manifest["payload_source"] = "<cooked>";
            atomic_write_json(target / "manifest.json", manifest);
            atomic_write_json(
                destination / "assets" / id / "current.json",
                {{"schema_version", 1}, {"generation", generation}, {"source", "<cooked>"}});
            packaged.load_asset(id);
        }
    }
    Json export_game(Job& job) {
        validate_scene(job.scene);
        validate_assets(job.scene);
        assets::AssetStore source(config.cache_root);
        for (const auto& id : asset_references(job.scene))
            job.asset_manifests[id] = source.current_manifest(id);
        const auto built = build(job, true);
        validate_component_types(job.scene,
                                 read_json(path_from_utf8(built.at("schema").get<std::string>())));
        auto output = fs::absolute(job.output);
        if (output.empty())
            throw std::runtime_error("An export destination is required");
        fs::create_directories(output / "generations");
        auto staging = output / (".staging-" + job.status.id);
        auto generation = output / "generations" / job.status.id;
        fs::create_directories(staging);
        try {
            checkpoint(job, "Cooking export snapshot", .72);
            write_cooked_scene(staging / "scene.fscene", job.scene);
            auto build_directory = path_from_utf8(built.at("directory").get<std::string>());
            if (job.lua.enabled()) {
                // Never read live Scripts files for a published game: schemas, source,
                // and fingerprint all originate in the same validated build snapshot.
                const auto captured = scripting::loadLuaProject(build_directory);
                if (captured.fingerprint != job.lua.fingerprint)
                    throw std::runtime_error("Lua build snapshot is corrupt");
                scripting::writeLuaSources(captured, staging);
                copy_required_file(build_directory / "project.faset.json",
                                   staging / "project.faset.json");
            }
            copy_required_file(build_directory / ("faset_player" + executable_suffix()),
                               staging / ("faset_player" + executable_suffix()));
            for (const auto* shader : {"vertexMain.spv", "fragmentMain.spv", "shadowMain.spv",
                                       "lightTileMain.spv", "lightTileMain.reflection.json",
                                       "vertexMain.reflection.json", "fragmentMain.reflection.json",
                                       "shadowMain.reflection.json", "gpuVertexMain.spv",
                                       "gpuShadowMain.spv", "gpuCullMain.spv", "gpuHzbMain.spv",
                                       "gpuPostCullMain.spv", "gpuVertexMain.reflection.json",
                                       "gpuShadowMain.reflection.json", "gpuCullMain.reflection.json",
                                       "gpuHzbMain.reflection.json", "gpuPostCullMain.reflection.json",
                                       "temporalResolveMain.spv", "temporalResolveMain.reflection.json",
                                       "temporalCompositeVertexMain.spv",
                                       "temporalCompositeVertexMain.reflection.json",
                                       "temporalCompositeFragmentMain.spv",
                                       "temporalCompositeFragmentMain.reflection.json",
                                       "temporalVertexMain.spv", "temporalVertexMain.reflection.json",
                                       "temporalFragmentMain.spv", "temporalFragmentMain.reflection.json",
                                       "gpuTemporalVertexMain.spv",
                                       "gpuTemporalVertexMain.reflection.json"})
                copy_required_file(build_directory / "shaders" / shader,
                                   staging / "shaders" / shader);
            for (const auto& entry : fs::directory_iterator(build_directory)) {
                auto extension = path_to_utf8(entry.path().extension());
                for (auto& c : extension)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (entry.is_regular_file() && extension == ".dll")
                    copy_required_file(entry.path(), staging / entry.path().filename());
            }
            checkpoint(job, "Packaging assets and notices", .80);
            package_assets(job, staging);
            package_notices(staging / "Notices",
                            path_from_utf8(built.at("build_directory").get<std::string>()),
                            job.lua.enabled());
            atomic_write(
                staging / "README.txt",
                "Run faset_player" + executable_suffix() +
                    " to start this game.\nThe executable loads scene.fscene and assets beside "
                    "it.\nKeep shaders/, assets/, Notices/, and any Scripts/ and "
                    "project.faset.json "
                    "with the executable.\nA compatible "
                    "Vulkan 1.3 driver and the supported OS runtime are required.\n");
#ifdef _WIN32
            atomic_write(staging / "Windows-Runtime.txt",
                         "Install the Microsoft Visual C++ x64 Redistributable if Windows reports "
                         "a missing MSVCP140 or VCRUNTIME140 DLL.\nOfficial installer: "
                         "https://aka.ms/vc14/vc_redist.x64.exe\nThe Vulkan loader and GPU driver "
                         "must also be installed.\n");
#endif
            checkpoint(job, "Validating packaged Player", .90);
            auto validation_output =
                run(job,
                    {path_to_utf8(staging / ("faset_player" + executable_suffix())), "--validate",
                     "--scene", path_to_utf8(staging / "scene.fscene"), "--assets",
                     path_to_utf8(staging)},
                    staging);
            if (job.lua.enabled() &&
                scripting::loadLuaProject(staging).fingerprint != job.lua.fingerprint)
                throw std::runtime_error("Packaged Lua snapshot changed during validation");
            Json files = Json::array();
            for (const auto& entry : fs::recursive_directory_iterator(staging)) {
                if (entry.is_symlink())
                    throw std::runtime_error("Export contains a symlink");
                if (entry.is_regular_file())
                    files.push_back(
                        {{"path", generic_path_to_utf8(entry.path().lexically_relative(staging))},
                         {"sha256", sha256_file(entry.path())},
                         {"size", entry.file_size()}});
            }
            Json manifest{{"format", "faset.export"},
                          {"version", 1},
                          {"generation", job.status.id},
                          {"build_fingerprint", built.at("fingerprint")},
                          {"lua_enabled", job.lua.enabled()},
                          {"lua_fingerprint", job.lua.fingerprint},
                          {"scene_hash", sha256(job.scene.dump())},
                          {"asset_generations", Json::object()},
                          {"configuration", built.at("configuration")},
                          {"executable", "faset_player" + executable_suffix()},
                          {"files", files}};
            manifest["units"] = {
                {"distance", "metre"}, {"angle", "radian"}, {"coordinates", "right-handed Y-up"}};
            manifest["simulation"] = {{"fixed_delta", 1.0 / 60.0},
                                      {"max_catch_up_ticks", 4},
                                      {"physics_substeps", 4},
                                      {"gravity", {0, -9.81, 0}}};
            if (job.scene.contains("simulation"))
                manifest["simulation"].update(job.scene.at("simulation"));
            manifest["renderer_profile"] = {
                {"api", "Vulkan 1.3"},
                {"required_features", {"dynamicRendering", "synchronization2"}},
                {"materials", {"base-color factor and texture", "metallic and roughness factors"}},
                {"texture_sampling", "linear clamp, one mip level"},
                {"shadow_map", {{"resolution", 1024}, {"world_extent", 40}}},
                {"unsupported_material_features",
                 {"normal maps", "metallic-roughness maps", "emissive and occlusion maps",
                  "alpha mode selection", "unlit mode", "per-material face culling"}}};
            manifest["validation_log"] = validation_output;
            for (const auto& [id, asset] : job.asset_manifests.items())
                manifest["asset_generations"][id] = asset.at("generation");
#ifdef _WIN32
            manifest["platform"] = "windows";
            manifest["prerequisites"] = {
                "Windows x64", "Vulkan 1.3 driver",
                "Microsoft Visual C++ x64 Redistributable (Visual Studio 2022 or newer)"};
#else
            manifest["platform"] = "linux";
            manifest["prerequisites"] = {"Linux x86_64", "Vulkan 1.3 driver",
                                         "Compatible glibc and libstdc++ runtime"};
#endif
            atomic_write_json(staging / "manifest.json", manifest);
            checkpoint(job, "Publishing export generation", .98);
            fs::rename(staging, generation);
            atomic_write_json(output / "current.json",
                              {{"format", "faset.export-pointer"},
                               {"version", 1},
                               {"generation", job.status.id},
                               {"directory", "generations/" + job.status.id}});
            return {
                {"directory", path_to_utf8(generation)},
                {"executable", path_to_utf8(generation / ("faset_player" + executable_suffix()))},
                {"manifest", path_to_utf8(generation / "manifest.json")},
                {"generation", job.status.id},
                {"build", built},
                {"schema", built.at("schema")},
                {"player", built.at("player")},
                {"build_directory", built.at("build_directory")},
                {"configuration", built.at("configuration")}};
        } catch (...) {
            std::error_code error;
            fs::remove_all(staging, error);
            throw;
        }
    }
    void work() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    if (stopping)
                        return;
                    continue;
                }
                job = queue.front();
                queue.pop_front();
            }
            {
                std::lock_guard lock(job->mutex);
                job->status.state = "running";
            }
            try {
                if (job->cancelled)
                    throw Cancelled{};
                Json result;
                if (job->status.kind == "build")
                    result = build(*job);
                else if (job->status.kind == "cook")
                    result = cook(*job);
                else
                    result = export_game(*job);
                std::lock_guard lock(job->mutex);
                job->status.state = "succeeded";
                job->status.stage = "Complete";
                job->status.progress = 1;
                job->status.result = std::move(result);
            } catch (const Cancelled&) {
                std::lock_guard lock(job->mutex);
                job->status.state = "cancelled";
                job->status.stage = "Cancelled";
                job->status.error =
                    "Job cancelled; previous published generations remain available";
            } catch (const std::exception& error) {
                std::lock_guard lock(job->mutex);
                job->status.state = "failed";
                job->status.stage = "Failed";
                job->status.error = error.what();
            }
            job->finished.notify_all();
            condition.notify_all();
        }
    }
};
BuildService::BuildService(BuildConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
BuildService::~BuildService() = default;
const BuildConfig& BuildService::config() const {
    return impl_->config;
}
void BuildService::scaffold(const std::string& name, int dimension) {
    if (name.empty() || (dimension != 2 && dimension != 3))
        throw std::invalid_argument("Project name and dimension 2 or 3 required");
    const auto& c = impl_->config;
    fs::create_directories(c.project_root / "Scripts");
    fs::create_directories(c.project_root / "Scenes");
    fs::create_directories(c.project_root / "Assets");
    for (const auto* file : {"Gameplay.cpp", "Gameplay.hpp"}) {
        auto target = c.project_root / "Scripts" / file;
        if (!fs::exists(target)) {
            auto source = c.engine_root / "tools" / "project_templates" / file;
            copy_required_file(source, target);
        }
    }
    auto project = c.project_root / "project.faset.json";
    if (!fs::exists(project))
        atomic_write_json(project, {{"format", "faset.project"},
                                    {"version", 1},
                                    {"id", new_id()},
                                    {"name", name},
                                    {"dimension", dimension},
                                    {"start_scene", "Scenes/main.scene.json"}});
    if (!fs::exists(c.project_root / ".gitignore"))
        atomic_write(c.project_root / ".gitignore", ".faset/\nExports/\n");
}
void BuildService::scaffold(const std::string& name, int dimension,
                            std::string_view language) {
    if (name.empty() || (dimension != 2 && dimension != 3) ||
        (language != "cpp" && language != "lua"))
        throw std::invalid_argument("Starter requires a name, 2D/3D and cpp/lua language");
    const auto& c = impl_->config;
    StarterCreateLock lock(c.project_root);
    require_new_starter_directory(c.project_root);
    const auto scene = starter_scene(name, dimension, language);
    const auto source = c.engine_root / "tools/project_templates" /
                        (language == "lua" ? "lua-main.lua" : "Gameplay.cpp");
    const auto module = read_text(source);
    const auto header = language == "cpp"
                            ? read_text(c.engine_root / "tools/project_templates/Gameplay.hpp")
                            : std::string();
    const auto lua_annotations = language == "lua"
                                     ? read_text(c.engine_root / "tools/lua/faset.lua")
                                     : std::string();
    const auto lua_config = language == "lua"
                                ? read_json(c.engine_root / "tools/lua/luarc.json")
                                : Json();
    const auto lua_scripts_config =
        language == "lua" ? read_json(c.engine_root / "tools/lua/luarc-scripts.json") : Json();
    const auto stage = lock.path / "staged";
    fs::create_directories(stage / "Scripts");
    fs::create_directories(stage / "Scenes");
    fs::create_directories(stage / "Assets");
    if (language == "cpp") {
        atomic_write(stage / "Scripts/Gameplay.cpp", module);
        atomic_write(stage / "Scripts/Gameplay.hpp", header);
    } else {
        atomic_write(stage / "Scripts/main.lua", module);
        atomic_write(stage / ".faset/lua/faset.lua", lua_annotations);
        atomic_write_json(stage / ".luarc.json", lua_config);
        atomic_write_json(stage / "Scripts/.luarc.json", lua_scripts_config);
    }
    atomic_write_json(stage / "Scenes/main.scene.json", scene);
    Json project = {{"format", "faset.project"},
                    {"version", 1},
                    {"id", new_id()},
                    {"name", name},
                    {"dimension", dimension},
                    {"start_scene", "Scenes/main.scene.json"}};
    if (language == "lua")
        project["scripting"] = {{"lua", {{"scripts", Json::array({"Scripts/main.lua"})}}}};
    atomic_write_json(stage / "project.faset.json", project);
    atomic_write(stage / ".gitignore",
                 ".faset/*\n!.faset/lua/\n.faset/lua/*\n!.faset/lua/faset.lua\nExports/\n");
    std::vector<std::pair<fs::path, std::string>> created;
    auto publish = [&](const fs::path& relative) {
        const auto source = stage / relative;
        const auto target = c.project_root / relative;
        fs::create_directories(target.parent_path());
        if (!fs::copy_file(source, target, fs::copy_options::none))
            throw std::runtime_error("Starter destination appeared during creation: " +
                                     path_to_utf8(target));
        created.emplace_back(target, sha256_file(source));
    };
    try {
        publish(".gitignore");
        if (language == "cpp") {
            publish("Scripts/Gameplay.cpp");
            publish("Scripts/Gameplay.hpp");
        } else {
            publish("Scripts/main.lua");
            publish(".faset/lua/faset.lua");
            publish(".luarc.json");
            publish("Scripts/.luarc.json");
        }
        publish("Scenes/main.scene.json");
        fs::create_directory(c.project_root / "Assets");
        // The manifest is the final commit marker: an interrupted create has no
        // valid project record and never replaces an existing project file.
        publish("project.faset.json");
    } catch (...) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            try {
                std::error_code ignored;
                if (fs::is_regular_file(it->first, ignored) &&
                    sha256_file(it->first) == it->second)
                    fs::remove(it->first, ignored);
            } catch (...) { /* Preserve the original create failure. */
            }
        }
        for (const auto& relative : {"Assets", "Scenes", "Scripts", ".faset/lua"}) {
            std::error_code ignored;
            fs::remove(c.project_root / relative, ignored); // Empty directories only.
        }
        throw;
    }
}
std::string BuildService::start_build() {
    return impl_->enqueue("build");
}
std::string BuildService::start_cook(Json scene) {
    return impl_->enqueue("cook", std::move(scene));
}
std::string BuildService::start_export(Json scene, const fs::path& output) {
    if (output.empty())
        throw std::invalid_argument("Export output directory is required");
    return impl_->enqueue("export", std::move(scene), output);
}
JobStatus BuildService::job(const std::string& id) const {
    auto value = impl_->lookup(id);
    std::lock_guard lock(value->mutex);
    return value->status;
}
std::vector<JobStatus> BuildService::jobs() const {
    std::vector<std::shared_ptr<Impl::Job>> values;
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& [_, value] : impl_->jobs)
            values.push_back(value);
    }
    std::vector<JobStatus> result;
    for (auto& value : values) {
        std::lock_guard lock(value->mutex);
        result.push_back(value->status);
    }
    return result;
}
void BuildService::cancel(const std::string& id) {
    impl_->lookup(id)->cancelled = true;
    impl_->condition.notify_all();
}
JobStatus BuildService::wait(const std::string& id) {
    auto value = impl_->lookup(id);
    std::unique_lock lock(value->mutex);
    value->finished.wait(lock, [&] { return value->status.finished(); });
    return value->status;
}
} // namespace faset::editor
