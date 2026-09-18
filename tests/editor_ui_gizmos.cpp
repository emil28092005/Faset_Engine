#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
using namespace faset;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
render::Event mouse(render::Event::Type type, render::Vec2 p) {
    render::Event result;
    result.type = type;
    result.button = 1;
    result.x = p[0];
    result.y = p[1];
    return result;
}
void click(editor::EditorUI& ui, const std::string& id) {
    const auto* widget = ui.widgets().find(id);
    check(widget, "Missing gizmo test widget");
    const auto r = widget->rect.intersection(widget->clip);
    check(r.width > 0 && r.height > 0, "Clipped gizmo test widget");
    const render::Vec2 p{r.x + r.width * .5f, r.y + r.height * .5f};
    ui.frame({mouse(render::Event::Type::MouseDown, p), mouse(render::Event::Type::MouseUp, p)});
}
render::Vec2 project(editor::EditorUI& ui, render::Vec3 p) {
    const auto& s = ui.snapshot();
    const auto& m = s.view_projection;
    const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    return {s.scene_rect[0] +
                ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w + 1) * s.scene_rect[2] * .5f,
            s.scene_rect[1] + ((m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w + 1) *
                                  s.scene_rect[3] * .5f};
}
render::Vec2 along(render::Vec2 a, render::Vec2 b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}
void near(float a, float b, const char* message) {
    check(std::abs(a - b) < .002f, message);
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("faset-gizmo-ui-" + new_id());
    try {
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 900, "Parented gizmo acceptance", true, true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        ui.frame({});
        auto query = [&] { return session.authoring().query(ui.current_document()); };
        auto parent =
            authoring::make_entity(session.authoring().schemas(), "Rotated scaled parent");
        parent["components"][0]["fields"] = {
            {"position", {.5, .6, 0}}, {"rotation", {0, .8, 0}}, {"scale", {2, 2, 2}}};
        auto child =
            authoring::make_entity(session.authoring().schemas(), "Child", parent.at("id"));
        child["components"].push_back(
            {{"id", new_id()},
             {"type", "faset.mesh"},
             {"version", 1},
             {"fields", {{"asset", ""}, {"primitive", "cube"}, {"color", {.6, .6, .65, 1.}}}}});
        session.authoring().transact(ui.current_document(), query().at("revision"),
                                     Json::array({{{"op", "entity.create"}, {"entity", parent}},
                                                  {{"op", "entity.create"}, {"entity", child}}}));
        ui.select_entity(child.at("id"));
        ui.frame({});
        const auto original_world = render::transform({.5f, .6f, 0}, {0, .8f, 0}, {2, 2, 2});
        auto check_parent = [&] {
            check(query()["scene"]["entities"][0] == parent, "Child gizmo must not mutate parent");
        };
        auto check_world = [&](render::Mat4 expected) {
            check(!ui.snapshot().draws.empty(), "Rendered child mesh missing");
            for (std::size_t i = 0; i < 16; ++i)
                near(ui.snapshot().draws[0].model[i], expected[i],
                     "Rendered child world transform mismatch");
        };
        check_world(original_world);
        const auto center = project(ui, {.5f, .6f, 0});
        const auto tip = project(ui, {1.94f, .6f, 0});
        const auto before = query()["revision"].get<std::uint64_t>();
        ui.frame({mouse(render::Event::Type::MouseDown, along(center, tip, .65f))});
        ui.frame({mouse(render::Event::Type::MouseMove, along(center, tip, .9f))});
        ui.frame({mouse(render::Event::Type::MouseMove, along(center, tip, 1.15f))});
        check(query()["revision"] == before, "Multi-frame Move only previews before release");
        auto moved = original_world;
        moved[12] += .72f;
        check_world(moved);
        ui.frame({mouse(render::Event::Type::MouseUp, along(center, tip, 1.15f))});
        check(query()["revision"] == before + 1, "Move commits exactly one transaction");
        check_world(moved);
        check_parent();
        click(ui, "undo");
        check_world(original_world);

        // Rotate and Scale expose the selected object's local axes, including its parent.
        const render::Vec3 local_x{std::cos(.8f), 0, -std::sin(.8f)};
        const auto local_tip = project(ui, {.5f + local_x[0] * 1.44f, .6f, local_x[2] * 1.44f});
        for (const std::string mode : {"Rotate", "Scale"}) {
            click(ui, "gizmo-" + mode);
            const auto revision = query()["revision"].get<std::uint64_t>();
            ui.frame({mouse(render::Event::Type::MouseDown, along(center, local_tip, .7f))});
            ui.frame({mouse(render::Event::Type::MouseMove, along(center, local_tip, .85f))});
            ui.frame({mouse(render::Event::Type::MouseMove, along(center, local_tip, 1.f))});
            check(query()["revision"] == revision, "Local-axis gizmo preview does not author");
            ui.frame({mouse(render::Event::Type::MouseUp, along(center, local_tip, 1.f))});
            check(query()["revision"] == revision + 1, "Local-axis gizmo commits one transaction");
            const auto fields = query()["scene"]["entities"][1]["components"][0]["fields"];
            if (mode == "Rotate") {
                near(fields["rotation"][0], .3f * 3.141593f, "Rotate edits local X radians");
                near(fields["rotation"][1], 0, "Rotate preserves local Y");
                check_world(render::multiply(original_world,
                                             render::transform({}, {.3f * 3.141593f, 0, 0})));
            } else {
                near(fields["scale"][0], 1.3f, "Scale edits local X");
                near(fields["scale"][1], 1, "Scale preserves local Y");
                check_world(
                    render::multiply(original_world, render::transform({}, {}, {1.3f, 1, 1})));
            }
            check_parent();
            click(ui, "undo");
            check_world(original_world);
        }
        click(ui, "gizmo-Move");
        const auto cancelled = query()["revision"];
        ui.frame({mouse(render::Event::Type::MouseDown, along(center, tip, .65f))});
        ui.frame({mouse(render::Event::Type::MouseMove, along(center, tip, 1.15f))});
        render::Event escape;
        escape.type = render::Event::Type::KeyDown;
        escape.key = "Escape";
        ui.frame({escape, mouse(render::Event::Type::MouseUp, along(center, tip, 1.15f))});
        check(query()["revision"] == cancelled, "Escape cancels gizmo without authoring");
        check_world(original_world);
        session.authoring().transact(ui.current_document(), query().at("revision"),
                                     Json::array({{{"op", "component.set"},
                                                   {"entity", child.at("id")},
                                                   {"component", child["components"][0]["id"]},
                                                   {"field", "rotation"},
                                                   {"value", {.3, .4, .2}}}}));
        ui.frame({});
        click(ui, "gizmo-Rotate");
        const auto rotated_local = render::transform({}, {.3f, .4f, .2f});
        const auto rotated_world = render::multiply(original_world, rotated_local);
        check_world(rotated_world);
        const auto y_tip = project(ui, {.5f + rotated_world[4] * .72f,
                                        .6f + rotated_world[5] * .72f, rotated_world[6] * .72f});
        ui.frame({mouse(render::Event::Type::MouseDown, along(center, y_tip, .7f))});
        ui.frame({mouse(render::Event::Type::MouseMove, along(center, y_tip, .9f))});
        ui.frame({mouse(render::Event::Type::MouseUp, along(center, y_tip, .9f))});
        check_world(
            render::multiply(rotated_world, render::transform({}, {0, .2f * 3.141593f, 0})));
        check_parent();
        click(ui, "undo");
        check_world(rotated_world);
        renderer.render(ui.snapshot());
        renderer.capture(root / "gizmos.ppm");
        check(renderer.stats().validation_errors == 0, "Vulkan validation");
        std::cout << "Parented world Move/local Rotate/local Scale, multi-frame preview, Undo, "
                     "Escape and composed local rotation passed. "
                  << path_to_utf8(root) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nRetained: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
