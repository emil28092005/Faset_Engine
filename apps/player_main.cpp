#include "Gameplay.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/player/SceneView.hpp>
#include <faset/runtime/Runtime.hpp>
#include <faset/runtime/schema.hpp>
#include <faset/scripting/project.hpp>
#if defined(FASET_HAS_LUA)
#include <faset/scripting/LuaModule.hpp>
#endif
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
struct ProfileSample {
    double wall{}, simulation{}, snapshot{}, render{}, rendererCpu{}, gpu{}, readbackCpu{};
    faset::runtime::FrameStats runtime;
    std::uint32_t draws{}, vertices{};
    std::uint64_t gpuAllocatedBytes{};
    std::uint32_t textureCount{};
    bool physicsDebug{};
    bool gpuVisibilityActive{};
};
Json distribution(std::vector<double> values) {
    if (values.empty())
        return nullptr;
    std::sort(values.begin(), values.end());
    auto percentile = [&](double fraction) {
        return values[static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1];
    };
    return {{"samples", values.size()},
            {"min", values.front()},
            {"p50", percentile(.5)},
            {"p95", percentile(.95)},
            {"max", values.back()}};
}
Json profileFrames(const std::vector<ProfileSample>& samples) {
    Json frames = Json::array();
    std::vector<double> wall, simulation, snapshot, render, rendererCpu, gpu, readbackCpu;
    for (const auto& sample : samples) {
        wall.push_back(sample.wall);
        simulation.push_back(sample.simulation);
        snapshot.push_back(sample.snapshot);
        render.push_back(sample.render);
        rendererCpu.push_back(sample.rendererCpu);
        readbackCpu.push_back(sample.readbackCpu);
        // The backend reports zero if timestamp queries are unavailable. Do not
        // present that sentinel as a measured zero-cost GPU frame.
        const bool gpuMeasured = std::isfinite(sample.gpu) && sample.gpu > 0;
        if (gpuMeasured)
            gpu.push_back(sample.gpu);
        frames.push_back({{"frame", frames.size() + 1},
                          {"wall_ms", sample.wall},
                          {"simulation_ms", sample.simulation},
                          {"snapshot_ms", sample.snapshot},
                          {"render_call_ms", sample.render},
                          {"renderer_cpu_ms", sample.rendererCpu},
                          {"renderer_readback_cpu_ms", sample.readbackCpu},
                          {"gpu_ms", gpuMeasured ? Json(sample.gpu) : Json(nullptr)},
                          {"fixed_ticks", sample.runtime.fixedTicks},
                          {"tick", sample.runtime.tick},
                          {"dropped_simulation_seconds", sample.runtime.droppedTime},
                          {"interpolation_alpha", sample.runtime.interpolationAlpha},
                          {"draw_calls", sample.draws},
                          {"vertices", sample.vertices},
                          {"gpu_allocated_bytes", sample.gpuAllocatedBytes},
                          {"texture_count", sample.textureCount},
                          {"physics_debug", sample.physicsDebug},
                          {"gpu_visibility_active", sample.gpuVisibilityActive}});
    }
    return {{"samples", std::move(frames)},
            {"summary_ms",
             {{"wall", distribution(std::move(wall))},
              {"simulation", distribution(std::move(simulation))},
              {"snapshot", distribution(std::move(snapshot))},
              {"render_call", distribution(std::move(render))},
              {"renderer_cpu", distribution(std::move(rendererCpu))},
              {"renderer_readback_cpu", distribution(std::move(readbackCpu))},
              {"gpu", distribution(std::move(gpu))}}}};
}
Json physicsScene(const faset::runtime::Runtime& world, const Json& presentation) {
    const int dimension = presentation.at("dimension");
    const std::string bodyName = dimension == 2 ? "rigid_body_2d" : "rigid_body_3d";
    Json result{{"dimension", dimension}, {"entities", Json::array()}};
    for (const auto& entity : presentation.at("entities")) {
        const auto handle = world.find(entity.at("id").get<std::string>());
        Json body;
        try {
            body = world.fields(handle, "faset." + bodyName);
        } catch (const std::invalid_argument&) {
            continue; // Render-only entities do not own a physics component.
        }
        const auto pose = world.transform(handle); // Current physics pose, not interpolation.
        result["entities"].push_back(
            {{"parent", nullptr},
             {"transform",
              {{"position", pose.position}, {"rotation", pose.rotation}, {"scale", pose.scale}}},
             {bodyName, std::move(body)}});
    }
    return result;
}
std::filesystem::path executableDirectory(const char* argument) {
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        throw std::runtime_error("Cannot locate Player executable");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::absolute(argument).parent_path() : executable.parent_path();
#endif
}
std::uint64_t count(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Frame count must be a positive integer");
    auto result = std::stoull(value);
    if (result == 0 || result > 10000000)
        throw std::invalid_argument("Frame count out of range");
    return result;
}
faset::runtime::RuntimeConfig simulationConfig(const nlohmann::json& scene) {
    faset::runtime::RuntimeConfig config;
    if (!scene.contains("simulation"))
        return config;
    const auto& settings = scene.at("simulation");
    if (!settings.is_object())
        throw std::invalid_argument("simulation must be an object");
    config.fixedDelta = settings.value("fixed_delta", config.fixedDelta);
    auto boundedInteger = [&](const char* key, int fallback, int maximum) {
        if (!settings.contains(key))
            return fallback;
        const auto& value = settings.at(key);
        if (!value.is_number_integer())
            throw std::invalid_argument(std::string(key) + " must be an integer");
        // Check before narrowing, so very large unsigned values cannot wrap into
        // a valid configuration on a platform with a narrower unsigned type.
        const auto numeric = value.get<double>();
        if (numeric < 1 || numeric > maximum)
            throw std::invalid_argument(std::string(key) + " is out of range");
        return value.get<int>();
    };
    config.maxCatchUpTicks = static_cast<unsigned>(
        boundedInteger("max_catch_up_ticks", static_cast<int>(config.maxCatchUpTicks), 1024));
    config.physicsSubsteps = boundedInteger("physics_substeps", config.physicsSubsteps, 128);
    if (settings.contains("gravity")) {
        const auto& gravity = settings.at("gravity");
        if (!gravity.is_array() || gravity.size() != 3)
            throw std::invalid_argument("gravity must contain three numbers");
        config.gravity = gravity.get<faset::runtime::Vec3>();
    }
    return config;
}
void validatePackagedShaders(const std::filesystem::path& directory,
                             faset::render::VisibilityMode visibilityMode) {
    const auto shaders = directory / "shaders";
    for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain"})
        for (const auto* extension : {".spv", ".reflection.json"}) {
            const auto path = shaders / (std::string(entry) + extension);
            if (!std::filesystem::is_regular_file(path))
                throw std::runtime_error("Packaged shader file is missing: " +
                                         faset::path_to_utf8(path));
        }
    faset::render::validate_shader_bundle(shaders);
    if (visibilityMode != faset::render::VisibilityMode::Direct)
        faset::render::validate_gpu_shader_bundle(shaders);
}
} // namespace
int player_main(int argc, char** argv) {
    const auto started = Clock::now();
    try {
        std::filesystem::path scenePath, assetsPath, capturePath, controlPath, profilePath,
            projectRoot;
        bool headless = false, validateOnly = false, debugPhysics = false, watchLua = false;
        auto visibilityMode = faset::render::VisibilityMode::Direct;
        std::string visibilityName = "direct";
        std::uint64_t maximumFrames = 0;
        std::set<std::string> options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout
                    << "faset_player [--scene PATH] [--assets CACHE] [--frames N] "
                       "[--headless] [--capture PATH.ppm] [--validate] [--control PATH] "
                       "[--profile PATH.json] [--debug-physics] [--project ROOT] "
                       "[--watch-lua] [--visibility direct|gpu-frustum|gpu-occlusion]\n"
                       "No --scene: open scene.fscene beside the executable. CACHE contains "
                       "assets/<id>/.\n"
                       "Headless uses offscreen Vulkan; --frames uses the configured fixed "
                       "simulation delta.\n"
                       "--validate checks scene/resources on CPU without gameplay callbacks "
                       "or Vulkan initialization.\n"
                       "--control is an optional editor mailbox for pause/resume/step/stop, "
                       "without world queries.\n"
                       "--project loads Lua declared in project.faset.json; packaged projects "
                       "are discovered beside the scene or executable.\n"
                       "--watch-lua enables development-only script reload (or control "
                       "reload-lua): the scene restarts, runtime state is not preserved.\n"
                       "--profile requires explicit --frames 1..100000; measured durations "
                       "include the first frame and renderer GPU waits/readback.\n"
                       "--visibility selects the renderer for this Player run; Direct is "
                       "the default. GPU modes require their packaged shader bundle and "
                       "device capabilities.\n"
                       "Keys: A/D horizontal, W/S vertical, Space jump, E interact, P pause, "
                       "N single-step, F3 physics boxes, Escape quit.\n";
                return 0;
            }
            if (!options.insert(arg).second)
                throw std::invalid_argument("Repeated option: " + arg);
            auto value = [&]() -> std::string {
                if (i + 1 >= argc)
                    throw std::invalid_argument("Missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--scene")
                scenePath = faset::path_from_utf8(value());
            else if (arg == "--assets")
                assetsPath = faset::path_from_utf8(value());
            else if (arg == "--capture")
                capturePath = faset::path_from_utf8(value());
            else if (arg == "--control")
                controlPath = faset::path_from_utf8(value());
            else if (arg == "--profile")
                profilePath = faset::path_from_utf8(value());
            else if (arg == "--project")
                projectRoot = faset::path_from_utf8(value());
            else if (arg == "--visibility") {
                visibilityName = value();
                if (visibilityName == "direct")
                    visibilityMode = faset::render::VisibilityMode::Direct;
                else if (visibilityName == "gpu-frustum")
                    visibilityMode = faset::render::VisibilityMode::GpuFrustum;
                else if (visibilityName == "gpu-occlusion")
                    visibilityMode = faset::render::VisibilityMode::GpuOcclusion;
                else
                    throw std::invalid_argument("--visibility must be direct, gpu-frustum, "
                                                "or gpu-occlusion");
            } else if (arg == "--frames")
                maximumFrames = count(value());
            else if (arg == "--headless")
                headless = true;
            else if (arg == "--validate")
                validateOnly = true;
            else if (arg == "--debug-physics")
                debugPhysics = true;
            else if (arg == "--watch-lua")
                watchLua = true;
            else
                throw std::invalid_argument("Unknown option: " + arg);
        }
        if (options.contains("--profile") &&
            (profilePath.empty() || !options.contains("--frames") || maximumFrames > 100000 ||
             validateOnly))
            throw std::invalid_argument("--profile requires an output path and explicit --frames "
                                        "1..100000, without --validate");
        if (options.contains("--project") && projectRoot.empty())
            throw std::invalid_argument("--project requires a nonempty root path");
        if (scenePath.empty())
            scenePath = executableDirectory(argv[0]) / "scene.fscene";
        scenePath = std::filesystem::absolute(scenePath).lexically_normal();
        const auto executableRoot = executableDirectory(argv[0]);
        if (projectRoot.empty()) {
            if (std::filesystem::is_regular_file(scenePath.parent_path() / "project.faset.json"))
                projectRoot = scenePath.parent_path();
            else if (std::filesystem::is_regular_file(executableRoot / "project.faset.json"))
                projectRoot = executableRoot;
        }
        if (!projectRoot.empty())
            projectRoot = std::filesystem::absolute(projectRoot).lexically_normal();
        if (watchLua && (projectRoot.empty() || validateOnly))
            throw std::invalid_argument("--watch-lua requires a project, without --validate");
        if (assetsPath.empty())
            assetsPath = scenePath.parent_path();
        if (!std::filesystem::is_directory(assetsPath))
            throw std::invalid_argument("Asset cache directory does not exist: " +
                                        faset::path_to_utf8(assetsPath));
        if (headless && maximumFrames == 0)
            maximumFrames = 1;
        const auto sceneReadStarted = Clock::now();
        const auto document = faset::player::readScene(scenePath);
        const auto nativeSchema = faset::gameplay::schema();
        if (!nativeSchema.is_array())
            throw std::runtime_error("Gameplay schema() must return a type array");
        const auto luaProject = projectRoot.empty() ? faset::scripting::LuaProject{}
                                                    : faset::scripting::loadLuaProject(projectRoot);
        auto schema = nativeSchema;
#if defined(FASET_HAS_LUA)
        std::unique_ptr<faset::scripting::LuaModule> lua;
        if (luaProject.enabled()) {
            lua = std::make_unique<faset::scripting::LuaModule>(luaProject);
            for (const auto& type : lua->schema())
                schema.push_back(type);
        }
#else
        if (luaProject.enabled() || watchLua)
            throw std::runtime_error("This Player was built without Lua support; configure "
                                     "FASET_ENABLE_LUA=ON for this project");
#endif
        faset::runtime::validate_scene_schemas(document, schema);
#if defined(FASET_HAS_LUA)
        if (lua)
            lua->validateScene(document);
#endif
        const auto config = simulationConfig(document);
        const auto sceneReadFinished = Clock::now();
        if (scenePath.extension() == ".fscene" &&
            std::filesystem::equivalent(scenePath.parent_path(), executableRoot))
            validatePackagedShaders(executableRoot, visibilityMode);
        if (validateOnly) {
            if (!capturePath.empty() || !controlPath.empty())
                throw std::invalid_argument("--validate cannot capture or control a running game");
            faset::runtime::Runtime validator(config);
            validator.load(document);
            faset::player::SceneView view(assetsPath);
            view.build(document, 16.0f / 9.0f);
            for (const auto& diagnostic : view.diagnostics()) {
                if (diagnostic.starts_with("error:"))
                    throw std::runtime_error(diagnostic);
                std::cerr << diagnostic << '\n';
            }
            std::cout << nlohmann::json{{"validated", true},
                                        {"dimension", document.value("dimension", 3)}}
                             .dump()
                      << '\n';
            return 0;
        }
        const auto worldStarted = Clock::now();
        auto world = std::make_unique<faset::runtime::Runtime>(config);
        faset::gameplay::registerGameplay(*world);
#if defined(FASET_HAS_LUA)
        if (lua)
            lua->registerBehaviors(*world);
#endif
        world->load(document);
        std::size_t logCursor = 0;
        auto printGameplayLogs = [&]() {
            while (logCursor < world->diagnostics().size())
                std::cerr << world->diagnostics()[logCursor++] << '\n';
#if defined(FASET_HAS_LUA)
            if (lua)
                for (const auto& message : lua->takeLogs())
                    std::cerr << message << '\n';
#endif
        };
        printGameplayLogs();
        faset::player::SceneView view(assetsPath);
        const auto rendererStarted = Clock::now();
        faset::render::RendererConfig renderConfig;
        renderConfig.width = 1280;
        renderConfig.height = 720;
        renderConfig.title = document.value("name", std::string("Faset Player"));
        renderConfig.headless = headless;
        renderConfig.validation = true;
        renderConfig.visibility_mode = visibilityMode;
        faset::render::Renderer renderer(renderConfig);
        const auto rendererReady = Clock::now();
        std::vector<ProfileSample> profile;
        if (!profilePath.empty())
            profile.reserve(static_cast<std::size_t>(maximumFrames));
        Json firstFrameMs = nullptr;
        std::set<std::string> held;
        bool stop = false;
        std::uint64_t frames = 0;
        std::set<std::string> reported;
        std::uint64_t controlSequence = 0;
        std::string previousControl;
        auto previous = std::chrono::steady_clock::now();
#if defined(FASET_HAS_LUA)
        auto lastLuaCheck = Clock::now();
        std::string lastLuaFingerprint = luaProject.fingerprint;
        std::string lastLuaReloadError;
        auto reloadLua = [&](bool force) {
            if (!watchLua)
                return;
            const auto now = Clock::now();
            if (!force && now - lastLuaCheck < std::chrono::milliseconds(500))
                return;
            lastLuaCheck = now;
            std::string candidateFingerprint;
            try {
                const auto candidateProject = faset::scripting::loadLuaProject(projectRoot);
                candidateFingerprint = candidateProject.fingerprint;
                if (!force && candidateProject.fingerprint == lastLuaFingerprint)
                    return;
                // Do not repeatedly execute a broken candidate every half-second.
                // A corrected source or manifest produces a new fingerprint.
                lastLuaFingerprint = candidateProject.fingerprint;
                auto candidateSchema = nativeSchema;
                std::unique_ptr<faset::scripting::LuaModule> candidateLua;
                if (candidateProject.enabled()) {
                    candidateLua = std::make_unique<faset::scripting::LuaModule>(candidateProject);
                    for (const auto& type : candidateLua->schema())
                        candidateSchema.push_back(type);
                }
                faset::runtime::validate_scene_schemas(document, candidateSchema);
                if (candidateLua)
                    candidateLua->validateScene(document);
                auto candidateWorld = std::make_unique<faset::runtime::Runtime>(config);
                faset::gameplay::registerGameplay(*candidateWorld);
                if (candidateLua)
                    candidateLua->registerBehaviors(*candidateWorld);
                candidateWorld->load(document);
                // Runtime isolates callback exceptions into diagnostics. A bad
                // on_start must not replace the currently running scene.
                if (!candidateWorld->diagnostics().empty())
                    throw std::runtime_error(candidateWorld->diagnostics().front());
                candidateWorld->setPaused(world->paused());
                world->clear();
                printGameplayLogs();
                world = std::move(candidateWorld);
                lua = std::move(candidateLua);
                logCursor = 0;
                printGameplayLogs();
                lastLuaReloadError.clear();
                // Compilation and initialization are not simulation wall time.
                previous = Clock::now();
                std::cerr << "Lua reloaded: scene restarted; runtime state reset\n";
            } catch (const std::exception& error) {
                const std::string message =
                    std::string("Lua reload rejected; previous scene retained: ") + error.what();
                // Bad/missing manifests may fail before a fingerprint exists.
                // Retry them on the next poll but report an unchanged failure once.
                const auto failure = candidateFingerprint + "\n" + message;
                if (force || failure != lastLuaReloadError)
                    std::cerr << message << '\n';
                lastLuaReloadError = failure;
            }
        };
#endif
        while (!stop && !renderer.should_close() &&
               (maximumFrames == 0 || frames < maximumFrames)) {
            const auto frameStarted = Clock::now();
            faset::runtime::InputState input;
            bool singleStep = false;
            bool requestLuaReload = false;
            if (!controlPath.empty() && std::filesystem::is_regular_file(controlPath)) {
                try {
                    if (std::filesystem::file_size(controlPath) > 65536)
                        throw std::runtime_error("control message exceeds 64 KiB");
                    auto content = faset::read_text(controlPath);
                    if (content != previousControl) {
                        previousControl = content;
                        const auto message = nlohmann::json::parse(content);
                        const auto& sequence = message.at("sequence");
                        if (!(sequence.is_number_unsigned() ||
                              (sequence.is_number_integer() && sequence.get<std::int64_t>() >= 0)))
                            throw std::invalid_argument(
                                "control sequence must be a nonnegative integer");
                        const auto value = sequence.get<std::uint64_t>();
                        if (value > controlSequence) {
                            const auto command = message.at("command").get<std::string>();
                            if (command == "pause")
                                world->setPaused(true);
                            else if (command == "resume")
                                world->setPaused(false);
                            else if (command == "step") {
                                world->setPaused(true);
                                singleStep = true;
                            } else if (command == "stop")
                                stop = true;
                            else if (command == "reload-lua" && watchLua)
                                requestLuaReload = true;
                            else
                                throw std::invalid_argument("unsupported control command");
                            controlSequence = value;
                        }
                    }
                } catch (const std::exception& error) {
                    const std::string message =
                        std::string("Player control ignored: ") + error.what();
                    if (reported.insert(message).second)
                        std::cerr << message << '\n';
                }
            }
            for (const auto& event : renderer.poll_events()) {
                using Type = faset::render::Event::Type;
                if (event.type == Type::Quit)
                    stop = true;
                if (event.type == Type::FocusLost)
                    held.clear();
                std::string key = event.key;
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                if (event.type == Type::KeyUp)
                    held.erase(key);
                if (event.type == Type::KeyDown) {
                    held.insert(key);
                    if (key == "ESCAPE")
                        stop = true;
                    if (!event.repeat) {
                        if (key == "SPACE")
                            input.jumpPressed = true;
                        if (key == "E")
                            input.interactPressed = true;
                        if (key == "P")
                            world->setPaused(!world->paused());
                        if (key == "N")
                            singleStep = true;
                        if (key == "F3")
                            debugPhysics = !debugPhysics;
                    }
                }
            }
            if (stop)
                break;
#if defined(FASET_HAS_LUA)
            reloadLua(requestLuaReload);
#else
            (void)requestLuaReload;
#endif
            input.horizontal = float(held.contains("D") || held.contains("RIGHT")) -
                               float(held.contains("A") || held.contains("LEFT"));
            input.vertical = float(held.contains("W") || held.contains("UP")) -
                             float(held.contains("S") || held.contains("DOWN"));
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = maximumFrames
                                       ? config.fixedDelta
                                       : std::chrono::duration<double>(now - previous).count();
            previous = now;
            const auto simulationStarted = Clock::now();
            const auto runtimeStats = singleStep && world->paused()
                                          ? world->singleStep(input)
                                          : world->advance(elapsed, input);
            const auto simulationFinished = Clock::now();
            const auto presentation = world->snapshotJson();
            auto snapshot = view.build(presentation, static_cast<float>(renderer.width()) /
                                                         std::max(1u, renderer.height()));
            if (debugPhysics)
                view.appendPhysicsDebug(snapshot, physicsScene(*world, presentation));
            const auto snapshotFinished = Clock::now();
            for (const auto& diagnostic : view.diagnostics()) {
                if (diagnostic.starts_with("error:"))
                    throw std::runtime_error(diagnostic);
                if (reported.insert(diagnostic).second)
                    std::cerr << diagnostic << '\n';
            }
            printGameplayLogs();
            const auto renderStarted = Clock::now();
            renderer.render(snapshot);
            if (frames == 0 && visibilityMode != faset::render::VisibilityMode::Direct &&
                !renderer.stats().gpu_visibility_active)
                std::cerr << "Requested GPU visibility is unavailable on this device; "
                             "using Direct rendering.\n";
            const auto frameFinished = Clock::now();
            if (frames == 0)
                firstFrameMs = milliseconds(started, frameFinished);
            if (!profilePath.empty()) {
                const auto measured = renderer.stats();
                profile.push_back(
                    {milliseconds(frameStarted, frameFinished),
                     milliseconds(simulationStarted, simulationFinished),
                     milliseconds(simulationFinished, snapshotFinished),
                     milliseconds(renderStarted, frameFinished), measured.cpu_ms, measured.gpu_ms,
                     measured.readback_cpu_ms, runtimeStats, measured.draw_calls, measured.vertices,
                     measured.gpu_allocated_bytes, measured.texture_count, debugPhysics,
                     measured.gpu_visibility_active});
            }
            ++frames;
        }
        const auto completedTicks = world->snapshot().tick;
        // Run normal shutdown while diagnostics are still observable. Runtime's
        // destructor is a fallback and cannot print messages after this scope ends.
        world->clear();
        printGameplayLogs();
        if (!capturePath.empty()) {
            if (frames == 0)
                throw std::runtime_error("No frame was rendered for capture");
            renderer.capture(capturePath);
        }
        const auto stats = renderer.stats();
        if (!profilePath.empty()) {
            auto report = profileFrames(profile);
            report.update(
                {{"format", "faset.player-profile"},
                 {"version", 1},
                 {"requested_frames", maximumFrames},
                 {"completed_frames", frames},
                 {"dimension", document.value("dimension", 3)},
                 {"device", stats.device},
                 {"width", renderer.width()},
                 {"height", renderer.height()},
                 {"validation_enabled", stats.validation_enabled},
                 {"validation_errors", stats.validation_errors},
                 {"presentation_mode", headless ? "offscreen" : "windowed"},
                 {"visibility_mode", visibilityName},
                 {"simulation_mode", "synthetic_fixed_timestep"},
                 {"fixed_delta_seconds", config.fixedDelta},
                 {"percentile_method", "nearest_rank_all_completed_frames_no_warmup_exclusion"},
                 {"resource_notes", "gpu_allocated_bytes sums live Vulkan memory allocations, "
                                    "including alignment, excluding driver internals. "
                                    "texture_count includes the white fallback texture."},
                 {"timing_notes",
                  "Durations use steady_clock wall time, not thread CPU usage. "
                  "render_call/renderer_cpu include GPU waits, readback and window presentation. "
                  "renderer_readback_cpu measures map/copy/unmap wall time within that call. "
                  "GPU timestamps cover submitted rendering, not CPU work. A missing/zero GPU "
                  "timestamp is null. Frame durations exclude profile bookkeeping and final "
                  "capture/profile file writes. Startup begins at Player application entry "
                  "after platform argument normalization, excluding OS loader/launcher."},
                 {"startup_ms",
                  {{"scene_read", milliseconds(sceneReadStarted, sceneReadFinished)},
                   {"world_initialization", milliseconds(worldStarted, rendererStarted)},
                   {"renderer_initialization", milliseconds(rendererStarted, rendererReady)},
                   {"main_to_first_frame", firstFrameMs}}}});
            faset::atomic_write_json(profilePath, report);
        }
        std::cout << nlohmann::json{{"frames", frames},
                                    {"ticks", completedTicks},
                                    {"dimension", document.value("dimension", 3)},
                                    {"device", stats.device},
                                    {"visibility_mode", visibilityName},
                                    {"gpu_visibility_active", stats.gpu_visibility_active},
                                    {"validation_errors", stats.validation_errors}}
                         .dump()
                  << '\n';
        return stats.validation_errors == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Player failed: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, player_main);
}
#else
int main(int argc, char** argv) {
    return player_main(argc, argv);
}
#endif
