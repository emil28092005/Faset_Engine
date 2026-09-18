#include <chrono>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
#include <set>
#include <thread>
using namespace faset;
namespace {
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void click(editor::EditorUI& ui, const std::string& id) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        const auto* w = ui.widgets().find(id);
        check(w, "Missing widget: " + id);
        const auto visible = w->rect.intersection(w->clip);
        if (visible.width > 2 && visible.height > 2) {
            render::Event down;
            down.type = render::Event::Type::MouseDown;
            down.button = 1;
            down.x = visible.x + visible.width * .5f;
            down.y = visible.y + visible.height * .5f;
            auto up = down;
            up.type = render::Event::Type::MouseUp;
            ui.frame({down, up});
            return;
        }
        auto* parent = w->parent;
        while (parent && !parent->layout.scroll)
            parent = parent->parent;
        check(parent, "Cannot scroll to " + id);
        render::Event move, wheel;
        move.type = render::Event::Type::MouseMove;
        move.x = parent->rect.x + 10;
        move.y = parent->rect.y + 10;
        wheel.type = render::Event::Type::Wheel;
        wheel.y = w->rect.y < parent->rect.y ? 1 : -1;
        ui.frame({move, wheel});
    }
    throw std::runtime_error("Widget remains clipped: " + id);
}
Json job(editor::Session& session, const std::string& id) {
    return session.commands().call("faset_job", {{"id", id}});
}
Json wait(editor::Session& session, editor::EditorUI& ui, const std::string& id) {
    for (int i = 0; i < 1000; ++i) {
        ui.frame({});
        const auto current = job(session, id);
        if (current.at("state") != "queued" && current.at("state") != "running")
            return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Import did not finish");
}
std::set<std::string> jobs(editor::Session& session) {
    std::set<std::string> result;
    const auto response = session.commands().call("faset_jobs", Json::object());
    for (const auto& value : response.at("jobs"))
        result.insert(value.at("id"));
    return result;
}
std::string new_job(editor::Session& session, const std::set<std::string>& before) {
    for (const auto& id : jobs(session))
        if (!before.contains(id))
            return id;
    throw std::runtime_error("UI did not submit an import job");
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("faset-import-ui-" + new_id());
    try {
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 900, "Import conflict workflow", true, true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        const auto source = root / "Assets/model.gltf";
        Json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", {0, 1}}}})},
            {"nodes",
             Json::array({{{"name", "Keep"}, {"extras", {{"faset_id", new_id()}}}},
                          {{"name", "Removed node"}, {"extras", {{"faset_id", new_id()}}}}})}};
        atomic_write_json(source, gltf);
        const auto initial_id = session.commands()
                                    .call("faset_import", {{"path", "Assets/model.gltf"},
                                                           {"settings", {{"units", "metres"}}}})
                                    .at("job")
                                    .get<std::string>();
        const auto initial = wait(session, ui, initial_id);
        check(initial.at("state") == "succeeded", "Fixture initial import: " + initial.dump());
        const auto asset_id = initial.at("result").at("asset_id").get<std::string>();
        const auto initial_generation = initial.at("result").at("generation");
        assets::AssetStore store(root / ".faset/cache");
        const auto document_before = session.authoring().query(ui.current_document());
        gltf["nodes"].erase(gltf["nodes"].end() - 1);
        gltf["scenes"][0]["nodes"] = Json::array({0});
        atomic_write_json(source, gltf);
        click(ui, "tab-assets");
        click(ui, "asset-refresh");
        click(ui, "file-Assets/model.gltf");
        auto before = jobs(session);
        click(ui, "asset-import");
        const auto conflict_id = new_job(session, before);
        auto conflict = wait(session, ui, conflict_id);
        check(conflict.at("state") == "conflict", "Deleted output must remain a conflict");
        click(ui, "tab-jobs");
        click(ui, "job-review-" + conflict_id);
        const auto group = "import-conflict-" + conflict_id;
        const auto removed_id = conflict["result"]["removed_output_ids"][0].get<std::string>();
        const auto* output = ui.widgets().find(group + "-output-" + removed_id);
        check(output && output->text.find("Removed node") != std::string::npos,
              "Review lists exact removed identity and its last-good name");
        check(store.current_manifest(asset_id).at("generation") == initial_generation,
              "Showing a conflict must not accept the removal");
        renderer.render(ui.snapshot());
        renderer.capture(root / "import-removal-review.ppm");
        gltf["nodes"][0]["translation"] = {1, 0, 0};
        atomic_write_json(source, gltf);
        before = jobs(session);
        click(ui, group + "-accept");
        const auto stale_id = new_job(session, before);
        const auto stale = wait(session, ui, stale_id);
        check(stale.at("state") == "conflict" &&
                  store.current_manifest(asset_id).at("generation") == initial_generation,
              "Changed source refuses reviewed acceptance and keeps the old generation: " +
                  stale.dump());
        click(ui, "tab-conflicts");
        const auto stale_group = "import-conflict-" + stale_id;
        check(ui.widgets().find(stale_group + "-diagnostic")->text.find("changed since review") !=
                  std::string::npos,
              "Stale acceptance exposes the changed-generation diagnostic and new review");
        before = jobs(session);
        click(ui, stale_group + "-retry");
        const auto fresh_id = new_job(session, before);
        const auto fresh = wait(session, ui, fresh_id);
        check(fresh.at("state") == "conflict", "Reimport creates a fresh removal review");
        click(ui, "tab-jobs");
        click(ui, "job-review-" + fresh_id);
        check(!ui.widgets().find(group), "New conflict supersedes the explicitly retried review");
        before = jobs(session);
        click(ui, "import-conflict-" + fresh_id + "-accept");
        const auto accepted_id = new_job(session, before);
        const auto accepted = wait(session, ui, accepted_id);
        check(accepted.at("state") == "succeeded", "Explicit reviewed removal must publish");
        check(store.current_manifest(asset_id).at("generation") == fresh["result"]["generation"],
              "UI publishes exactly the reviewed candidate");
        check(store.current_manifest(asset_id).at("settings").at("units") == "metres",
              "Retry preserves the saved import settings");
        click(ui, "tab-conflicts");
        check(ui.widgets().find("conflicts-empty"),
              "Resolved import conflicts leave the review list");
        check(session.authoring().query(ui.current_document()) == document_before,
              "Import approval never rewrites the authoring scene or its Undo revision");
        renderer.render(ui.snapshot());
        check(renderer.stats().validation_errors == 0, "Vulkan validation");
        std::cout << "Import UI: removed IDs/names, stale review rejection, explicit retry and "
                     "exact-generation acceptance passed. "
                  << path_to_utf8(root) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nRetained: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
