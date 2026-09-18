#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <faset/assets/asset_data.hpp>
#include <faset/authoring/schema.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <faset/editor/build_service.hpp>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

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
} // namespace
Json JobStatus::json() const {
    return {{"id", id},       {"kind", kind},         {"state", state},
            {"stage", stage}, {"progress", progress}, {"log", log},
            {"error", error}, {"result", result}};
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
    std::string run(Job& job, std::vector<std::string> arguments, const fs::path& cwd) {
        if (job.cancelled)
            throw Cancelled{};
        std::string description = "$";
        for (const auto& argument : arguments)
            description += " " + Json(argument).dump();
        description += '\n';
        log(job, description);
        Process process({std::move(arguments), cwd, {}});
        std::string output;
        while (true) {
            if (job.cancelled) {
                process.cancel();
                auto final = process.poll();
                log(job, final.output);
                throw Cancelled{};
            }
            auto poll = process.poll();
            log(job, poll.output);
            output += poll.output;
            if (output.size() > max_log_bytes)
                output.erase(0, output.size() - max_log_bytes);
            if (!poll.running) {
                if (poll.exit_code.value_or(1) != 0)
                    throw std::runtime_error("Process exited with code " +
                                             std::to_string(poll.exit_code.value_or(1)) +
                                             "; see job log");
                return output;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    void validate_assets(const Json& scene) {
        assets::AssetStore pipeline(config.cache_root);
        for (const auto& id : asset_references(scene))
            pipeline.load_asset(id);
    }
    Json build(Job& job, bool exporting = false) {
        const auto& configuration = exporting ? config.export_configuration : config.configuration;
        const auto native_directory = config.build_directory / configuration;
        checkpoint(job, "Configuring C++ gameplay", .05);
        if (!fs::is_regular_file(config.project_root / "Scripts" / "Gameplay.cpp") ||
            !fs::is_regular_file(config.project_root / "Scripts" / "Gameplay.hpp"))
            throw std::runtime_error("Project Scripts/Gameplay.cpp and Gameplay.hpp are required; "
                                     "create a project scaffold first");
        fs::create_directories(native_directory);
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
                                                  path_to_utf8(config.project_root / "Scripts")};
        bool compiler_overridden = false;
        for (const auto& arg : config.configure_arguments)
            if (arg.starts_with("-DCMAKE_CXX_COMPILER="))
                compiler_overridden = true;
        if (!compiler_overridden) {
#ifdef _WIN32
            auto compiler = find_executable("clang-cl");
            arguments.push_back("-DCMAKE_C_COMPILER=" + path_to_utf8(compiler));
            arguments.push_back("-DCMAKE_CXX_COMPILER=" + path_to_utf8(compiler));
#else
            arguments.push_back("-DCMAKE_C_COMPILER=" + path_to_utf8(find_executable("clang")));
            arguments.push_back("-DCMAKE_CXX_COMPILER=" + path_to_utf8(find_executable("clang++")));
#endif
        }
        arguments.insert(arguments.end(), config.configure_arguments.begin(),
                         config.configure_arguments.end());
        arguments.push_back("-DCMAKE_BUILD_TYPE=" + configuration);
        run(job, std::move(arguments), config.project_root);
        checkpoint(job, "Compiling and linking Player", .25);
        run(job,
            {config.cmake, "--build", path_to_utf8(native_directory), "--config", configuration,
             "--parallel", "4", "--target", "faset_player", "faset_schema_exporter"},
            config.project_root);
        checkpoint(job, "Exporting gameplay schema", .58);
        auto player = build_executable(native_directory, configuration, "faset_player");
        auto exporter = build_executable(native_directory, configuration, "faset_schema_exporter");
        const auto staging = config.cache_root / "builds" / (".staging-" + job.status.id);
        const auto generation = config.cache_root / "builds" / job.status.id;
        fs::create_directories(staging);
        try {
            const auto schema_file = staging / "schema.json";
            run(job, {path_to_utf8(exporter), "--output", path_to_utf8(schema_file)},
                config.project_root);
            auto schema = read_json(schema_file);
            if (schema.value("format", "") != "faset.schema" || schema.value("version", 0) != 1 ||
                !schema.contains("types") || !schema.at("types").is_array())
                throw std::runtime_error("SchemaExporter returned an invalid manifest");
            // Validate the complete metadata before publishing either the schema or
            // its Player generation. Session uses this same authoring contract.
            (void)authoring::gameplay_schemas(schema);
            std::string fingerprint = sha256_file(player) + sha256_file(exporter) +
                                      read_text(native_directory / "CMakeCache.txt");
            for (const auto& file : {"Gameplay.cpp", "Gameplay.hpp"})
                fingerprint += read_text(config.project_root / "Scripts" / file);
            fingerprint = sha256(fingerprint);
            schema["build_fingerprint"] = fingerprint;
            atomic_write_json(schema_file, schema);
            copy_required_file(player, staging / ("faset_player" + executable_suffix()));
            copy_required_file(exporter, staging / ("faset_schema_exporter" + executable_suffix()));
            for (const auto* file : {"vertexMain.spv", "fragmentMain.spv", "shadowMain.spv",
                                     "vertexMain.reflection.json", "fragmentMain.reflection.json",
                                     "shadowMain.reflection.json"})
                copy_required_file(native_directory / "shaders" / file, staging / "shaders" / file);
            copy_runtime_libraries(job, player, staging, native_directory, configuration);
            Json manifest{{"format", "faset.build"},
                          {"version", 1},
                          {"id", job.status.id},
                          {"fingerprint", fingerprint},
                          {"configuration", configuration},
                          {"player", "faset_player" + executable_suffix()},
                          {"schema", "schema.json"}};
            atomic_write_json(staging / "manifest.json", manifest);
            checkpoint(job, "Publishing build generation", .68);
            fs::rename(staging, generation);
            atomic_write_json(config.cache_root / "last_build.json",
                              {{"generation", job.status.id}, {"fingerprint", fingerprint}});
            return {{"generation", job.status.id},
                    {"directory", path_to_utf8(generation)},
                    {"build_directory", path_to_utf8(native_directory)},
                    {"configuration", configuration},
                    {"player", path_to_utf8(generation / ("faset_player" + executable_suffix()))},
                    {"schema", path_to_utf8(generation / "schema.json")},
                    {"fingerprint", fingerprint}};
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
    void package_notices(const fs::path& destination, const fs::path& native_directory) {
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
            copy_required_file(build_directory / ("faset_player" + executable_suffix()),
                               staging / ("faset_player" + executable_suffix()));
            for (const auto* shader : {"vertexMain.spv", "fragmentMain.spv", "shadowMain.spv",
                                       "vertexMain.reflection.json", "fragmentMain.reflection.json",
                                       "shadowMain.reflection.json"})
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
                            path_from_utf8(built.at("build_directory").get<std::string>()));
            atomic_write(
                staging / "README.txt",
                "Run faset_player" + executable_suffix() +
                    " to start this game.\nThe executable loads scene.fscene and assets beside "
                    "it.\nKeep shaders/, assets/, and Notices/ with the executable.\nA compatible "
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
