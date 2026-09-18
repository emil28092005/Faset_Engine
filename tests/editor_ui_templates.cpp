#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
using namespace faset;
namespace {
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
render::Event key(std::string name, bool control = false) {
    render::Event e;
    e.type = render::Event::Type::KeyDown;
    e.key = std::move(name);
    e.control = control;
    return e;
}
void click(editor::EditorUI& editor, const std::string& id) {
    for (int attempts = 0; attempts < 30; ++attempts) {
        auto* widget = editor.widgets().find(id);
        check(widget, "Missing widget: " + id);
        const auto visible = widget->rect.intersection(widget->clip);
        if (visible.width > 2 && visible.height > 2) {
            render::Event down;
            down.type = render::Event::Type::MouseDown;
            down.button = 1;
            down.x = visible.x + visible.width * .5f;
            down.y = visible.y + visible.height * .5f;
            auto up = down;
            up.type = render::Event::Type::MouseUp;
            editor.frame({down, up});
            return;
        }
        auto* parent = widget->parent;
        while (parent && !parent->layout.scroll)
            parent = parent->parent;
        check(parent, "Widget is hidden: " + id);
        render::Event move;
        move.type = render::Event::Type::MouseMove;
        move.x = parent->rect.x + 20;
        move.y = parent->rect.y + 20;
        render::Event wheel;
        wheel.type = render::Event::Type::Wheel;
        wheel.y = widget->rect.y < parent->rect.y ? 2 : -2;
        editor.frame({move, wheel});
    }
    throw std::runtime_error("Cannot scroll to widget: " + id);
}
void text(editor::EditorUI& editor, const std::string& id, const std::string& value) {
    click(editor, id);
    render::Event input;
    input.type = render::Event::Type::TextInput;
    input.text = value;
    editor.frame({key("A", true), input, key("Return")});
}
Json query(editor::Session& session, editor::EditorUI& editor) {
    return session.authoring().query(editor.current_document());
}
Json leaf(editor::Session& session, const std::string& document, std::size_t depth) {
    const auto resolved = session.commands().resolved_scene(document);
    for (const auto& e : resolved.at("scene").at("entities"))
        if (e.at("origin").at("path").size() == depth)
            return e;
    throw std::runtime_error("Resolved template leaf missing");
}
std::string position_field(const Json& object) {
    return "field-" + object["components"][0]["id"].get<std::string>() + "-position-0";
}
float position(const Json& object) {
    return object["components"][0]["fields"]["position"][0].get<float>();
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("faset-template-ui-" + new_id());
    try {
        editor::Session session({root, FASET_TEST_ENGINE, root});
        render::Renderer renderer({1280, 900, "Template workflow test", true, true});
        editor::EditorUI ui(session, renderer,
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        ui.frame({});
        const auto main = ui.current_document();
        click(ui, "add-cube");
        text(ui, "object-name", "Door");
        const auto local = ui.selected_entity();
        click(ui, "menu-Scene");
        text(ui, "template-path", "Assets/Templates/Door.scene.json");
        click(ui, "save-template");
        check(std::filesystem::exists(root / "Assets/Templates/Door.scene.json"),
              "Manual source template creation");
        for (int i = 0; i < 2; ++i) {
            click(ui, "menu-Scene");
            click(ui, "instance-template");
        }
        check(query(session, ui)["scene"]["instances"].size() == 2,
              "Two instances from manual Scene menu");
        ui.select_entity(local);
        ui.frame({});
        ui.frame({key("Delete")});
        check(query(session, ui)["scene"]["entities"].empty(),
              "Delete local original independently of template");
        click(ui, "menu-File");
        click(ui, "new-3d");
        const auto outer_document = ui.current_document();
        click(ui, "menu-Scene");
        click(ui, "instance-template");
        click(ui, "menu-File");
        text(ui, "save-path", "Assets/Templates/Outer.scene.json");
        click(ui, "save-as-button");
        click(ui, "back-document");
        check(ui.current_document() == main, "Open source/back document navigation");
        click(ui, "menu-Scene");
        text(ui, "template-path", "Assets/Templates/Outer.scene.json");
        click(ui, "instance-template");
        auto nested = leaf(session, main, 2);
        const auto nested_id = nested.at("id").get<std::string>();
        click(ui, "entity-" + nested_id);
        text(ui, position_field(nested), "5");
        auto state = query(session, ui);
        const auto& record = state["scene"]["instances"].back()["overrides"][0];
        check(record["address"]["path"].size() == 1, "Nested override uses relative instance path");
        check(position(leaf(session, main, 2)) == 5, "Nested Inspector override");
        const auto revert =
            "field-" + nested["components"][0]["id"].get<std::string>() + "-position-revert";
        check(ui.widgets().find(revert)->enabled, "Override provenance enables Revert");
        click(ui, revert);
        check(position(leaf(session, main, 2)) == 0, "Revert reads source value");
        click(ui, "object-open-source");
        check(ui.current_document() != main && ui.current_document() != outer_document,
              "Nested Open source reaches inner source");
        text(ui, "object-name", "Renamed Door");
        auto source = query(session, ui)["scene"]["entities"][0];
        text(ui, position_field(source), "2");
        click(ui, "back-document");
        check(ui.current_document() == main, "Back returns to instance scene");
        nested = leaf(session, main, 2);
        check(nested["id"] == nested_id && nested["name"] == "Renamed Door" &&
                  position(nested) == 2,
              "Dirty open source propagates while identity survives rename");
        click(ui, "entity-" + nested_id);
        ui.frame({key("Delete")});
        state = query(session, ui);
        const auto top = state["scene"]["instances"].back()["id"];
        check(state["scene"]["instances"].back()["suppressed"].size() == 1,
              "Delete inherited object records suppression");
        click(ui, "instance-" + Json::array({top}).dump());
        click(ui, "instance-restore-0");
        check(leaf(session, main, 2)["id"] == nested_id, "Restore suppression keeps identity");
        click(ui, "instance-add-local");
        const auto additions = session.commands().resolved_scene(main).at("scene").at("entities");
        std::string addition_id;
        for (const auto& object : additions)
            if (object.at("origin").value("local", false) &&
                object.at("origin").at("path").size() == 1)
                addition_id = object.at("id");
        check(!addition_id.empty(), "Instance-local addition is resolved");
        click(ui, "entity-" + addition_id);
        text(ui, "object-name", "Local lamp");
        click(ui, "add-component");
        click(ui, "component-choice-faset.mesh");
        const auto addition = query(session, ui)["scene"]["instances"].back()["additions"][0];
        check(addition["name"] == "Local lamp" && addition["components"].size() == 2 &&
                  addition["components"].back()["type"] == "faset.mesh",
              "Local addition supports manual rename and component creation");
        click(ui, "simulation-settings");
        const auto before = query(session, ui)["revision"].get<std::uint64_t>();
        text(ui, "simulation-tick-rate", "120");
        check(std::abs(query(session, ui)["scene"]["simulation"]["fixed_delta"].get<double>() -
                       1.0 / 120) < 1e-8,
              "Simulation UI converts Hz to seconds");
        check(query(session, ui)["revision"] == before + 1,
              "Simulation field is one Undo transaction");
        text(ui, "simulation-gravity-1", "-3");
        check(query(session, ui)["scene"]["simulation"]["gravity"][1] == -3,
              "Simulation gravity field");
        click(ui, "simulation-close");
        click(ui, "undo");
        check(query(session, ui)["scene"]["simulation"]["gravity"][1] == -9.81,
              "Simulation Undo restores gravity");

        nested = leaf(session, main, 2);
        click(ui, "entity-" + nested_id);
        text(ui, position_field(nested), "7");
        click(ui, "object-open-source");
        source = query(session, ui)["scene"]["entities"][0];
        click(ui, "component-remove-" + source["components"][0]["id"].get<std::string>());
        click(ui, "back-document");
        const auto conflicted = session.commands().resolved_scene(main);
        check(!conflicted.at("conflicts").empty() &&
                  query(session, ui)["scene"]["instances"].back()["overrides"].size() == 1,
              "Source deletion retains unapplied override as an explicit conflict");
        click(ui, "tab-conflicts");
        click(ui, "conflict-0-discard");
        check(query(session, ui)["scene"]["instances"].back()["overrides"].empty(),
              "Conflict discard is an explicit authoring action");
        click(ui, "undo");
        check(query(session, ui)["scene"]["instances"].back()["overrides"].size() == 1,
              "Conflict discard is undoable");

        auto opaque = authoring::make_entity(session.authoring().schemas(), "Future transform");
        opaque["components"][0]["version"] = 2;
        opaque["components"][0]["fields"] = {{"future", "untouched"}};
        session.authoring().transact(main, query(session, ui).at("revision"),
                                     Json::array({{{"op", "entity.create"}, {"entity", opaque}}}));
        ui.frame({});
        click(ui, "entity-" + opaque.at("id").get<std::string>());
        const auto opaque_component = opaque["components"][0]["id"].get<std::string>();
        const auto* raw = ui.widgets().find("opaque-fields-" + opaque_component);
        check(raw && !raw->enabled && raw->text == opaque["components"][0]["fields"].dump(),
              "Unsupported component version stays opaque and read-only");
        check(!ui.widgets().find("field-" + opaque_component + "-future"),
              "Current-schema Inspector does not interpret future component fields");
        check(query(session, ui)["scene"]["entities"].back() == opaque,
              "Opaque component data survives Inspector and viewport updates");
        renderer.render(ui.snapshot());
        renderer.capture(root / "templates.ppm");
        check(renderer.stats().validation_errors == 0, "Vulkan validation");
        std::cout << "Manual templates: create/two instances/nesting/overrides/Revert/source "
                     "rename/suppression restore/local additions/conflicts/opaque versions; "
                     "simulation UI+Undo passed. "
                  << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nRetained: " << root << '\n';
        return 1;
    }
}
