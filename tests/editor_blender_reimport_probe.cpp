#include <chrono>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
#include <thread>

using namespace faset;
namespace fs = std::filesystem;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
Json geometry(const render::DrawItem& draw) {
    Json positions = Json::array();
    for (const auto& vertex : draw.mesh->vertices)
        positions.push_back(vertex.position);
    return positions;
}
int run(int argc, char** argv) {
    try {
        check(argc == 3, "Usage: faset_blender_editor_probe FIXTURE_DIRECTORY NEW_PROJECT");
        const auto fixture = path_from_utf8(argv[1]), project = path_from_utf8(argv[2]);
        check(!fs::exists(project), "Use a new project directory for the Blender UI probe");
        const auto source = project / "Assets/door";
        fs::create_directories(source);
        editor::Session session({project, path_from_utf8(FASET_TEST_ENGINE), project});
        render::Renderer renderer({.width = 1280,
                                   .height = 900,
                                   .title = "Blender reimport",
                                   .headless = true,
                                   .validation = true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        auto import = [&](const char* stage) {
            fs::copy(fixture / stage, source,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            const auto job = session.commands()
                                 .call("faset_import", {{"path", "Assets/door/manifest.json"}})
                                 .at("job");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < deadline) {
                ui.frame({});
                auto result = session.commands().call("faset_job", {{"id", job}});
                if (result.at("state") != "queued" && result.at("state") != "running")
                    return result;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            throw std::runtime_error("Blender import timed out");
        };
        const auto initial = import("initial");
        check(initial.at("state") == "succeeded", "Initial Blender import failed");
        const auto id = initial.at("result").at("asset_id");
        const auto document = ui.current_document();
        auto current = session.authoring().query(document);
        Json operations = Json::array();
        for (int index = 0; index < 2; ++index) {
            auto entity = authoring::make_entity(session.authoring().schemas(), "Placed door");
            entity["components"][0]["fields"]["position"] = {index ? 3 : -3, 0, 0};
            entity["components"].push_back(
                {{"id", new_id()},
                 {"type", "faset.mesh"},
                 {"version", 1},
                 {"fields",
                  {{"asset", id},
                   {"primitive", "asset"},
                   {"color", index ? Json{0.5, 0.8, 0.3, 1} : Json{0.3, 0.5, 0.9, 1}}}}});
            // Missing gameplay packages stay opaque; reimport must not remove them.
            entity["components"].push_back({{"id", new_id()},
                                            {"type", "project.door"},
                                            {"version", 2},
                                            {"fields", {{"locked", index == 0}, {"speed", 2.5}}}});
            auto body = session.authoring().schemas().default_fields("faset.rigid_body_3d");
            body["body_type"] = "static";
            body["friction"] = index ? 0.75 : 0.25;
            entity["components"].push_back({{"id", new_id()},
                                            {"type", "faset.rigid_body_3d"},
                                            {"version", 1},
                                            {"fields", body}});
            operations.push_back({{"op", "entity.create"}, {"entity", entity}});
        }
        session.authoring().transact(document, current.at("revision"), operations);
        current = session.authoring().query(document);
        ui.frame({});
        check(ui.snapshot().draws.size() >= 2, "Both placed Blender meshes must render");
        const auto first = ui.snapshot().draws[0], second = ui.snapshot().draws[1];
        check(first.mesh == second.mesh && first.model != second.model,
              "Two independent placements must share the imported mesh");
        renderer.render(ui.snapshot());
        renderer.capture(project / "initial.ppm");
        const auto renamed = import("renamed");
        check(renamed.at("state") == "succeeded", "Renamed Blender import failed");
        // Exercise the real editor's generation watcher, without manually clearing SceneView.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        ui.frame({});
        const auto first_after = ui.snapshot().draws[0], second_after = ui.snapshot().draws[1];
        check(geometry(first) != geometry(first_after) &&
                  geometry(second) != geometry(second_after),
              "Both live Editor instances must receive changed Blender geometry");
        check(first_after.mesh == second_after.mesh, "New instances share one cooked mesh");
        check(first.model == first_after.model && second.model == second_after.model &&
                  first.color == first_after.color && second.color == second_after.color,
              "Reimport must preserve each instance's placement and tint");
        check(session.authoring().query(document) == current,
              "Reimport must preserve authoring IDs, gameplay fields and revision");
        renderer.render(ui.snapshot());
        renderer.capture(project / "renamed.ppm");
        const auto removed = import("removed");
        check(removed.at("state") == "conflict", "Removed Blender output must conflict");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        ui.frame({});
        check(geometry(ui.snapshot().draws[0]) == geometry(first_after) &&
                  geometry(ui.snapshot().draws[1]) == geometry(second_after),
              "Conflict must keep both last-good visible meshes");
        check(session.authoring().query(document) == current,
              "Import conflict must preserve authoring");
        check(renderer.stats().validation_errors == 0, "Vulkan validation failed");
        atomic_write_json(project / "probe.json",
                          {{"status", "passed"},
                           {"instances", 2},
                           {"live_geometry_updated", true},
                           {"placement_and_tint_preserved", true},
                           {"opaque_gameplay_preserved", true},
                           {"physics_fields_preserved", true},
                           {"conflict_keeps_visible_generation", true},
                           {"validation_enabled", renderer.stats().validation_enabled},
                           {"validation_errors", renderer.stats().validation_errors}});
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, run);
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
