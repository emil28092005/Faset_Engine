#include <faset/core/io.hpp>
#include <iostream>

// Native stand-in for CMake and SchemaExporter. Tests exercise the real
// asynchronous BuildService and publication code without compiling a game.
namespace fs = std::filesystem;
using namespace faset;
int tool_main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--output") {
            atomic_write_json(path_from_utf8(argv[2]), read_json("schema-fixture.json"));
            return 0;
        }
        if (argc > 2 && std::string_view(argv[1]) == "--build")
            return 0;
        fs::path build;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "-B")
                build = path_from_utf8(argv[i + 1]);
        if (build.empty())
            throw std::runtime_error("Fixture expects CMake configure or SchemaExporter arguments");
        fs::create_directories(build / "shaders");
        atomic_write(build / "CMakeCache.txt", "Native schema publication fixture\n");
        const auto self = fs::absolute(path_from_utf8(argv[0]));
#ifdef _WIN32
        constexpr auto suffix = ".exe";
#else
        constexpr auto suffix = "";
#endif
        for (const auto* target : {"faset_player", "faset_schema_exporter"})
            fs::copy_file(self, build / (std::string(target) + suffix),
                          fs::copy_options::overwrite_existing);
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain"})
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
