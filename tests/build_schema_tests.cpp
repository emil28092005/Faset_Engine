#include <faset/authoring/service.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/editor/build_service.hpp>
#include <iostream>

using namespace faset;
namespace fs = std::filesystem;
namespace {
void check(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
Json manifest() {
    return {
        {"format", "faset.schema"},
        {"version", 1},
        {"types",
         Json::array(
             {{{"id", "game.mover"},
               {"version", 2},
               {"fields",
                {{"speed", {{"type", "number"}, {"default", 2.5}, {"min", 0}, {"max", 10}}}}}}})}};
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
        authoring::AuthoringService authoring(config.project_root, authoring::builtin_schemas());
        const auto valid = manifest();
        atomic_write_json(config.project_root / "schema-fixture.json", valid);
        const auto first = builds.wait(builds.start_build());
        check(first.state == "succeeded", "Valid custom schema v2 publishes: " + first.error);
        const auto directory = path_from_utf8(first.result.at("directory").get<std::string>());
        const auto player = path_from_utf8(first.result.at("player").get<std::string>());
        const auto schema = path_from_utf8(first.result.at("schema").get<std::string>());
        const auto last_build = builds.config().cache_root / "last_build.json";
        const auto previous_pointer = read_text(last_build);
        const auto previous_player = sha256_file(player);
        const auto previous_schema = read_text(schema);
        const auto previous_manifest = read_text(directory / "manifest.json");
        authoring.replace_external_schemas(read_json(schema));
        const auto previous_registry = authoring.schemas().manifest();
        check(authoring.schemas().schema("game.mover").at("version") == 2,
              "Matching custom v2 metadata reaches authoring");

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
                check(entry.path() == directory, "Invalid build leaves no staging or generation");
            }
            check(generations == 1, "Only the validated build generation remains");
            bool rejected{};
            try {
                authoring.replace_external_schemas(invalid[index]);
            } catch (const std::exception&) {
                rejected = true;
            }
            check(rejected && authoring.schemas().manifest() == previous_registry,
                  "Cached-schema validation uses the same contract and preserves the registry");
        }
        std::cout << "Valid v2 schema and atomic rejection of " << invalid.size()
                  << " malformed metadata generations passed\n";
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
