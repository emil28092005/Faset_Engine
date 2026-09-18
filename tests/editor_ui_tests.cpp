#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
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
int main() {
    auto root = std::filesystem::temp_directory_path() / ("faset-ui-authoring-" + new_id());
    try {
        std::filesystem::create_directories(root);
        atomic_write_json(root / "project.faset.json", {{"format", "faset.project"},
                                                        {"version", 1},
                                                        {"name", "UI integration test"},
                                                        {"dimension", 3}});
#if defined(FASET_TEST_PLUGIN_DIRECTORY)
        std::filesystem::create_directories(root / "Plugins");
        for (const auto& file : std::filesystem::directory_iterator(FASET_TEST_PLUGIN_DIRECTORY))
            if (file.is_regular_file())
                std::filesystem::copy_file(file.path(), root / "Plugins" / file.path().filename());
#endif
        editor::Session session({root, FASET_TEST_ENGINE, root});
        render::Renderer renderer({1280, 800, "Faset editor test", true, true});
        editor::EditorUI ui(session, renderer,
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/ui/dark.json");
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
        renderer.render(ui.snapshot());
        renderer.capture(root / "editor-ui.ppm");
        check(renderer.stats().validation_errors == 0, "Vulkan validation errors");
        std::cout << "Editor UI: actual events create/select/rename/typed "
                     "fields/Undo/Redo/conflict/one drag transaction passed. "
                     "Screenshot: "
                  << (root / "editor-ui.ppm") << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
