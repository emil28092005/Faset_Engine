#include <faset/authoring/service.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/editor/build_service.hpp>
#include <faset/editor/commands.hpp>
#include <faset/scripting/project.hpp>
#include <iostream>
#include <limits>

using namespace faset;
namespace fs = std::filesystem;
namespace {
void check(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
Json manifest() {
    Json type{
        {"id", "game.mover"},
        {"version", 2},
        {"fields",
         {{"speed", {{"type", "number"}, {"default", 2.5}, {"min", 0}, {"max", 10}}},
          {"enabled", {{"type", "boolean"}, {"default", true}}}}},
        {"migrations",
         Json::array(
             {{{"from_version", 1},
               {"fields", {{"speed", {{"scale", 0.01}}}, {"enabled", {{"default", true}}}}}}})}};
    return {{"format", "faset.schema"}, {"version", 1}, {"types", Json::array({type})}};
}
void migration_contracts(const fs::path& root, const Json& metadata) {
    auto old_scene = authoring::make_scene("Legacy movement", 2);
    auto object = authoring::make_entity(authoring::builtin_schemas(), "Mover");
    object["id"] = "mover";
    object["components"].push_back({{"id", "movement"},
                                    {"type", "game.mover"},
                                    {"version", 1},
                                    {"fields", {{"speed", 300}, {"unknown", "preserve me"}}}});
    old_scene["entities"].push_back(object);
    atomic_write_json(root / "Scenes/legacy.scene.json", old_scene);
    const auto original = read_text(root / "Scenes/legacy.scene.json");
    authoring::AuthoringService service(root);
    service.replace_external_schemas(metadata);
    editor::Commands commands(service);
    const auto opened =
        commands.call("faset_document_open", {{"path", "Scenes/legacy.scene.json"}});
    const auto id = opened.at("id").get<std::string>();
    check(opened.at("scene") == old_scene && !opened.at("dirty").get<bool>(),
          "Opening an older schema preserves opaque data without implicit migration");
    auto apply = [&] {
        return commands.call("faset_scene_edit",
                             {{"document", id},
                              {"revision", service.query(id).at("revision")},
                              {"operations", Json::array({{{"op", "entity.rename"},
                                                           {"entity", "mover"},
                                                           {"name", "Migrated mover"}},
                                                          {{"op", "component.migrate"},
                                                           {"entity", "mover"},
                                                           {"component", "movement"}}})}});
    };
    const auto migrated = apply();
    const auto& value = migrated.at("scene").at("entities")[0].at("components")[1];
    check(value.at("id") == "movement" && value.at("version") == 2 &&
              value.at("fields").at("speed") == 3.0 && value.at("fields").at("enabled") == true &&
              value.at("fields").at("unknown") == "preserve me",
          "Exported declarative steps convert values and retain IDs and unknown fields");
    check(read_text(root / "Scenes/legacy.scene.json") == original,
          "Explicit migration remains unsaved until Save");
    const auto undone =
        commands.call("faset_undo", {{"document", id}, {"revision", migrated.at("revision")}});
    check(undone.at("scene") == old_scene, "One Undo restores the entire migration transaction");
    authoring::AuthoringService recovered(root);
    recovered.replace_external_schemas(metadata);
    check(recovered.recover(id).at("scene") == old_scene,
          "Recovery preserves the journal's older version without automatic migration");
    commands.call("faset_redo", {{"document", id}, {"revision", undone.at("revision")}});
    check(service.query(id).at("scene") == migrated.at("scene"), "Redo restores migrated values");
    commands.call("faset_undo", {{"document", id}, {"revision", service.query(id).at("revision")}});
    for (const auto* failure : {"missing", "manual", "overflow"}) {
        auto rules = metadata;
        if (std::string_view(failure) == "missing")
            rules["types"][0].erase("migrations");
        else if (std::string_view(failure) == "manual")
            rules["types"][0]["migrations"][0]["fields"]["speed"] = {{"require_manual", true}};
        else
            rules["types"][0]["migrations"][0]["fields"]["speed"]["scale"] =
                std::numeric_limits<double>::max();
        service.replace_external_schemas(rules);
        const auto before = service.query(id);
        bool rejected{};
        try {
            (void)apply();
        } catch (const Error&) {
            rejected = true;
        }
        check(rejected && service.query(id) == before &&
                  read_text(root / "Scenes/legacy.scene.json") == original,
              "Missing/manual/overflow migration preserves document, revision, Undo and source");
        authoring::AuthoringService reopened(root);
        reopened.replace_external_schemas(rules);
        check(reopened.open("Scenes/legacy.scene.json").at("scene") == old_scene,
              "An unavailable migration never prevents opening old data");
    }
    service.replace_external_schemas(metadata);
    auto addition = object;
    addition["id"] = "local-mover";
    addition["components"][0]["id"] = "local-transform";
    addition["components"][1]["id"] = "local-movement";
    auto current = service.query(id);
    current = service.transact(id, current.at("revision"),
                               Json::array({{{"op", "template.instance"},
                                             {"instance",
                                              {{"id", "local-instance"},
                                               {"source", "Scenes/unused.scene.json"},
                                               {"additions", Json::array({addition})}}}}}));
    const auto local_before = current;
    current = commands.call("faset_scene_edit",
                            {{"document", id},
                             {"revision", current.at("revision")},
                             {"operations", Json::array({{{"op", "component.migrate"},
                                                          {"instance", "local-instance"},
                                                          {"entity", "local-mover"},
                                                          {"component", "local-movement"}}})}});
    check(current.at("scene")
                  .at("instances")[0]
                  .at("additions")[0]
                  .at("components")[1]
                  .at("fields")
                  .at("speed") == 3.0,
          "Instance-local additions use the same migration command");
    check(service.undo(id, current.at("revision")).at("scene") == local_before.at("scene"),
          "Local-addition migration is one undoable edit");
}
int test_main(int argc, char** argv) {
    const auto root =
        fs::temp_directory_path() / path_from_utf8("Faset schema Café 世界 " + new_id());
    try {
        check(argc == 3, "Expected native fixture tool and engine root");
        editor::BuildConfig config;
        config.project_root = root / "project";
        config.engine_root = fs::absolute(path_from_utf8(argv[2]));
        config.cmake = path_to_utf8(fs::absolute(path_from_utf8(argv[1])));
        config.configure_arguments = {"-DCMAKE_CXX_COMPILER=fixture-does-not-compile"};
        editor::BuildService builds(config);
        builds.scaffold("Schema publication", 2);
        atomic_write(config.project_root / "Scripts/Extensions/BuildOnly.hpp",
                     "#define BUILD_ONLY 1\n");
        authoring::AuthoringService authoring(config.project_root, authoring::builtin_schemas());
        const auto valid = manifest();
        atomic_write_json(config.project_root / "schema-fixture.json", valid);
        {
            auto compiler_config = config;
            compiler_config.project_root = root / "c-only-compiler-project";
            compiler_config.configure_arguments = {
                "-DCMAKE_C_COMPILER:FILEPATH=fixture-c-only"};
            editor::BuildService compiler_build(compiler_config);
            compiler_build.scaffold("Compiler argument fixture", 2);
            atomic_write_json(compiler_config.project_root / "schema-fixture.json", valid);
            const auto compiler_result = compiler_build.wait(compiler_build.start_build());
            check(compiler_result.state == "succeeded",
                  "C-only compiler override fixture builds: " + compiler_result.error);
            const auto compiler_args =
                read_json(compiler_config.project_root / "configure-fixture.json");
            std::size_t c_overrides{};
            bool cxx_default{};
            for (const auto& arg : compiler_args) {
                const auto text = arg.get<std::string>();
                if (text.starts_with("-DCMAKE_C_COMPILER")) {
                    ++c_overrides;
                    check(text == "-DCMAKE_C_COMPILER:FILEPATH=fixture-c-only",
                          "Typed C override is not replaced by a default compiler");
                }
                if (text.starts_with("-DCMAKE_CXX_COMPILER="))
                    cxx_default = true;
            }
            check(c_overrides == 1 && cxx_default,
                  "C and C++ compiler defaults are selected independently");
        }
        auto first = builds.wait(builds.start_build());
        check(first.state == "succeeded", "Valid custom schema v2 publishes: " + first.error);
        fs::path compiled_scripts;
        for (const auto& arg : read_json(config.project_root / "configure-fixture.json")) {
            const auto text = arg.get<std::string>();
            constexpr std::string_view prefix = "-DFASET_GAMEPLAY_SOURCE_DIR=";
            if (text.starts_with(prefix))
                compiled_scripts = path_from_utf8(text.substr(prefix.size()));
        }
        check(!compiled_scripts.empty() &&
                  compiled_scripts != config.project_root / "Scripts" &&
                  read_text(compiled_scripts / "Extensions/BuildOnly.hpp") ==
                      "#define BUILD_ONLY 1\n",
              "Native C++ build receives the immutable source snapshot");
        const auto repeated = builds.wait(builds.start_build());
        check(repeated.state == "succeeded" &&
                  repeated.result.at("generation") == first.result.at("generation") &&
                  repeated.result.at("schema_cache_hit") == true &&
                  repeated.result.at("generation_reused") == true &&
                  read_text(config.project_root / "schema-export-count.txt") == "1",
              "Unchanged native build reuses a verified schema generation");
        atomic_write(path_from_utf8(first.result.at("schema").get<std::string>()), "truncated");
        first = builds.wait(builds.start_build());
        check(first.state == "succeeded" && first.result.at("schema_cache_hit") == false &&
                  read_text(config.project_root / "schema-export-count.txt") == "2",
              "Corrupt schema cannot be a cache hit");
        atomic_write(path_from_utf8(first.result.at("directory").get<std::string>()) /
                         "shaders/vertexMain.spv",
                     "corrupt");
        first = builds.wait(builds.start_build());
        check(first.state == "succeeded" && first.result.at("schema_cache_hit") == false &&
                  read_text(config.project_root / "schema-export-count.txt") == "3",
              "Corrupt shader cannot be a cache hit");
        const auto directory = path_from_utf8(first.result.at("directory").get<std::string>());
        for (const auto* entry : {"gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                  "gpuHzbMain", "gpuPostCullMain"})
            for (const auto* extension : {".spv", ".reflection.json"})
                check(fs::is_regular_file(directory / "shaders" /
                                          (std::string(entry) + extension)),
                      "Published Player contains every checked P2 shader artifact");
        const auto player = path_from_utf8(first.result.at("player").get<std::string>());
        const auto schema = path_from_utf8(first.result.at("schema").get<std::string>());
        const auto last_build = builds.config().cache_root / "last_build.json";
        const auto previous_pointer = read_text(last_build);
        const auto previous_player = sha256_file(player);
        const auto previous_schema = read_text(schema);
        const auto previous_manifest = read_text(directory / "manifest.json");
        atomic_write(config.project_root / "emit-clang-error-and-fail-build", "fixture\n");
        const auto compile_failed = builds.wait(builds.start_build());
        check(compile_failed.state == "failed" &&
                  read_text(last_build) == previous_pointer &&
                  compile_failed.json().at("diagnostics").size() == 1 &&
                  compile_failed.json().at("diagnostics")[0].at("file") ==
                      "Scripts/Gameplay.cpp" &&
                  compile_failed.log.find("fixture compile failure") != std::string::npos,
              "Failed compiler output retains raw log and structured source location");
        fs::remove(config.project_root / "emit-clang-error-and-fail-build");
        atomic_write(config.project_root / "flood-warnings-and-fail-build", "fixture\n");
        const auto flooded = builds.wait(builds.start_build());
        bool retained_error = false;
        for (const auto& diagnostic : flooded.diagnostics)
            retained_error |= diagnostic.value("severity", "") == "error" &&
                              diagnostic.value("file", "") == "Scripts/Gameplay.cpp";
        check(flooded.state == "failed" && flooded.diagnostics.size() <= 200 &&
                  retained_error && read_text(last_build) == previous_pointer,
              "Compiler errors survive bounded third-party warning floods");
        fs::remove(config.project_root / "flood-warnings-and-fail-build");
        atomic_write(config.project_root / "fail-unparseable-build", "fixture\n");
        const auto generic_failed = builds.wait(builds.start_build());
        check(generic_failed.state == "failed" &&
                  generic_failed.json().at("diagnostics").size() == 1 &&
                  generic_failed.json().at("diagnostics")[0].at("severity") == "error" &&
                  !generic_failed.json().at("diagnostics")[0].contains("file") &&
                  read_text(last_build) == previous_pointer,
              "Unparseable process failure has a non-navigable diagnostic");
        fs::remove(config.project_root / "fail-unparseable-build");
        atomic_write(config.project_root / "Scripts/Extensions/BuildOnly.hpp",
                     "#define BUILD_ONLY 3\n");
        atomic_write(config.project_root / "mutate-cpp-header-during-build", "fixture\n");
        const auto raced_header = builds.wait(builds.start_build());
        check(raced_header.state == "failed" && read_text(last_build) == previous_pointer,
              "Header changed during schema export cannot publish a mixed build");
        fs::remove(config.project_root / "mutate-cpp-header-during-build");
        atomic_write(config.project_root / "Scripts/Extensions/BuildOnly.hpp",
                     "#define BUILD_ONLY 1\n");
        check(!first.result.at("lua_enabled").get<bool>() &&
                  !fs::exists(directory / "project.faset.json"),
              "C++-only build publishes no Lua sources or project manifest");
        check(read_json(config.project_root / "configure-fixture.json").back() ==
                  "-DFASET_ENABLE_LUA=OFF",
              "C++-only build explicitly disables the Lua VM in CMake");
        authoring.replace_external_schemas(read_json(schema));
        const auto previous_registry = authoring.schemas().manifest();
        std::size_t published_generations{};
        for (const auto& entry : fs::directory_iterator(directory.parent_path())) {
            (void)entry;
            ++published_generations;
        }
        check(authoring.schemas().schema("game.mover").at("version") == 2,
              "Matching custom v2 metadata reaches authoring");
        migration_contracts(config.project_root / "migration-contracts", read_json(schema));

        Json scene{
            {"format", "faset.scene"},
            {"version", 1},
            {"id", "fixture-scene"},
            {"dimension", 2},
            {"instances", Json::array()},
            {"entities",
             Json::array({{{"id", "mover"},
                           {"components", Json::array({{{"id", "behavior"},
                                                        {"type", "game.mover"},
                                                        {"version", 2},
                                                        {"fields", {{"speed", 2.5}}}}})}}})}};
        check(builds.wait(builds.start_cook(scene)).state == "succeeded",
              "Matching component v2 cooks against the published schema");

        std::vector<Json> invalid;
        auto candidate = valid;
        candidate["types"][0]["fields"]["speed"]["default"] = "not a number";
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"][0]["fields"]["speed"]["type"] = "unsupported-kind";
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"][0]["fields"]["speed"]["min"] = "not a number";
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"][0]["fields"]["speed"]["enum"] = 2.5;
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"][0]["fields"]["speed"]["id"] = "different-field-id";
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"].push_back(candidate["types"][0]);
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"] = Json::array({authoring::builtin_schemas().schema("faset.transform")});
        invalid.push_back(candidate);
        for (const Json& version : {Json(0), Json(2.5), Json("2")}) {
            candidate = valid;
            candidate["types"][0]["version"] = version;
            invalid.push_back(candidate);
        }
        candidate = valid;
        candidate["types"][0].erase("fields");
        invalid.push_back(candidate);
        candidate = valid;
        candidate["types"] = Json::object();
        invalid.push_back(candidate);
        for (const auto& step : Json::array(
                 {{{"from_version", 2}, {"fields", Json::object()}},
                  {{"from_version", 0}, {"fields", Json::object()}},
                  {{"from_version", 1.5}, {"fields", Json::object()}},
                  {{"from_version", 1}, {"fields", {{"speed", {{"execute", "unsafe"}}}}}},
                  {{"from_version", 1}, {"fields", {{"speed", {{"scale", "bad"}}}}}},
                  {{"from_version", 1}, {"fields", {{"speed", {{"scale", nullptr}}}}}},
                  {{"from_version", 1}, {"fields", {{"speed", {{"require_manual", 1}}}}}},
                  {{"from_version", 1}, {"fields", Json::object()}, {"unsupported", true}}})) {
            candidate = valid;
            candidate["types"][0]["migrations"] = Json::array({step});
            invalid.push_back(candidate);
        }
        candidate = valid;
        candidate["types"][0]["migrations"].push_back(candidate["types"][0]["migrations"][0]);
        invalid.push_back(candidate);
        for (std::size_t index = 0; index < invalid.size(); ++index) {
            atomic_write_json(config.project_root / "schema-fixture.json", invalid[index]);
            atomic_write(config.project_root / "Scripts/Gameplay.cpp",
                         "// Invalid metadata fixture " + std::to_string(index));
            const auto failed = builds.wait(builds.start_build());
            check(failed.state == "failed" && failed.result.empty() && !failed.error.empty(),
                  "Malformed schema must fail before a successful build result is published");
            check(read_text(last_build) == previous_pointer,
                  "Last good build pointer is preserved");
            check(sha256_file(player) == previous_player && read_text(schema) == previous_schema &&
                      read_text(directory / "manifest.json") == previous_manifest,
                  "Last good binary, schema and build manifest remain unchanged");
            std::size_t generations{};
            for (const auto& entry : fs::directory_iterator(directory.parent_path())) {
                ++generations;
                check(!entry.path().filename().string().starts_with(".staging-"),
                      "Invalid build leaves no staging generation");
            }
            check(generations == published_generations,
                  "Invalid build publishes no new generation");
            bool rejected{};
            try {
                authoring.replace_external_schemas(invalid[index]);
            } catch (const std::exception&) {
                rejected = true;
            }
            check(rejected && authoring.schemas().manifest() == previous_registry,
                  "Cached-schema validation uses the same contract and preserves the registry");
        }
        atomic_write_json(config.project_root / "schema-fixture.json", valid);
        auto project = read_json(config.project_root / "project.faset.json");
        project["scripting"]["lua"]["scripts"] = Json::array({"Scripts/main.lua"});
        atomic_write_json(config.project_root / "project.faset.json", project);
        const auto lua_source =
            "return faset.behavior { id = 'game.mover', version = 2, fields = {} }\n";
        atomic_write(config.project_root / "Scripts/main.lua", lua_source);
        atomic_write(config.project_root / "Scripts/lib/util.lua", "return {value = 1}\n");
        fs::remove(config.project_root / "Scripts/Gameplay.cpp");
        fs::remove(config.project_root / "Scripts/Gameplay.hpp");
        const auto lua_build = builds.wait(builds.start_build());
        check(lua_build.state == "succeeded" && lua_build.result.at("lua_enabled") == true,
              "Lua-only project builds without a Gameplay.cpp/Gameplay.hpp pair: " +
                  lua_build.error);
        check(read_json(config.project_root / "configure-fixture.json").back() ==
                  "-DFASET_ENABLE_LUA=ON",
              "Lua declaration enables the module in the native Player");
        const auto lua_directory =
            path_from_utf8(lua_build.result.at("directory").get<std::string>());
        const auto captured = scripting::loadLuaProject(lua_directory);
        check(captured.enabled() && captured.sources.size() == 2 &&
                  captured.fingerprint ==
                      lua_build.result.at("lua_fingerprint").get<std::string>() &&
                  read_json(path_from_utf8(lua_build.result.at("schema").get<std::string>()))
                          .at("lua_fingerprint")
                          .get<std::string>() == captured.fingerprint,
              "Immutable source snapshot and merged schema share a Lua fingerprint");
        check(read_json(config.project_root / "exporter-project-fixture.json").at("scripting") ==
                  project.at("scripting"),
              "Schema exporter receives the captured project's Lua entries");
        atomic_write(config.project_root / "Scripts/lib/util.lua", "return {value = 2}\n");
        const auto changed = builds.wait(builds.start_build());
        check(
            changed.state == "succeeded" &&
                changed.result.at("fingerprint") != lua_build.result.at("fingerprint") &&
                changed.result.at("lua_fingerprint") != lua_build.result.at("lua_fingerprint"),
            "Module-only Lua edits produce new build provenance without changing native fixtures");
        check(scripting::loadLuaProject(lua_directory).fingerprint == captured.fingerprint,
              "Later edits never mutate an already published Lua generation");
        const auto lua_pointer = read_text(last_build);
        for (const auto* marker : {"mutate-lua-during-build", "mutate-lua-snapshot"}) {
            atomic_write(config.project_root / "Scripts/main.lua",
                         std::string(lua_source) + "-- force a schema export\n");
            atomic_write(config.project_root / marker, "fixture\n");
            const auto raced = builds.wait(builds.start_build());
            check(raced.state == "failed" && read_text(last_build) == lua_pointer,
                  "Source or snapshot changes during schema export preserve the last good build");
            fs::remove(config.project_root / marker);
            atomic_write(config.project_root / "Scripts/main.lua", lua_source);
        }
        const auto exported = builds.wait(builds.start_export(scene, root / "lua-export"));
        check(exported.state == "succeeded",
              "Lua export publishes a captured source package: " + exported.error);
        const auto packaged = path_from_utf8(exported.result.at("directory").get<std::string>());
        const auto package_manifest = read_json(packaged / "manifest.json");
        check(scripting::loadLuaProject(packaged).fingerprint ==
                      package_manifest.at("lua_fingerprint").get<std::string>() &&
                  package_manifest.at("lua_enabled") == true &&
                  read_json(packaged / "Notices/dependencies.json").contains("lua") &&
                  fs::is_regular_file(packaged / "Notices/lua/LICENSE.txt"),
              "Export contains source, fingerprint and the selected Lua runtime's license");
        check(!fs::exists(packaged / "schema.json") &&
                  !fs::exists(packaged / "faset_schema_exporter") &&
                  !fs::exists(packaged / "faset_schema_exporter.exe") &&
                  !fs::exists(packaged / ".luarc.json") &&
                  !fs::exists(packaged / "Scripts/Gameplay.cpp"),
              "Runtime export omits schema tools, editor configuration and C++ source");
        std::size_t packaged_sources{};
        for (const auto& file : package_manifest.at("files"))
            if (file.at("path").get<std::string>().starts_with("Scripts/")) {
                ++packaged_sources;
                check(sha256_file(packaged / path_from_utf8(file.at("path").get<std::string>())) ==
                          file.at("sha256").get<std::string>(),
                      "Every packaged source hash matches the export manifest");
            }
        check(packaged_sources == 2, "Entry and require module both appear in export provenance");
        project.erase("scripting");
        atomic_write_json(config.project_root / "project.faset.json", project);
        atomic_write(config.project_root / "Scripts/Gameplay.cpp", "// C++ fixture\n");
        atomic_write(config.project_root / "Scripts/Gameplay.hpp", "// C++ fixture\n");
        const auto cpp_export = builds.wait(builds.start_export(scene, root / "cpp-export"));
        check(cpp_export.state == "succeeded", "C++ export still succeeds: " + cpp_export.error);
        const auto cpp_package =
            path_from_utf8(cpp_export.result.at("directory").get<std::string>());
        check(!fs::exists(cpp_package / "Scripts") &&
                  !fs::exists(cpp_package / "project.faset.json") &&
                  !read_json(cpp_package / "Notices/dependencies.json").contains("lua") &&
                  read_json(config.project_root / "configure-fixture.json").back() ==
                      "-DFASET_ENABLE_LUA=OFF",
              "Removing Lua declarations drops scripts, notices and the Lua link dependency");
        std::cout << "Valid v2 schema and atomic rejection of " << invalid.size()
                  << " malformed metadata generations; Lua snapshots and export contracts passed\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, test_main);
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
