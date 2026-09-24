#include "assets_image_fixtures.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/authoring/service.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <faset/editor/build_service.hpp>
#include <faset/scripting/project.hpp>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif
#ifdef __linux__
#include <cerrno>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace faset;
namespace fs = std::filesystem;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string collect(Process& process) {
    std::string text;
    while (true) {
        auto poll = process.poll();
        text += poll.output;
        if (!poll.running) {
            require(poll.exit_code == 0, "Child process failed");
            return text;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
Json scene(int dimension) {
    return {{"format", "faset.scene"},   {"version", 1},           {"id", "scene-test"},
            {"name", "Build test"},      {"dimension", dimension}, {"entities", Json::array()},
            {"instances", Json::array()}};
}
void starter_contracts(const fs::path& root) {
    for (const int dimension : {2, 3}) {
        for (const char* language : {"cpp", "lua"}) {
            const auto project = root / path_from_utf8(std::string("Starter café ") + language +
                                                        std::to_string(dimension));
            editor::BuildConfig config;
            config.project_root = project;
            config.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
            editor::BuildService service(config);
            service.scaffold("Starter", dimension, language);
            const auto manifest = read_json(project / "project.faset.json");
            const auto start = read_json(project / "Scenes/main.scene.json");
            require(manifest.at("dimension") == dimension &&
                        manifest.at("start_scene") == "Scenes/main.scene.json",
                    "Explicit starter has the requested scene type and startup path");
            require(start.at("dimension") == dimension && !start.at("entities").empty(),
                    "Starter scene is valid and has visible contents");
            auto builtins = start;
            bool has_behavior = false;
            for (auto& entity : builtins["entities"]) {
                auto& components = entity["components"].get_ref<Json::array_t&>();
                std::erase_if(components, [&](const Json& component) {
                    const auto custom =
                        !component.at("type").get<std::string>().starts_with("faset.");
                    has_behavior |= custom;
                    return custom;
                });
            }
            require(has_behavior, "Starter scene binds its gameplay behavior");
            authoring::validate_scene(builtins, authoring::builtin_schemas());
            if (std::string_view(language) == "lua") {
                require(!fs::exists(project / "Scripts/Gameplay.cpp") &&
                            !fs::exists(project / "Scripts/Gameplay.hpp"),
                        "Lua starter has no C++ gameplay stub");
                require(manifest.at("scripting").at("lua").at("scripts") ==
                            Json::array({"Scripts/main.lua"}) &&
                            scripting::loadLuaProject(project).enabled() &&
                            fs::is_regular_file(project / ".luarc.json") &&
                            fs::is_regular_file(project / "Scripts/.luarc.json") &&
                            fs::is_regular_file(project / ".faset/lua/faset.lua"),
                        "Lua starter declares an actual runnable module");
            } else {
                require(fs::is_regular_file(project / "Scripts/Gameplay.cpp") &&
                            fs::is_regular_file(project / "Scripts/Gameplay.hpp") &&
                            !manifest.contains("scripting"),
                        "C++ starter contains its compiled gameplay module");
            }
            const auto marker = read_text(project / "Scenes/main.scene.json");
            bool rejected = false;
            try {
                service.scaffold("Again", dimension, language);
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected && read_text(project / "Scenes/main.scene.json") == marker,
                    "Explicit creation rejects existing project without overwriting it");
        }
    }
    editor::BuildConfig legacy;
    legacy.project_root = root / "legacy";
    legacy.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
    editor::BuildService(legacy).scaffold("Legacy", 3);
    require(!fs::exists(legacy.project_root / "Scenes/main.scene.json"),
            "Two-argument scaffold retains its prior no-scene behavior");
    const auto concurrent_root = root / "concurrent";
    editor::BuildConfig concurrent;
    concurrent.project_root = concurrent_root;
    concurrent.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
    editor::BuildService first(concurrent), second(concurrent);
    std::atomic<int> ready = 0;
    std::atomic<int> successes = 0;
    auto attempt = [&](editor::BuildService& service, const char* name) {
        ++ready;
        while (ready.load() < 2)
            std::this_thread::yield();
        try {
            service.scaffold(name, 2, "lua");
            ++successes;
        } catch (const std::exception&) {
        }
    };
    std::thread a(attempt, std::ref(first), "First");
    std::thread b(attempt, std::ref(second), "Second");
    a.join();
    b.join();
    require(successes == 1, "Two concurrent creators publish exactly one starter");
    const auto winner = read_json(concurrent_root / "project.faset.json");
    require(winner.at("name") == "First" || winner.at("name") == "Second",
            "Published manifest belongs to the successful creator");
    require(fs::is_regular_file(concurrent_root / "Scripts/main.lua") &&
                fs::is_regular_file(concurrent_root / "Scenes/main.scene.json"),
            "The winning starter publishes all required files");
    const auto ignore = read_text(concurrent_root / ".gitignore");
    require(ignore.find("!.faset/lua/faset.lua") != std::string::npos,
            "LuaLS declarations are commit-friendly in generated projects");
}
void lua_project_contracts(const fs::path& root) {
    fs::create_directories(root);
    require(!scripting::loadLuaProject(root).enabled(), "No manifest means no Lua dependency");
    Json manifest{{"format", "faset.project"}, {"version", 1}};
    const auto save_manifest = [&] { atomic_write_json(root / "project.faset.json", manifest); };
    save_manifest();
    require(!scripting::loadLuaProject(root).enabled(), "C++ manifest requires no Lua sources");
    atomic_write(root / "Scripts/main.lua", "return {value = 1}\n");
    atomic_write(root / "Scripts/lib/util.lua", "return {answer = 42}\n");
    atomic_write(root / "Scripts/Gameplay.cpp", "// Not a Lua module\n");
    manifest["scripting"]["lua"]["scripts"] = Json::array({"Scripts/main.lua"});
    save_manifest();
    const auto original = scripting::loadLuaProject(root);
    require(original.enabled() && original.sources.size() == 2 && original.fingerprint.size() == 64,
            "Capture entry script and transitive module candidates, not C++");
    require(scripting::loadLuaProject(root).fingerprint == original.fingerprint,
            "Lua fingerprint is deterministic");
    atomic_write(root / "Scripts/lib/util.lua", "return {answer = 43}\n");
    require(scripting::loadLuaProject(root).fingerprint != original.fingerprint,
            "Changes to non-entry require modules invalidate the Lua fingerprint");
    const auto target = root.parent_path() / "lua-snapshot";
    scripting::writeLuaSources(original, target);
    atomic_write_json(target / "project.faset.json", manifest);
    require(scripting::loadLuaProject(target).fingerprint == original.fingerprint &&
                read_text(target / "Scripts/lib/util.lua") == "return {answer = 42}\n",
            "Publishing writes captured bytes rather than rereading live scripts");
    const auto valid_manifest = manifest;
    auto rejected = [&](const auto& operation) {
        try {
            operation();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    for (const auto& entries :
         {Json::array({"Scripts/main.lua", "Scripts/main.lua"}),
          Json::array({"Scripts/../main.lua"}), Json::array({"Scripts/missing.lua"}),
          Json::array({"Scripts/Gameplay.cpp"}), Json::array({"/Scripts/main.lua"}),
          Json::array({"Scripts\\main.lua"}), Json::array({"Scripts/./main.lua"}),
          Json::array({"Scripts//main.lua"}), Json::array({"C:/Scripts/main.lua"}),
          Json::array({12}), Json("Scripts/main.lua")}) {
        manifest["scripting"]["lua"]["scripts"] = entries;
        save_manifest();
        require(rejected([&] { (void)scripting::loadLuaProject(root); }),
                "Lua rejects malformed, duplicate, missing and escaping entry paths");
    }
    manifest = valid_manifest;
    manifest["scripting"]["lua"]["scripts"].push_back("Scripts/lib/util.lua");
    save_manifest();
    const auto with_second_entry = scripting::loadLuaProject(root).fingerprint;
    manifest["scripting"]["lua"]["scripts"] =
        Json::array({"Scripts/lib/util.lua", "Scripts/main.lua"});
    save_manifest();
    require(scripting::loadLuaProject(root).fingerprint != with_second_entry,
            "Entry order is part of the source fingerprint");
    manifest = valid_manifest;
    save_manifest();
    atomic_write(root / "Scripts/too-large.lua", std::string(1024 * 1024 + 1, ' '));
    require(rejected([&] { (void)scripting::loadLuaProject(root); }),
            "Source size is bounded before code execution");
    fs::remove(root / "Scripts/too-large.lua");
    auto corrupt = original;
    corrupt.sources["../outside.lua"] = "return {}";
    require(rejected([&] { scripting::writeLuaSources(corrupt, target); }),
            "Writing an externally supplied snapshot validates paths too");
    std::error_code symlink_error;
    fs::create_symlink(root / "Scripts/main.lua", root / "Scripts/link.lua", symlink_error);
    if (!symlink_error) {
        require(rejected([&] { (void)scripting::loadLuaProject(root); }),
                "Lua source snapshots reject even in-project symlink aliases");
        fs::remove(root / "Scripts/link.lua");
        fs::create_directory_symlink(root / "Scripts", target / "linked", symlink_error);
        if (!symlink_error)
            require(rejected([&] { scripting::writeLuaSources(original, target / "linked"); }),
                    "Snapshot destination root cannot be a symlink");
    }
    manifest["scripting"]["lua"]["scripts"] = Json::array();
    save_manifest();
    require(!scripting::loadLuaProject(root).enabled(), "Empty Lua declaration links no Lua VM");
}
int integration(const fs::path& root) {
    fs::create_directories(root);
    editor::BuildConfig config;
    config.project_root = root / "project";
    config.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
    config.build_directory = root / "native-build";
    config.cache_root = root / "project" / ".faset" / "cache";
    editor::BuildService service(config);
    service.scaffold("Export integration", 3);
    // This dedicated integration fixture is reset before testing incremental user edits.
    atomic_write(config.project_root / "Scripts" / "Gameplay.cpp",
                 read_text(config.engine_root / "tools" / "project_templates" / "Gameplay.cpp"));
    auto wait = [&](const std::string& id) {
        std::string stage;
        while (true) {
            auto job = service.job(id);
            if (job.stage != stage) {
                stage = job.stage;
                std::cout << job.stage << std::endl;
            }
            if (job.finished()) {
                if (job.state != "succeeded") {
                    std::cerr << job.error << '\n' << job.log;
                    throw std::runtime_error("Integration job failed");
                }
                return job;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    const auto assets = config.project_root / "Assets";
    std::string binary;
    for (float value : {-1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 2.f, 0.f}) {
        auto bits = std::bit_cast<std::uint32_t>(value);
        for (int i = 0; i < 4; ++i)
            binary.push_back(static_cast<char>(bits >> (8 * i)));
    }
    atomic_write(assets / "triangle.bin", binary);
    Json gltf = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", {0}}}})},
        {"nodes", Json::array({{{"mesh", 0}, {"extras", {{"faset_id", "triangle-node"}}}}})},
        {"buffers", Json::array({{{"uri", "triangle.bin"}, {"byteLength", 36}}})},
        {"bufferViews", Json::array({{{"buffer", 0}, {"byteLength", 36}}})},
        {"accessors", Json::array({{{"bufferView", 0},
                                    {"componentType", 5126},
                                    {"count", 3},
                                    {"type", "VEC3"},
                                    {"min", {-1, 0, 0}},
                                    {"max", {1, 2, 0}}}})},
        {"meshes",
         Json::array({{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}}}}})}}})}};
    atomic_write_json(assets / "triangle.gltf", gltf);
    assets::AssetPipeline importer(config.cache_root);
    auto imported = importer.import_asset({assets / "triangle.gltf"});
    require(imported.ok(), "Integration asset import");
    for (int dimension : {2, 3}) {
        auto document = scene(dimension);
        auto component = [](std::string type, Json fields) {
            return Json{
                {"id", type}, {"type", type}, {"version", 1}, {"fields", std::move(fields)}};
        };
        Json components = Json::array(
            {component("faset.transform",
                       {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}})});
        if (dimension == 2)
            components.push_back(
                component("faset.sprite", {{"size", {2, 2}}, {"color", {.2, .7, .9, 1}}}));
        else
            components.push_back(component(
                "faset.mesh", {{"asset", imported.asset_id + "#" +
                                             importer.load_asset(imported.asset_id).nodes.at(0).id},
                               {"color", {.3, .8, .5, 1}}}));
        document["entities"].push_back({{"id", "object"},
                                        {"name", "Object"},
                                        {"parent", nullptr},
                                        {"components", components}});
        auto result =
            wait(service.start_export(document, root / ("export-" + std::to_string(dimension))));
        const auto directory = path_from_utf8(result.result.at("directory").get<std::string>());
        require(result.result.at("configuration") == "Release",
                "Exports default to the Release profile");
        require(path_from_utf8(result.result.at("build_directory").get<std::string>()) ==
                    config.build_directory / "Release",
                "Export has a separate CMake directory");
        require(read_json(directory / "manifest.json").at("configuration") == "Release",
                "Export manifest records the actual profile");
        Process player(
            {{result.result.at("executable").get<std::string>(), "--headless", "--frames", "3",
              "--capture", path_to_utf8(directory / "verification.ppm")},
             directory,
             {}});
        std::cout << collect(player);
        require(fs::file_size(directory / "verification.ppm") > 1000,
                "Exported game rendered a frame");
        atomic_write_json(root / ("result-" + std::to_string(dimension) + ".json"), result.result);
    }
    auto source = read_text(config.project_root / "Scripts" / "Gameplay.cpp");
    auto position = source.find("Character");
    require(position != std::string::npos, "Template schema fixture");
    source.replace(position, 9, "Custom Character");
    atomic_write(config.project_root / "Scripts" / "Gameplay.cpp", source);
    auto release_cache = read_text(config.build_directory / "Release" / "CMakeCache.txt");
    auto rebuilt = wait(service.start_build());
    require(rebuilt.result.at("configuration") == "Debug", "Development builds remain Debug");
    require(path_from_utf8(rebuilt.result.at("build_directory").get<std::string>()) ==
                config.build_directory / "Debug",
            "Development CMake directory is isolated");
    require(read_text(config.build_directory / "Release" / "CMakeCache.txt") == release_cache,
            "Development build preserves the Release cache");
    auto schema = read_json(path_from_utf8(rebuilt.result.at("schema").get<std::string>()));
    bool updated{};
    for (const auto& type : schema.at("types"))
        if (type.value("name", "") == "Custom Character")
            updated = true;
    require(updated, "Incremental C++ build produced new schema");
    auto last = read_text(config.cache_root / "last_build.json");
    atomic_write(config.project_root / "Scripts" / "Gameplay.cpp",
                 source + "\n#error intentional_build_failure\n");
    auto failed = service.wait(service.start_build());
    require(failed.state == "failed", "Invalid user C++ must fail build");
    bool navigable_cpp_error = false;
    for (const auto& diagnostic : failed.diagnostics)
        navigable_cpp_error |= diagnostic.value("severity", "") == "error" &&
                               diagnostic.value("file", "") == "Scripts/Gameplay.cpp" &&
                               diagnostic.value("line", 0) > 0;
    require(navigable_cpp_error,
            "Real staged C++ compile failure maps to a navigable project source");
    require(read_text(config.cache_root / "last_build.json") == last,
            "Failed compile preserved last good build");
    atomic_write(config.project_root / "Scripts" / "Gameplay.cpp", source);
    std::cout << "Real 2D/3D exports, imported mesh packaging, native launches, incremental C++ "
                 "schema and failed-build recovery passed\n";
    return 0;
}
#ifdef __linux__
void descendant_cleanup(const fs::path& executable, const fs::path& directory) {
    struct Subreaper {
        int previous{};
        Subreaper() {
            require(::prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0 &&
                        ::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0,
                    "Install test-only descendant reaper");
        }
        ~Subreaper() {
            ::prctl(PR_SET_CHILD_SUBREAPER, previous);
        }
    } subreaper;
    for (bool cancel : {false, true}) {
        Process process(
            {{path_to_utf8(executable), cancel ? "--tree-sleep" : "--tree-exit"}, directory, {}});
        std::string output;
        bool stopped{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            auto state = process.poll();
            output += state.output;
            if (cancel && output.find("leader-tail\n") != std::string::npos) {
                process.cancel();
                state = process.poll();
                output += state.output;
            }
            if (!state.running) {
                require(cancel || state.exit_code == 0, "Leader exit status remains intact");
                stopped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(stopped && output.find("leader-tail\n") != std::string::npos,
                "Leader exit and buffered stdout tail are observed");
        const auto descendant = static_cast<pid_t>(std::stol(output));
        int status{};
        pid_t reaped{};
        const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            reaped = ::waitpid(descendant, &status, WNOHANG);
            if (reaped != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < reap_deadline);
        const bool cleaned =
            reaped == descendant && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        if (reaped == 0) {
            // Clean up the still-owned child even when this regression fails.
            ::kill(descendant, SIGKILL);
            while (::waitpid(descendant, &status, 0) < 0 && errno == EINTR) {
            }
        }
        require(cleaned, "Leader completion terminates its SIGTERM-resistant descendant");
        process.cancel(); // Completed ownership must not signal any stale process-group ID.
    }
}
#endif
int test_main(int argc, char** argv) {
#ifdef __linux__
    if (argc > 1 &&
        (std::string(argv[1]) == "--tree-exit" || std::string(argv[1]) == "--tree-sleep")) {
        int ready[2];
        if (::pipe(ready) != 0)
            return 2;
        const auto descendant = ::fork();
        if (descendant < 0)
            return 3;
        if (descendant == 0) {
            ::close(ready[0]);
            std::signal(SIGTERM, SIG_IGN);
            const char byte = 'r';
            if (::write(ready[1], &byte, 1) != 1)
                ::_exit(4);
            ::close(ready[1]);
            while (true)
                ::pause();
        }
        ::close(ready[1]);
        char byte{};
        const auto count = ::read(ready[0], &byte, 1);
        ::close(ready[0]);
        if (count != 1)
            return 5;
        std::cout << descendant << "\nleader-tail\n" << std::flush;
        if (std::string(argv[1]) == "--tree-sleep")
            while (true)
                ::pause();
        return 0;
    }
#endif
    if (argc > 1 && std::string(argv[1]) == "--child") {
        Json args = Json::array();
        for (int i = 2; i < argc; ++i)
            args.push_back(argv[i]);
        std::cout << Json{{"args", args},
                          {"cwd", path_to_utf8(fs::current_path())},
                          {"env", std::getenv("FASET_PROCESS_TEST")
                                      ? std::getenv("FASET_PROCESS_TEST")
                                      : ""}}
                         .dump()
                  << std::endl;
        std::cerr << "stderr-sentinel\n";
        std::cout << std::string(100000, 'x') << std::endl;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--sleep") {
#ifndef _WIN32
        std::signal(SIGTERM, SIG_IGN);
#endif
        std::cout << "ready\n" << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--integration") {
        try {
            return integration(fs::absolute(path_from_utf8(argv[2])));
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    fs::path temporary = fs::temp_directory_path() / path_from_utf8("Faset Café 世界 " + new_id());
    try {
        fs::create_directories(temporary);
        lua_project_contracts(temporary / "lua-project");
        starter_contracts(temporary / "starters");
        const auto original_executable = fs::absolute(path_from_utf8(argv[0]));
        const auto executable = temporary / original_executable.filename();
        fs::copy_file(original_executable, executable);
        Process child({{path_to_utf8(executable), "--child", "space argument", "quote\"backslash\\",
                        "$(touch not-executed); & |", "", "Café 世界 Привет 😀"},
                       temporary,
                       {{"FASET_PROCESS_TEST", "value with spaces"}}});
        auto text = collect(child);
        auto result = Json::parse(text.substr(0, text.find('\n')));
        require(result["args"] ==
                    Json::array({"space argument", "quote\"backslash\\",
                                 "$(touch not-executed); & |", "", "Café 世界 Привет 😀"}),
                "Arguments must remain literal");
        require(result["env"] == "value with spaces", "Child environment override");
        require(fs::equivalent(path_from_utf8(result["cwd"].get<std::string>()), temporary),
                "Child working directory");
        require(text.find("stderr-sentinel") != std::string::npos && text.size() > 100000,
                "Combined pipe output drained fully");
        require(!fs::exists(temporary / "not-executed"), "No shell execution");
        Process sleeper({{path_to_utf8(executable), "--sleep"}, temporary, {}});
        bool ready{};
        while (!ready) {
            auto p = sleeper.poll();
            ready = p.output.find("ready") != std::string::npos;
            require(p.running, "Sleeper exited early");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto start = std::chrono::steady_clock::now();
        sleeper.cancel();
        require(!sleeper.poll().running, "Cancellation must reap the process");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
                "Cancellation must finish promptly");
#ifdef __linux__
        descendant_cleanup(executable, temporary);
#endif
        editor::BuildConfig config;
        config.project_root = temporary / "project";
        config.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
        editor::BuildService service(config);
        service.scaffold("Test project", 2);
        atomic_write(config.project_root / "Scripts" / "Gameplay.cpp", "// User code\n");
        service.scaffold("Another name", 3);
        require(read_text(config.project_root / "Scripts" / "Gameplay.cpp") == "// User code\n",
                "Scaffold preserves existing source");
        auto id = service.start_cook(scene(2));
        auto cooked = service.wait(id);
        require(cooked.state == "succeeded", "Cook job succeeds");
        auto bytes = read_text(path_from_utf8(cooked.result.at("scene").get<std::string>()));
        require(bytes.substr(0, 8) == "FASETSCN" && bytes.size() > 20, "Cooked envelope magic");
        std::uint32_t version{};
        std::uint64_t size{};
        for (unsigned i = 0; i < 4; ++i)
            version |= std::uint32_t(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
        for (unsigned i = 0; i < 8; ++i)
            size |= std::uint64_t(static_cast<unsigned char>(bytes[12 + i])) << (8 * i);
        require(version == 1 && size == bytes.size() - 20, "Cooked envelope version and size");
        require(Json::from_cbor(bytes.begin() + 20, bytes.end()) == scene(2),
                "Cooked scene preserves all values");
        auto previous = read_text(service.config().cache_root / "last_cook.json");
        auto broken = scene(3);
        broken["version"] = 999;
        auto bad = service.wait(service.start_cook(broken));
        require(bad.state == "failed", "Unsupported scene version rejected");
        require(read_text(service.config().cache_root / "last_cook.json") == previous,
                "Failed cook preserves last good generation");
        broken = scene(3);
        broken["entities"].push_back(
            {{"id", "entity"},
             {"components",
              Json::array({{{"type", "faset.mesh"}, {"fields", {{"asset", "missing-asset"}}}}})}});
        auto missing = service.wait(service.start_cook(broken));
        require(missing.state == "failed", "Missing asset blocks publication");
        require(read_text(service.config().cache_root / "last_cook.json") == previous,
                "Missing asset preserves last good generation");
        broken = scene(3);
        broken["entities"].push_back({{"id", "entity"},
                                      {"components", Json::array({{{"type", "faset.unknown"},
                                                                   {"version", 1},
                                                                   {"fields", Json::object()}}})}});
        auto unknown = service.wait(service.start_cook(broken));
        require(unknown.state == "failed" &&
                    unknown.error.find("unresolved component") != std::string::npos,
                "Unknown component type blocks cooking");
        require(read_text(service.config().cache_root / "last_cook.json") == previous,
                "Unknown schema preserves last good generation");
        const auto picture = config.project_root / "Assets" / path_from_utf8("Freshness Café.png");
        auto write_picture = [&](const auto& bytes) {
            atomic_write(picture,
                         std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        };
        write_picture(test_images::png_red_green);
        assets::AssetPipeline importer(service.config().cache_root);
        const auto imported = importer.import_asset({picture});
        require(imported.ok(), "Freshness fixture imports a real image");
        auto textured = scene(2);
        textured["entities"].push_back(
            {{"id", "sprite"},
             {"components", Json::array({{{"type", "faset.sprite"},
                                          {"version", 1},
                                          {"fields", {{"texture", imported.asset_id}}}}})}});
        require(service.wait(service.start_cook(textured)).state == "succeeded",
                "Current referenced assets can be cooked");
        const auto fresh_pointer = read_text(service.config().cache_root / "last_cook.json");
        write_picture(test_images::png_blue_white);
        for (const auto& stale :
             {service.wait(service.start_cook(textured)),
              service.wait(service.start_export(textured, temporary / "export"))}) {
            require(stale.state == "failed" && stale.error.find("Reimport") != std::string::npos,
                    "Stale source blocks cook and export before compilation/publication");
            require(read_text(service.config().cache_root / "last_cook.json") == fresh_pointer,
                    "Stale source preserves the published cook pointer");
        }
        require(!fs::exists(temporary / "export" / "current.json"),
                "Stale export never publishes a game pointer");
        require(importer.import_asset({picture}).ok() &&
                    service.wait(service.start_cook(textured)).state == "succeeded",
                "Explicit reimport permits cooking the refreshed generation");
        fs::remove(picture);
        require(service.wait(service.start_cook(textured)).state == "failed",
                "Unavailable source cannot silently cook old cached content");
        std::cout << "Literal process arguments, pipes, cancellation, scaffold and atomic cook "
                     "contracts passed\n";
        fs::remove_all(temporary);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, test_main);
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
