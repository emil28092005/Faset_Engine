#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
#include <thread>
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
void poll(editor::EditorUI& ui) {
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    ui.frame({});
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("faset-ui-reload-" + new_id());
    try {
        const auto styles = root / "styles/dark.json",
                   layout_path = root / "styles/editor-layout.json";
        auto theme = read_json(path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        auto layout = read_json(path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/editor-layout.json");
        atomic_write_json(styles, theme);
        atomic_write_json(layout_path, layout);
        editor::Session session({root / "project", path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 800, "Style reload acceptance", true, true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            styles);
        ui.frame({});
        click(ui, "add-cube");
        click(ui, "object-name");
        render::Event input;
        input.type = render::Event::Type::TextInput;
        input.text = "Unfinished name";
        ui.frame({key("A", true), input});
        const auto revision = session.authoring().query(ui.current_document()).at("revision");
        const auto width = ui.widgets().find("scene_panel")->rect.width;
        renderer.render(ui.snapshot());
        const auto pixels_before = renderer.pixels();
        theme["surface"] = {.18, .24, .12, 1};
        layout["root"]["children"][2]["children"][0]["layout"]["width"] = 270;
        atomic_write_json(styles, theme);
        atomic_write_json(layout_path, layout);
        poll(ui);
        check(ui.widgets().find("scene_panel")->rect.width == 270 && width != 270,
              "Valid layout edit changes live panel geometry");
        check(ui.widgets().focused_id() == "object-name" &&
                  ui.widgets().find("object-name")->text == "Unfinished name",
              "Valid reload preserves focus and unfinished text");
        renderer.render(ui.snapshot());
        const auto pixels_after = renderer.pixels();
        const auto pixel = (500 * renderer.width() + 10) * 4;
        check(pixels_before.at(pixel) != pixels_after.at(pixel) ||
                  pixels_before.at(pixel + 1) != pixels_after.at(pixel + 1),
              "Valid theme edit changes actual Vulkan pixels");
        check(session.authoring().query(ui.current_document()).at("revision") == revision,
              "Presentation reload does not author the scene");
        const auto working_theme = ui.widgets().theme().to_json();
        const auto working_menubar = ui.widgets().find("menubar")->layout.height;
        auto bad_layout = layout;
        bad_layout["root"]["children"][0]["layout"]["height"] = 68;
        bad_layout["root"]["children"].back()["text"] = 7;
        theme["surface"] = {.3, .08, .2, 1};
        atomic_write_json(styles, theme);
        atomic_write_json(layout_path, bad_layout);
        poll(ui);
        check(ui.widgets().theme().to_json() == working_theme &&
                  ui.widgets().find("menubar")->layout.height == working_menubar,
              "Invalid layout rejects both candidate files atomically");
        check(ui.widgets().find("scene_panel")->rect.width == 270 &&
                  ui.widgets().focused_id() == "object-name" &&
                  ui.widgets().find("object-name")->text == "Unfinished name",
              "Invalid reload retains last good layout and editing state");
        const auto log_count = session.logs().size();
        poll(ui);
        check(session.logs().size() == log_count,
              "Unchanged invalid file does not flood editor logs");
        atomic_write_json(layout_path, layout);
        poll(ui);
        check(ui.widgets().theme().surface[0] == .3f,
              "Fixing invalid layout publishes pending theme candidate");
        atomic_write(styles, "{ broken json");
        poll(ui);
        check(ui.widgets().theme().surface[0] == .3f,
              "Partially written JSON keeps last working theme");
        atomic_write_json(styles, theme);
        poll(ui);
        ui.frame({key("Return")});
        const auto saved = session.authoring().query(ui.current_document());
        check(saved.at("revision") == revision.get<std::uint64_t>() + 1 &&
                  saved["scene"]["entities"][0]["name"] == "Unfinished name",
              "Retained callback commits preserved edit once after failed and valid reloads");
        renderer.render(ui.snapshot());
        renderer.capture(root / "reloaded.ppm");
        check(renderer.stats().validation_errors == 0, "Reload Vulkan validation");
        std::cout << "Theme/layout live reload pixels+geometry, atomic rejection, focus+draft "
                     "preservation, callback and diagnostic dedup passed. "
                  << path_to_utf8(root) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nRetained: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
