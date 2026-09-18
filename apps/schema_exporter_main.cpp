#include "Gameplay.hpp"
#include <faset/core/io.hpp>
#include <faset/runtime/schema.hpp>
#include <faset/scripting/project.hpp>
#if defined(FASET_HAS_LUA)
#include <faset/scripting/LuaModule.hpp>
#endif
#include <iostream>
#include <stdexcept>

int schema_main(int argc, char** argv) {
    try {
        std::filesystem::path output, projectRoot;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "faset_schema_exporter [--output PATH] [--project ROOT]\n"
                             "Exports C++ and declared Lua gameplay schemas without creating a "
                             "world or invoking lifecycle callbacks.\n";
                return 0;
            }
            if (argument == "--output" && i + 1 < argc && output.empty())
                output = faset::path_from_utf8(argv[++i]);
            else if (argument == "--project" && i + 1 < argc && projectRoot.empty())
                projectRoot = faset::path_from_utf8(argv[++i]);
            else
                throw std::invalid_argument("Unknown, repeated or incomplete argument: " +
                                            argument);
        }
        auto types = faset::gameplay::schema();
        if (!types.is_array())
            throw std::runtime_error("Gameplay schema() must return a type array");
        if (!projectRoot.empty()) {
            const auto project = faset::scripting::loadLuaProject(projectRoot);
            if (project.enabled()) {
#if defined(FASET_HAS_LUA)
                faset::scripting::LuaModule lua(project);
                for (const auto& type : lua.schema())
                    types.push_back(type);
#else
                throw std::runtime_error("This schema exporter was built without Lua support; "
                                         "configure FASET_ENABLE_LUA=ON for this project");
#endif
            }
        }
        // Check all IDs, including unused types, before publishing a manifest.
        // This player-side boundary intentionally has no authoring dependency.
        faset::runtime::validate_scene_schemas({{"entities", nlohmann::json::array()}}, types);
        const nlohmann::json manifest{{"format", "faset.schema"}, {"version", 1}, {"types", types}};
        if (output.empty())
            std::cout << manifest.dump(2) << '\n';
        else
            faset::atomic_write_json(output, manifest);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Schema export failed: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, schema_main);
}
#else
int main(int argc, char** argv) {
    return schema_main(argc, argv);
}
#endif
