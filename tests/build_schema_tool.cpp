#include <faset/core/io.hpp>
#include <iostream>

// Native stand-in for CMake and SchemaExporter. Tests exercise the real
// asynchronous BuildService and publication code without compiling a game.
namespace fs = std::filesystem;
using namespace faset;
int tool_main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string_view(argv[1]) == "--output") {
            const auto count_file = fs::path("schema-export-count.txt");
            const auto count = fs::exists(count_file) ? std::stoi(read_text(count_file)) : 0;
            atomic_write(count_file, std::to_string(count + 1));
            if (argc == 5 && std::string_view(argv[3]) == "--project") {
                const auto snapshot = path_from_utf8(argv[4]);
                const auto project = read_json(snapshot / "project.faset.json");
                for (const auto& name : project.at("scripting").at("lua").at("scripts"))
                    if (!fs::is_regular_file(snapshot / path_from_utf8(name.get<std::string>())))
                        throw std::runtime_error(
                            "Schema exporter did not receive captured sources");
                atomic_write_json("exporter-project-fixture.json", project);
                if (fs::exists("mutate-lua-during-build"))
                    atomic_write("Scripts/main.lua", "-- changed while schema was exporting\n");
                if (fs::exists("mutate-lua-snapshot"))
                    atomic_write(snapshot / "Scripts/main.lua", "-- corrupt snapshot\n");
            }
            if (fs::exists("mutate-cpp-header-during-build"))
                atomic_write("Scripts/Extensions/BuildOnly.hpp", "#define BUILD_ONLY 2\n");
            atomic_write_json(path_from_utf8(argv[2]), read_json("schema-fixture.json"));
            return 0;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--validate") {
            if (fs::exists("project.faset.json")) {
                const auto manifest = read_json("project.faset.json");
                for (const auto& name : manifest.at("scripting").at("lua").at("scripts"))
                    if (!fs::is_regular_file(path_from_utf8(name.get<std::string>())))
                        throw std::runtime_error("Packaged Player is missing a Lua entry source");
            }
            std::cout << "Native packaging fixture validated\n";
            return 0;
        }
        if (argc > 2 && std::string_view(argv[1]) == "--build") {
            if (fs::exists("flood-warnings-and-fail-build")) {
                for (int index = 0; index < 250; ++index)
                    std::cerr << "/external/library.cpp:1:1: warning: dependency warning "
                              << index << '\n';
                std::cerr << path_to_utf8(fs::current_path() / "Scripts/Gameplay.cpp")
                          << ":7:3: error: gameplay error after warnings\n";
                return 1;
            }
            if (fs::exists("emit-clang-error-and-fail-build")) {
                std::cerr << path_to_utf8(fs::current_path() / "Scripts/Gameplay.cpp")
                          << ":7:3: error: fixture compile failure\n";
                return 1;
            }
            if (fs::exists("fail-unparseable-build")) {
                std::cerr << "Synthetic native build failed without a source location\n";
                return 1;
            }
            return 0;
        }
        fs::path build;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "-B")
                build = path_from_utf8(argv[i + 1]);
        if (build.empty())
            throw std::runtime_error("Fixture expects CMake configure or SchemaExporter arguments");
        Json arguments = Json::array();
        for (int i = 1; i < argc; ++i)
            arguments.push_back(argv[i]);
        atomic_write_json("configure-fixture.json", arguments);
        fs::create_directories(build / "shaders");
        atomic_write(build / "CMakeCache.txt", "Native schema publication fixture\n");
        for (const auto* name : {"sdl3", "entt", "box2d", "box3d", "json", "stb"})
            atomic_write(build / "_deps" / (std::string(name) + "-src") / "LICENSE.txt",
                         "Synthetic dependency notice for packaging tests only.\n");
        const auto self = fs::absolute(path_from_utf8(argv[0]));
#ifdef _WIN32
        constexpr auto suffix = ".exe";
#else
        constexpr auto suffix = "";
#endif
        for (const auto* target : {"faset_player", "faset_schema_exporter"})
            fs::copy_file(self, build / (std::string(target) + suffix),
                          fs::copy_options::overwrite_existing);
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain",
                                  "gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                  "gpuHzbMain", "gpuPostCullMain"})
            for (const auto* extension : {".spv", ".reflection.json"})
                atomic_write(build / "shaders" / (std::string(entry) + extension), "fixture\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, tool_main);
}
#else
int main(int argc, char** argv) {
    return tool_main(argc, argv);
}
#endif
