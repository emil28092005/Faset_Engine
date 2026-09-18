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
void click(editor::EditorUI& ui, const std::string& id) {
    const auto* w = ui.widgets().find(id);
    check(w, "Missing widget: " + id);
    const auto r = w->rect.intersection(w->clip);
    check(r.width > 0 && r.height > 0, "Hidden widget: " + id);
    render::Event down;
    down.type = render::Event::Type::MouseDown;
    down.button = 1;
    down.x = r.x + r.width * .5f;
    down.y = r.y + r.height * .5f;
    auto up = down;
    up.type = render::Event::Type::MouseUp;
    ui.frame({down, up});
}
void text(editor::EditorUI& ui, const std::string& id, const std::string& value) {
    click(ui, id);
    render::Event input;
    input.type = render::Event::Type::TextInput;
    input.text = value;
    ui.frame({key("A", true), input, key("Return")});
}
void open(editor::EditorUI& ui) {
    click(ui, "menu-File");
    click(ui, "project-settings");
}
} // namespace
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("faset-project-settings-ui-" + new_id());
    try {
        const auto project_file = root / "project.faset.json";
        atomic_write_json(project_file, {{"format", "faset.project"},
                                         {"version", 1},
                                         {"id", new_id()},
                                         {"name", "Original project"},
                                         {"dimension", 3},
                                         {"custom_metadata", {{"preserve", true}}}});
        editor::Session session({root, FASET_TEST_ENGINE, root});
        render::Renderer renderer({1280, 800, "Project settings acceptance", true, true});
        editor::EditorUI ui(session, renderer,
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            std::filesystem::path(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        ui.frame({});
        click(ui, "add-cube");
        session.authoring().save(ui.current_document(), "Scenes/Main.scene.json");
        const auto other = session.authoring().create("Other", 2);
        session.authoring().save(other.at("id"), "Scenes/Other.scene.json");
        ui.frame({});
        const auto document_before = session.authoring().query(ui.current_document());
        const auto project_before = read_json(project_file);
        open(ui);
        text(ui, "project-settings-name", "Unsaved project name");
        click(ui, "project-settings-cancel");
        check(read_json(project_file) == project_before,
              "Cancel discards only form draft without writing project");
        open(ui);
        check(ui.widgets().find("project-settings-name")->text == "Original project",
              "Opening settings reloads current saved values");
        text(ui, "project-settings-name", "Новый проект");
        click(ui, "project-settings-2d");
        click(ui, "project-scene-choice-1");
        check(ui.widgets().find("project-settings-start")->text == "Scenes/Other.scene.json",
              "Saved scene chooser sets project-relative path");
        click(ui, "project-settings-save");
        auto saved = read_json(project_file);
        check(saved["name"] == "Новый проект" && saved["dimension"] == 2 &&
                  saved["start_scene"] == "Scenes/Other.scene.json",
              "Explicit Save project persists typed settings");
        check(saved["custom_metadata"] == project_before["custom_metadata"] &&
                  saved["id"] == project_before["id"],
              "Project settings preserve unknown metadata and project identity");
        check(session.authoring().query(ui.current_document()) == document_before,
              "Project settings do not change current document, dimension, revision or Undo");
        open(ui);
        text(ui, "project-settings-start", "Scenes/Missing.scene.json");
        click(ui, "project-settings-save");
        check(ui.widgets().find("project-settings-panel")->visible &&
                  !ui.widgets().find("project-settings-error")->text.empty() &&
                  read_json(project_file) == saved,
              "Invalid start scene keeps saved project and form open");
        click(ui, "project-settings-reload");
        text(ui, "project-settings-name", "Local draft");
        const auto external = session.commands().call("faset_project_settings_get", Json::object());
        session.commands().call(
            "faset_project_settings_set",
            {{"revision", external.at("revision")}, {"settings", {{"name", "External edit"}}}});
        click(ui, "project-settings-save");
        check(ui.widgets().find("project-settings-panel")->visible &&
                  read_json(project_file)["name"] == "External edit" &&
                  ui.widgets().find("project-settings-name")->text == "Local draft",
              "Revision conflict preserves external project and local draft");
        check(ui.widgets().find("project-settings-error")->text.find("reload") != std::string::npos,
              "Conflict directs user to reload");
        click(ui, "project-settings-reload");
        check(ui.widgets().find("project-settings-name")->text == "External edit",
              "Explicit Reload saved replaces stale form draft");
        click(ui, "project-settings-3d");
        click(ui, "project-scene-choice-0");
        click(ui, "project-settings-save");
        check(read_json(project_file)["start_scene"] == "Scenes/Main.scene.json",
              "Save after explicit reload uses new content revision");
        check(session.authoring().query(ui.current_document()) == document_before,
              "Project settings remain outside scene Undo throughout conflicts");
        open(ui);
        renderer.render(ui.snapshot());
        renderer.capture(root / "project-settings.ppm");
        ui.frame({key("Escape")});
        check(!ui.widgets().find("project-settings-panel")->visible,
              "Escape closes project settings without saving");
        check(renderer.stats().validation_errors == 0, "Project settings Vulkan validation");
        std::cout << "Project settings name/type/start-scene Save/Cancel/Reload/conflict, metadata "
                     "preservation and separate scene Undo passed. "
                  << root << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nRetained: " << root << '\n';
        return 1;
    }
}
