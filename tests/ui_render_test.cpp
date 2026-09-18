#include <faset/core/io.hpp>
#include <faset/ui/ui.hpp>
#include <iostream>
using namespace faset;
int ui_render_main(int argc, char** argv) {
    try {
        render::Renderer renderer({1280, 800, "Faset UI reference implementation", true, true});
        ui::Context ui(path_from_utf8(FASET_TEST_FONT));
        ui.set_theme(ui::Theme::load(path_from_utf8(FASET_UI_THEME)));
        ui.apply_layout(
            read_json(path_from_utf8(FASET_UI_THEME).parent_path() / "editor-layout.json"));
        auto& menu = ui.find("menubar")->add(ui::Kind::Row, "menuitems");
        for (const auto& name : {"Faset", "File", "Edit", "Scene", "View", "Help"})
            menu.add(ui::Kind::Button, "menu-" + std::string(name), name).layout.width = 65;
        auto& tools = ui.find("toolbar")->add(ui::Kind::Row, "tools");
        for (const auto& name :
             {"Workshop", "Courtyard", "Save", "Undo", "Redo", "Play", "Stop", "Build"})
            tools.add(ui::Kind::Button, "tool-" + std::string(name), name).layout.width = 80;
        auto& scene = *ui.find("scene_panel");
        scene.add(ui::Kind::Tab, "scene-tab", "Scene").selected = true;
        auto& tree = scene.add(ui::Kind::Column, "scene-tree");
        tree.layout.flex = 1;
        tree.layout.scroll = true;
        tree.layout.gap = 0;
        for (const auto& name :
             {"Courtyard", "Camera", "Sun", "Ground", "Player", "Door", "Crates"}) {
            auto& row = tree.add(ui::Kind::TreeRow, "object-" + std::string(name), name);
            row.indent = std::string(name) == "Courtyard" ? 0 : 1;
            row.selected = std::string(name) == "Door";
        }
        auto& inspector = *ui.find("inspector_panel");
        inspector.add(ui::Kind::Tab, "inspector-tab", "Inspector").selected = true;
        auto& body = inspector.add(ui::Kind::Column, "properties");
        body.layout.padding = 10;
        body.layout.scroll = true;
        body.layout.flex = 1;
        body.layout.gap = 8;
        body.add(ui::Kind::TextField, "object-name", "Door");
        body.add(ui::Kind::Label, "transform-title", "Transform");
        for (const auto& name : {"Position", "Rotation", "Scale"}) {
            auto& row = body.add(ui::Kind::Row, "property-" + std::string(name));
            row.layout.height = 30;
            row.add(ui::Kind::Label, "label-" + std::string(name), name).layout.width = 64;
            for (int axis = 0; axis < 3; ++axis) {
                auto& field =
                    row.add(ui::Kind::NumberField, std::string(name) + std::to_string(axis));
                field.layout.flex = 1;
                field.layout.min_width = 40;
                field.value = std::string(name) == "Scale" ? 1 : 0;
            }
        }
        body.add(ui::Kind::Label, "mesh-title", "Mesh");
        body.add(ui::Kind::Button, "mesh-asset", "Door.glb");
        body.add(ui::Kind::Label, "body-title", "Rigid Body");
        auto& check = body.add(ui::Kind::Checkbox, "static", "Static");
        check.checked = true;
        body.add(ui::Kind::Label, "controller-title", "Door Controller (C++)");
        auto& speed = body.add(ui::Kind::NumberField, "speed");
        speed.value = 2;
        body.add(ui::Kind::Button, "add-component", "Add Component");
        body.add(ui::Kind::Label, "unicode-check", "Cyrillic: Дверь, сцена");
        auto& bottom = *ui.find("bottom_panel");
        auto& tabs = bottom.add(ui::Kind::Row, "asset-tabs");
        tabs.layout.height = 30;
        tabs.add(ui::Kind::Tab, "assets-tab", "Assets").selected = true;
        tabs.add(ui::Kind::Tab, "console-tab", "Console");
        auto& search = bottom.add(ui::Kind::Row, "asset-search");
        search.layout.height = 30;
        search.add(ui::Kind::Label, "breadcrumb", "Assets / Models").layout.flex = 1;
        search.add(ui::Kind::TextField, "search", "Search assets...").layout.width = 260;
        for (const auto& name : {"Door.glb", "Crate.glb", "Ground.material", "Courtyard.scene"}) {
            auto& row = bottom.add(ui::Kind::TreeRow, "asset-" + std::string(name), name);
            row.layout.height = 26;
            row.indent = 1;
        }
        ui.find("statusbar")
            ->add(ui::Kind::Label, "status",
                  "Ready                                      Vulkan 1.3  |  C++ "
                  "gameplay  |  Local project");
        ui.layout(1280, 800);
        render::Snapshot snapshot;
        const auto rect = ui.find("viewport")->rect;
        snapshot.scene_rect = {rect.x, rect.y, rect.width, rect.height};
        snapshot.eye = {5, 4, 7};
        snapshot.view_projection =
            render::multiply(render::perspective(.75f, rect.width / rect.height, .1f, 100),
                             render::look_at(snapshot.eye, {0, 1, 0}));
        render::DrawItem ground;
        ground.mesh = render::cube_mesh();
        ground.model = render::transform({0, -.25f, 0}, {}, {8, .5f, 8});
        ground.color = {.25f, .29f, .32f, 1};
        snapshot.draws.push_back(ground);
        render::DrawItem door;
        door.mesh = render::cube_mesh();
        door.model = render::transform({0, 1.25f, 0}, {}, {1.6f, 2.5f, .3f});
        door.color = {.46f, .28f, .13f, 1};
        snapshot.draws.push_back(door);
        render::DrawItem crate;
        crate.mesh = render::cube_mesh();
        crate.model = render::transform({-2, .6f, 1}, {0, .2f, 0}, {1.2f, 1.2f, 1.2f});
        crate.color = {.38f, .25f, .14f, 1};
        snapshot.draws.push_back(crate);
        ui.draw(snapshot);
        renderer.render(snapshot);
        renderer.capture(path_from_utf8(argc > 1 ? argv[1] : "ui-test.ppm"));
        if (renderer.stats().validation_errors)
            throw std::runtime_error("Vulkan validation reported UI rendering errors");
        std::cout << "UI glyph atlas and retained panels rendered on " << renderer.stats().device
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, ui_render_main);
}
#else
int main(int argc, char** argv) {
    return ui_render_main(argc, argv);
}
#endif
