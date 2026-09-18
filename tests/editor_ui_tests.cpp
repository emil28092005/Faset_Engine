#include "assets_image_fixtures.hpp"
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <fstream>
#include <iostream>
#include <thread>
using namespace faset;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
render::Event key(std::string value, bool control = false, bool shift = false) {
    render::Event e;
    e.type = render::Event::Type::KeyDown;
    e.key = std::move(value);
    e.control = control;
    e.shift = shift;
    return e;
}
void click(editor::EditorUI& ui, const std::string& id) {
    const auto* widget = ui.widgets().find(id);
    check(widget != nullptr, "Missing widget");
    const auto rect = widget->rect.intersection(widget->clip);
    check(rect.width > 0 && rect.height > 0, "Widget clipped");
    render::Event down;
    down.type = render::Event::Type::MouseDown;
    down.button = 1;
    down.x = rect.x + rect.width * .5f;
    down.y = rect.y + rect.height * .5f;
    auto up = down;
    up.type = render::Event::Type::MouseUp;
    ui.frame({down, up});
}
void text(editor::EditorUI& ui, const std::string& id, const std::string& value,
          bool commit = true) {
    click(ui, id);
    render::Event e;
    e.type = render::Event::Type::TextInput;
    e.text = value;
    std::vector<render::Event> events{key("A", true), e};
    if (commit)
        events.push_back(key("Return"));
    ui.frame(events);
}
void migration_workflow(render::Renderer& renderer, const std::filesystem::path& root) {
    editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
    const auto gameplay_manifest = Json::parse(R"({"types":[
        {"id":"test.migratable","version":2,"name":"Movement","fields":{
          "speed":{"id":"speed","type":"number","default":1.0},
          "enabled":{"id":"enabled","type":"bool","default":true}},
          "migrations":[{"from_version":1,"fields":{"speed":{"scale":0.01},"enabled":{"default":true}}}]},
        {"id":"test.no_migration","version":2,"fields":{
          "speed":{"id":"speed","type":"number","default":1.0}}}
    ]})");
    session.authoring().replace_external_schemas(gameplay_manifest);
    auto object = authoring::make_entity(session.authoring().schemas(), "Old movement");
    const auto cid = new_id();
    object["components"].push_back({{"id", cid},
                                    {"type", "test.migratable"},
                                    {"version", 1},
                                    {"fields", {{"speed", 250.0}, {"unknown", "retained"}}}});
    const auto old = object["components"].back();
    auto source_scene = authoring::make_scene("Old source");
    source_scene["entities"].push_back(object);
    atomic_write_json(root / "Assets/old-source.fscene", source_scene);
    const auto doc = session.authoring().create("Migration test").at("id").get<std::string>();
    const auto instance = new_id();
    session.authoring().transact(
        doc, 0,
        Json::array({{{"op", "entity.create"}, {"entity", object}},
                     {{"op", "template.instance"},
                      {"instance", {{"id", instance}, {"source", "Assets/old-source.fscene"}}}}}));
    session.authoring().save(doc, "Scenes/migration.fscene");
    editor::EditorUI ui(session, renderer,
                        path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                        path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
    ui.frame({});
    // Explicit keyboard activation also exercises focus reveal in a long Inspector.
    auto activate = [&](const std::string& id) {
        check(ui.widgets().focus(id), "Migration workflow action must be focusable");
        ui.frame({key("Return")});
    };
    ui.select_entity(object.at("id"));
    ui.frame({});
    check(ui.widgets().find("opaque-fields-" + cid) &&
              !ui.widgets().find("opaque-fields-" + cid)->enabled,
          "Older component opens preserved and read-only before explicit migration");
    const auto before = session.authoring().query(doc);
    activate("component-migrate-" + cid);
    auto after = session.authoring().query(doc);
    const auto migrated = after["scene"]["entities"][0]["components"].back();
    check(after["revision"] == before["revision"].get<std::uint64_t>() + 1 &&
              migrated["id"] == cid && migrated["version"] == 2 &&
              migrated["fields"]["speed"] == 2.5 && migrated["fields"]["enabled"] == true &&
              migrated["fields"]["unknown"] == "retained",
          "Inspector Migrate applies declared rules atomically while preserving identity and data");
    check(ui.widgets().find("field-" + cid + "-speed-value") &&
              !ui.widgets().find("component-migrate-" + cid),
          "Successful migration replaces opaque presentation with typed fields");
    activate("undo");
    check(session.authoring().query(doc)["scene"]["entities"][0]["components"].back() == old &&
              ui.widgets().find("component-migrate-" + cid),
          "One Undo restores the original version and opaque Inspector");
    auto unresolved = object;
    unresolved["id"] = new_id();
    unresolved["name"] = "Missing rule";
    for (auto& c : unresolved["components"])
        c["id"] = new_id();
    unresolved["components"].back()["type"] = "test.no_migration";
    const auto unresolved_cid = unresolved["components"].back()["id"].get<std::string>();
    auto local = object;
    local["id"] = new_id();
    for (auto& c : local["components"])
        c["id"] = new_id();
    after = session.authoring().query(doc);
    session.authoring().transact(
        doc, after["revision"],
        Json::array({{{"op", "entity.create"}, {"entity", unresolved}},
                     {{"op", "template.add"}, {"instance", instance}, {"value", local}}}));
    ui.select_entity(unresolved.at("id"));
    ui.frame({});
    const auto failed_before = session.authoring().query(doc);
    activate("component-migrate-" + unresolved_cid);
    check(session.authoring().query(doc) == failed_before &&
              ui.widgets().find("status")->text.find("migration") != std::string::npos,
          "Missing migration reports an actionable error without revision or source changes");
    auto resolved = session.commands().resolved_scene(doc).at("scene").at("entities");
    for (const auto& e : resolved) {
        if (!e.contains("origin") || e["origin"].value("path", Json::array()).empty())
            continue;
        if (e["origin"].value("local", false)) {
            ui.select_entity(e.at("id"));
            ui.frame({});
            activate("component-migrate-" + e["components"].back()["id"].get<std::string>());
            const auto record =
                session.authoring().query(doc)["scene"]["instances"][0]["additions"][0];
            check(
                record["id"] == local["id"] &&
                    record["components"].back()["id"] == local["components"].back()["id"] &&
                    record["components"].back()["version"] == 2,
                "Local-addition migration addresses original source IDs, not resolved runtime IDs");
            activate("undo");
            check(session.authoring().query(doc)["scene"]["instances"][0]["additions"][0] == local,
                  "Local-addition migration is one undoable transaction");
        }
    }
    for (const auto& e : resolved) {
        if (!e.contains("origin") || e["origin"].value("path", Json::array()).empty() ||
            e["origin"].value("local", false))
            continue;
        ui.select_entity(e.at("id"));
        ui.frame({});
        const auto action = "component-migrate-" + e["components"].back()["id"].get<std::string>();
        check(ui.widgets().find(action)->text == "Open source to migrate",
              "Inherited migration explicitly navigates to the owning source");
        const auto main_before = session.authoring().query(doc);
        activate(action);
        check(ui.current_document() == source_scene.at("id").get<std::string>() &&
                  session.authoring().query(doc) == main_before &&
                  session.authoring()
                          .query(ui.current_document())["scene"]["entities"][0]["components"]
                          .back() == old &&
                  ui.widgets().find("component-migrate-" + cid),
              "Opening source keeps old data unchanged and offers explicit migration there");
        break;
    }
    // A metadata rebuild must invalidate the cached resolved scene even when
    // neither the document nor its open source has changed revision.
    auto v1 = gameplay_manifest;
    v1["types"][0]["version"] = 1;
    v1["types"][0].erase("migrations");
    v1["types"][0]["fields"].erase("enabled");
    session.authoring().replace_external_schemas(v1);
    auto main = session.authoring().query(doc);
    session.authoring().transact(doc, main.at("revision"),
                                 Json::array({{{"op", "template.override"},
                                               {"instance", instance},
                                               {"address",
                                                {{"path", Json::array()},
                                                 {"object", object.at("id")},
                                                 {"component", cid},
                                                 {"field", "speed"}}},
                                               {"value", 900.0}}}));
    ui.select_document(doc);
    std::string inherited_id, inherited_cid;
    for (const auto& e : resolved)
        if (e.contains("origin") && !e["origin"].value("path", Json::array()).empty() &&
            !e["origin"].value("local", false)) {
            inherited_id = e.at("id");
            inherited_cid = e["components"].back().at("id");
            break;
        }
    check(!inherited_id.empty(), "Migration schema-cache fixture has an inherited object");
    ui.select_entity(inherited_id);
    ui.frame({});
    const auto field_id = "field-" + inherited_cid + "-speed-value";
    check(ui.widgets().find(field_id) && ui.widgets().find(field_id)->value == 900.0 &&
              !ui.widgets().find("conflict-0-text"),
          "V1 schema applies and caches the inherited field override");
    const auto documents_before = session.authoring().documents();
    session.authoring().replace_external_schemas(gameplay_manifest);
    ui.frame({});
    const auto* conflict = ui.widgets().find("conflict-0-text");
    check(session.authoring().documents() == documents_before && conflict &&
              conflict->text.starts_with("override.schema_version") &&
              !ui.widgets().find(field_id) && ui.widgets().find("opaque-fields-" + inherited_cid),
          "Schema-only rebuild refreshes resolved conflicts and opaque fields without authoring "
          "edits");
    session.authoring().replace_external_schemas(v1);
    ui.frame({});
    check(!ui.widgets().find("conflict-0-text") && ui.widgets().find(field_id) &&
              ui.widgets().find(field_id)->value == 900.0,
          "Restoring compatible metadata clears cached conflicts and reapplies retained override");
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() / path_from_utf8("faset-ui-проект-" + new_id());
    try {
        std::filesystem::create_directories(root);
        atomic_write_json(root / "project.faset.json", {{"format", "faset.project"},
                                                        {"version", 1},
                                                        {"name", "UI integration test"},
                                                        {"dimension", 3}});
#if defined(FASET_TEST_PLUGIN_DIRECTORY)
        std::filesystem::create_directories(root / "Plugins");
        for (const auto& file :
             std::filesystem::directory_iterator(path_from_utf8(FASET_TEST_PLUGIN_DIRECTORY)))
            if (file.is_regular_file())
                std::filesystem::copy_file(file.path(), root / "Plugins" / file.path().filename());
#endif
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 800, "Faset editor test", true, true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        ui.frame({});
        click(ui, "add-cube");
        auto state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"].size() == 1, "Add cube button must author entity");
        const auto eid = state["scene"]["entities"][0]["id"].get<std::string>();
        check(ui.selected_entity() == eid, "New entity selected");
        text(ui, "object-name", "Дверь");
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"][0]["name"] == "Дверь", "Inspector Unicode rename");
        const auto cid = state["scene"]["entities"][0]["components"][0]["id"].get<std::string>();
        const auto position = "field-" + cid + "-position-0";
        text(ui, position, "3.5");
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"][0]["components"][0]["fields"]["position"][0] == 3.5,
              "Inspector typed field transaction");
        click(ui, "undo");
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"][0]["components"][0]["fields"]["position"][0] == 0,
              "Toolbar Undo uses authoring history");
        click(ui, "redo");
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"][0]["components"][0]["fields"]["position"][0] == 3.5,
              "Toolbar Redo");
        // A concurrent MCP edit cannot be overwritten by an unfinished inspector
        // edit.
        text(ui, "object-name", "Unsaved local name", false);
        state = session.authoring().query(ui.current_document());
        session.authoring().transact(
            ui.current_document(), state.at("revision"),
            Json::array({{{"op", "entity.rename"}, {"entity", eid}, {"name", "External rename"}}}));
        ui.frame({key("Return")});
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"][0]["name"] == "External rename",
              "Revision conflict preserves external edit");
        click(ui, "scene-root");
        check(ui.selected_entity().empty(), "Scene root deselects object");
        click(ui, "entity-" + eid);
        check(ui.selected_entity() == eid, "Scene tree selection");
        const auto initial_revision = state.at("revision").get<std::uint64_t>();
        auto* field = ui.widgets().find(position);
        const auto r = field->rect;
        render::Event down;
        down.type = render::Event::Type::MouseDown;
        down.button = 1;
        down.x = r.x + 20;
        down.y = r.y + 12;
        auto move = down;
        move.type = render::Event::Type::MouseMove;
        move.x += 20;
        auto move2 = move;
        move2.x += 20;
        auto up = move2;
        up.type = render::Event::Type::MouseUp;
        ui.frame({down, move, move2, up});
        state = session.authoring().query(ui.current_document());
        check(state["revision"] == initial_revision + 1,
              "Numeric drag commits exactly one transaction");
        // Selecting and manipulating the actual rendered geometry uses viewport
        // events.
        auto screen = [&](render::Vec3 position) {
            const auto& snap = ui.snapshot();
            const auto& m = snap.view_projection;
            const auto w = m[3] * position[0] + m[7] * position[1] + m[11] * position[2] + m[15];
            const auto nx =
                (m[0] * position[0] + m[4] * position[1] + m[8] * position[2] + m[12]) / w;
            const auto ny =
                (m[1] * position[0] + m[5] * position[1] + m[9] * position[2] + m[13]) / w;
            return render::Vec2{snap.scene_rect[0] + (nx + 1) * snap.scene_rect[2] * .5f,
                                snap.scene_rect[1] + (ny + 1) * snap.scene_rect[3] * .5f};
        };
        const auto x =
            state["scene"]["entities"][0]["components"][0]["fields"]["position"][0].get<float>();
        ui.select_entity("");
        ui.frame({});
        auto center = screen({x, 0, 0});
        down.x = center[0];
        down.y = center[1];
        up = down;
        up.type = render::Event::Type::MouseUp;
        ui.frame({down, up});
        check(ui.selected_entity() == eid, "Viewport ray must select visible cooked geometry");
        auto tip = screen({x + 1.44f, 0, 0});
        down.x = (center[0] + tip[0]) * .5f;
        down.y = (center[1] + tip[1]) * .5f;
        move = down;
        move.type = render::Event::Type::MouseMove;
        move.x += 20;
        up = move;
        up.type = render::Event::Type::MouseUp;
        const auto before_gizmo = state.at("revision").get<std::uint64_t>();
        ui.frame({down});
        ui.frame({move});
        check(session.authoring().query(ui.current_document())["revision"] == before_gizmo,
              "Gizmo preview must not mutate authoring");
        ui.frame({up});
        state = session.authoring().query(ui.current_document());
        check(state["revision"] == before_gizmo + 1, "Gizmo release commits one transaction");
        click(ui, "undo");
        state = session.authoring().query(ui.current_document());
        check(std::abs(state["scene"]["entities"][0]["components"][0]["fields"]["position"][0]
                           .get<float>() -
                       x) < .001f,
              "Gizmo undo restores transform");
#if defined(FASET_TEST_PLUGIN_DIRECTORY)
        session.authoring().register_schemas(Json::parse(
            R"([{"id":"example.beacon","name":"Beacon","version":1,"fields":{"speed":{"id":"speed","name":"Rotation speed","type":"number","default":1.0}}}])"));
        ui.frame({});
        check(!session.plugin_panels().empty(), "Actual native plugin panel must load");
        const auto before_plugin = state["scene"]["entities"].size();
        click(ui, "tab-plugin-example.beacon.tools");
        click(ui, "plugin-action-example.beacon.tools");
        state = session.authoring().query(ui.current_document());
        check(state["scene"]["entities"].size() == before_plugin + 1,
              "Plugin panel action must author Beacon through Commands");
        click(ui, "tab-assets");
#endif
        std::filesystem::create_directories(root / "Assets");
        const auto image_path = root / path_from_utf8("Assets/Два пикселя.png");
        {
            std::ofstream stream(image_path, std::ios::binary);
            const auto& png = faset::test_images::png_red_green;
            stream.write(reinterpret_cast<const char*>(png.data()),
                         static_cast<std::streamsize>(png.size()));
        }
        const auto import_job =
            session.commands()
                .call("faset_import", {{"path", "Assets/Два пикселя.png"},
                                       {"settings", {{"pixels_per_unit", 1.0}}}})
                .at("job")
                .get<std::string>();
        Json imported;
        for (int wait = 0; wait < 500; ++wait) {
            session.poll();
            imported = session.commands().call("faset_job", {{"id", import_job}});
            if (imported.at("state") != "queued" && imported.at("state") != "running")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(imported.at("state") == "succeeded", "Actual asynchronous PNG importer job");
        click(ui, "asset-refresh");
        ui.frame({});
        const auto image_id = imported.at("result").at("asset_id").get<std::string>();
        auto* image_row = ui.widgets().find("asset-" + image_id);
        check(image_row, "Imported image in asset browser");
        check(image_row->text.find("Два пикселя") != std::string::npos,
              "Asset browser displays Unicode source filename");
        const auto asset_rect = image_row->rect.intersection(image_row->clip);
        const auto viewport_rect = ui.widgets().find("viewport")->rect;
        down.x = asset_rect.x + 100;
        down.y = asset_rect.y + 12;
        move = down;
        move.type = render::Event::Type::MouseMove;
        move.x = viewport_rect.x + viewport_rect.width * .5f;
        move.y = viewport_rect.y + viewport_rect.height * .5f;
        up = move;
        up.type = render::Event::Type::MouseUp;
        ui.frame({down, move, up});
        state = session.authoring().query(ui.current_document());
        const auto& sprite = state["scene"]["entities"].back()["components"].back();
        check(sprite["type"] == "faset.sprite" && sprite["fields"]["texture"] == image_id,
              "Image drag/drop must author Sprite with AssetId");
        check(sprite["fields"]["size"] == Json::array({2.0, 1.0}),
              "Image drag/drop preserves dimensions through pixels_per_unit");
        check(!ui.snapshot().sprites.empty() && ui.snapshot().sprites.back().texture &&
                  ui.snapshot().sprites.back().texture->width == 2,
              "PNG sprite must decode into actual render texture");
        const auto before_switch = state.at("revision");
        click(ui, "menu-File");
        click(ui, "open-project");
        check(ui.widgets().find("project-switch-dialog")->visible && !ui.project_switch_requested(),
              "Dirty project switch requires an explicit recovery warning action");
        ui.frame({key("Delete")});
        check(session.authoring().query(ui.current_document()).at("revision") == before_switch,
              "Switch dialog captures editor shortcuts");
        click(ui, "project-switch-cancel");
        check(!ui.project_switch_requested(), "Cancelling project switch keeps editor open");
        ui.set_project_switch_enabled(false);
        click(ui, "menu-File");
        check(!ui.widgets().find("open-project")->enabled,
              "Project switch disabled while an MCP client owns the session");
        click(ui, "open-project");
        check(!ui.project_switch_requested(), "Disabled project switch cannot request exit");
        ui.set_project_switch_enabled(true);
        ui.frame({});
        click(ui, "open-project");
        click(ui, "project-switch-continue");
        check(ui.project_switch_requested(), "Confirmed project switch is exposed to application");
        renderer.render(ui.snapshot());
        renderer.capture(root / "editor-ui.ppm");
        migration_workflow(renderer, root / "migration-workflow");
        check(renderer.stats().validation_errors == 0, "Vulkan validation errors");
        std::cout << "Editor UI: actual events create/select/rename/typed "
                     "fields/Undo/Redo/conflict/one drag transaction passed. "
                     "Screenshot: "
                  << path_to_utf8(root / "editor-ui.ppm") << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
