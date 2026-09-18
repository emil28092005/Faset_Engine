#include <chrono>
#include <csignal>
#include <faset/core/io.hpp>
#include <faset/editor/mcp.hpp>
#include <faset/editor/session.hpp>
#include <iostream>
#include <thread>
#ifdef FASET_HAS_EDITOR_UI
#include <faset/editor/editor_ui.hpp>
#include <faset/editor/project_launcher.hpp>
namespace faset::editor {
int run_editor_ui(Session&, bool, std::uint64_t, const std::filesystem::path&);
}
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) {
    interrupted = 1;
}
std::filesystem::path executable_directory(const char* argument) {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        throw std::runtime_error("Cannot locate Editor executable");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
#else
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::absolute(argument).parent_path() : path.parent_path();
#endif
}
void help() {
    std::cout
        << "Faset Editor\n"
           "  faset_editor                         Open the project launcher\n"
           "  faset_editor --project PATH [--new NAME --dimension 2|3] [--scene RELATIVE_PATH]\n"
           "  faset_editor --project PATH --mcp [--gui]\n"
           "  faset_editor --project PATH --command JSON [--wait]\n"
           "Options: --engine SDK_SOURCE, --headless, --frames N, --capture PATH.ppm\n"
           "MCP uses JSON-RPC over stdio and only exposes authoring/editor services.\n";
}
} // namespace
int editor_main(int argc, char** argv) {
    using namespace faset;
    using namespace faset::editor;
    try {
        std::filesystem::path project, engine = path_from_utf8(FASET_ENGINE_SOURCE), scene, capture;
        std::string new_name, command;
        int dimension = 3;
        bool mcp = false, gui = true, explicit_gui = false, wait = false;
        std::uint64_t frames = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() {
                require(i + 1 < argc, "cli.argument", "Missing value for " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--help" || arg == "-h") {
                help();
                return 0;
            }
            if (arg == "--project")
                project = path_from_utf8(value());
            else if (arg == "--engine")
                engine = path_from_utf8(value());
            else if (arg == "--new")
                new_name = value();
            else if (arg == "--dimension")
                dimension = std::stoi(value());
            else if (arg == "--scene")
                scene = path_from_utf8(value());
            else if (arg == "--mcp")
                mcp = true;
            else if (arg == "--gui") {
                gui = true;
                explicit_gui = true;
            } else if (arg == "--headless")
                gui = false;
            else if (arg == "--command") {
                command = value();
                gui = false;
            } else if (arg == "--wait")
                wait = true;
            else if (arg == "--frames") {
                const auto text = value();
                require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos,
                        "cli.frames", "Frame count must be positive");
                frames = std::stoull(text);
                require(frames > 0 && frames <= 10000000, "cli.frames", "Frame count out of range");
            } else if (arg == "--capture")
                capture = path_from_utf8(value());
            else
                throw Error("cli.option", "Unknown option: " + arg);
        }
        require(!(mcp && !command.empty()), "cli.mode", "Choose MCP or a single command");
        if (mcp && !explicit_gui)
            gui = false;
        require(!project.empty() || (gui && !mcp && new_name.empty()), "cli.project",
                "Use --project PATH for command, MCP, or --new modes");
        auto previous_project = project;
        for (;;) {
            if (project.empty()) {
#ifdef FASET_HAS_EDITOR_UI
                const auto selection =
                    run_project_launcher(engine, previous_project, frames, capture);
                if (!selection)
                    return 0;
                project = selection->path;
                new_name = selection->create ? selection->name : std::string();
                dimension = selection->dimension;
#else
                throw Error("editor.gui_unavailable",
                            "This build has no graphical project launcher; use --project PATH");
#endif
            }
            Session session({std::filesystem::absolute(project), std::filesystem::absolute(engine),
                             executable_directory(argv[0])});
            if (!new_name.empty())
                session.scaffold(new_name, dimension);
            const auto settings = session.project();
#ifdef FASET_HAS_EDITOR_UI
            if (gui && std::filesystem::exists(project / "project.faset.json"))
                remember_project(project);
#endif
            if (scene.empty())
                scene = path_from_utf8(settings.value("start_scene", std::string()));
            if (!scene.empty() &&
                std::filesystem::exists(project_path(session.config().project_root, scene)))
                session.authoring().open(scene);
            if (!command.empty()) {
                const auto request = Json::parse(command);
                auto result = session.commands().call(request.at("name"),
                                                      request.value("arguments", Json::object()));
                if (wait && result.contains("job")) {
                    const auto id = result.at("job");
                    const auto started = std::chrono::steady_clock::now();
                    for (;;) {
                        session.poll();
                        result = session.commands().call("faset_job", {{"id", id}});
                        const auto state = result.value("state", std::string());
                        if (state == "succeeded" || state == "failed" || state == "cancelled" ||
                            state == "conflict")
                            break;
                        require(std::chrono::steady_clock::now() - started <
                                    std::chrono::minutes(30),
                                "job.timeout", "Command wait exceeded 30 minutes");
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
                std::cout << result.dump(2) << '\n';
                const auto state = result.value("state", std::string());
                return state == "failed" || state == "cancelled" || state == "conflict" ? 1 : 0;
            }
            if (gui) {
#ifdef FASET_HAS_EDITOR_UI
                const int result = run_editor_ui(session, mcp, frames, capture);
                if (result != 3)
                    return result;
                previous_project = project;
                project.clear();
                scene.clear();
                new_name.clear();
                continue;
#else
                throw Error(
                    "editor.gui_unavailable",
                    "This build has no graphical editor; use --mcp or --command, or build with "
                    "FASET_BUILD_EDITOR=ON");
#endif
            }
            require(mcp, "cli.mode", "Headless mode requires --mcp or --command");
            std::signal(SIGINT, interrupt);
            std::signal(SIGTERM, interrupt);
            McpServer server(session.commands());
            StdioTransport transport;
            while (!interrupted && !transport.closed()) {
                for (const auto& line : transport.poll()) {
                    try {
                        const auto reply = server.handle(Json::parse(line));
                        if (reply)
                            transport.send(*reply);
                    } catch (const Json::exception&) {
                        transport.send(server.parse_error());
                    }
                }
                session.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return 0;
        }
    } catch (const Error& error) {
        std::cerr << error.json().dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << Json{{"code", "editor.failure"}, {"message", error.what()}}.dump() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, editor_main);
}
#else
int main(int argc, char** argv) {
    return editor_main(argc, argv);
}
#endif
