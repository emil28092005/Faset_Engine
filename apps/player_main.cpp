#include "Gameplay.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <faset/core/io.hpp>
#include <faset/player/SceneView.hpp>
#include <faset/runtime/Runtime.hpp>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace {
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
void validatePackagedShaders(const std::filesystem::path& directory) {
    for (const auto* name : {"vertexMain.spv", "fragmentMain.spv", "shadowMain.spv"}) {
        const auto path = directory / "shaders" / name;
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("Packaged shader is missing: " + path.string());
        const auto bytes = faset::read_text(path);
        if (bytes.size() < 20 || bytes.size() % 4 != 0 ||
            static_cast<unsigned char>(bytes[0]) != 0x03 ||
            static_cast<unsigned char>(bytes[1]) != 0x02 ||
            static_cast<unsigned char>(bytes[2]) != 0x23 ||
            static_cast<unsigned char>(bytes[3]) != 0x07)
            throw std::runtime_error("Packaged shader is not a SPIR-V module: " + path.string());
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        std::filesystem::path scenePath, assetsPath, capturePath, controlPath;
        bool headless = false, validateOnly = false;
        std::uint64_t maximumFrames = 0;
        std::set<std::string> options;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "faset_player [--scene PATH] [--assets CACHE] [--frames N] "
                             "[--headless] [--capture PATH.ppm] [--validate] [--control PATH]\n"
                             "No --scene: open scene.fscene beside the executable. CACHE contains "
                             "assets/<id>/.\n"
                             "Headless uses offscreen Vulkan; --frames uses the configured fixed "
                             "simulation delta.\n"
                             "--validate checks scene/resources on CPU without gameplay callbacks "
                             "or Vulkan initialization.\n"
                             "--control is an optional editor mailbox for pause/resume/step/stop, "
                             "without world queries.\n"
                             "Keys: A/D horizontal, W/S vertical, Space jump, E interact, P pause, "
                             "N single-step, Escape quit.\n";
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
                scenePath = value();
            else if (arg == "--assets")
                assetsPath = value();
            else if (arg == "--capture")
                capturePath = value();
            else if (arg == "--control")
                controlPath = value();
            else if (arg == "--frames")
                maximumFrames = count(value());
            else if (arg == "--headless")
                headless = true;
            else if (arg == "--validate")
                validateOnly = true;
            else
                throw std::invalid_argument("Unknown option: " + arg);
        }
        if (scenePath.empty())
            scenePath = executableDirectory(argv[0]) / "scene.fscene";
        scenePath = std::filesystem::absolute(scenePath).lexically_normal();
        if (assetsPath.empty())
            assetsPath = scenePath.parent_path();
        if (!std::filesystem::is_directory(assetsPath))
            throw std::invalid_argument("Asset cache directory does not exist: " +
                                        assetsPath.string());
        if (headless && maximumFrames == 0)
            maximumFrames = 1;
        const auto document = faset::player::readScene(scenePath);
        const auto config = simulationConfig(document);
        const auto executableRoot = executableDirectory(argv[0]);
        if (scenePath.extension() == ".fscene" &&
            std::filesystem::equivalent(scenePath.parent_path(), executableRoot))
            validatePackagedShaders(executableRoot);
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
        faset::runtime::Runtime world(config);
        faset::gameplay::registerGameplay(world);
        world.load(document);
        faset::player::SceneView view(assetsPath);
        faset::render::Renderer renderer(
            {1280, 720, document.value("name", std::string("Faset Player")), headless, true});
        std::set<std::string> held;
        bool stop = false;
        std::uint64_t frames = 0;
        std::size_t logCursor = 0;
        std::set<std::string> reported;
        std::uint64_t controlSequence = 0;
        std::string previousControl;
        auto previous = std::chrono::steady_clock::now();
        while (!stop && !renderer.should_close() &&
               (maximumFrames == 0 || frames < maximumFrames)) {
            faset::runtime::InputState input;
            bool singleStep = false;
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
                                world.setPaused(true);
                            else if (command == "resume")
                                world.setPaused(false);
                            else if (command == "step") {
                                world.setPaused(true);
                                singleStep = true;
                            } else if (command == "stop")
                                stop = true;
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
                            world.setPaused(!world.paused());
                        if (key == "N")
                            singleStep = true;
                    }
                }
            }
            if (stop)
                break;
            input.horizontal = float(held.contains("D") || held.contains("RIGHT")) -
                               float(held.contains("A") || held.contains("LEFT"));
            input.vertical = float(held.contains("W") || held.contains("UP")) -
                             float(held.contains("S") || held.contains("DOWN"));
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = maximumFrames
                                       ? config.fixedDelta
                                       : std::chrono::duration<double>(now - previous).count();
            previous = now;
            if (singleStep && world.paused())
                world.singleStep(input);
            else
                world.advance(elapsed, input);
            auto snapshot = view.build(world.snapshotJson(), static_cast<float>(renderer.width()) /
                                                                 std::max(1u, renderer.height()));
            for (const auto& diagnostic : view.diagnostics()) {
                if (diagnostic.starts_with("error:"))
                    throw std::runtime_error(diagnostic);
                if (reported.insert(diagnostic).second)
                    std::cerr << diagnostic << '\n';
            }
            while (logCursor < world.diagnostics().size())
                std::cerr << world.diagnostics()[logCursor++] << '\n';
            renderer.render(snapshot);
            ++frames;
        }
        if (!capturePath.empty()) {
            if (frames == 0)
                throw std::runtime_error("No frame was rendered for capture");
            renderer.capture(capturePath);
        }
        const auto stats = renderer.stats();
        std::cout << nlohmann::json{{"frames", frames},
                                    {"ticks", world.snapshot().tick},
                                    {"dimension", document.value("dimension", 3)},
                                    {"device", stats.device},
                                    {"validation_errors", stats.validation_errors}}
                         .dump()
                  << '\n';
        return stats.validation_errors == 0 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "Player failed: " << error.what() << '\n';
        return 1;
    }
}
