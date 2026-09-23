#include <faset/editor/debug_overlay.hpp>
#include <iostream>
#include <stdexcept>

using namespace faset;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        render::Renderer renderer({640, 420, "Faset developer diagnostics test", true, true});
        editor::DebugOverlay overlay;
        render::Snapshot scene;
        scene.clear_color = {.05f, .06f, .07f, 1};
        overlay.append(scene, renderer, 1.f / 60.f);
        require(!overlay.visible() && scene.ui_triangles.empty(), "Diagnostics start hidden");
        renderer.render(scene);
        const auto clean = renderer.pixels();
        render::Event toggle;
        toggle.type = render::Event::Type::KeyDown;
        toggle.key = "F12";
        require(overlay.process_events(std::span(&toggle, 1)).empty() && overlay.visible(),
                "F12 is consumed and shows the diagnostic overlay");
        overlay.append(scene, renderer, 1.f / 60.f);
        require(!scene.ui_triangles.empty() && scene.ui_triangles.front().texture,
                "ImGui draw data reaches Faset's textured UI triangle API");
        require(renderer.visibility_mode() == render::VisibilityMode::Direct,
                "Diagnostic visibility mode starts with the renderer default");
        render::Event mode_click;
        mode_click.type = render::Event::Type::MouseDown;
        mode_click.button = 1;
        mode_click.x = 205;
        mode_click.y = 99;
        require(overlay.process_events(std::span(&mode_click, 1)).empty(),
                "Visibility mode button captures pointer down");
        scene.ui_triangles.clear();
        overlay.append(scene, renderer, 1.f / 60.f);
        mode_click.type = render::Event::Type::MouseUp;
        require(overlay.process_events(std::span(&mode_click, 1)).empty(),
                "Visibility mode button captures pointer up");
        scene.ui_triangles.clear();
        overlay.append(scene, renderer, 1.f / 60.f);
        require(renderer.visibility_mode() == render::VisibilityMode::GpuFrustum,
                "Clicking GPU frustum switches the live renderer");
        scene.ui_triangles.clear();
        overlay.append(scene, renderer, 1.f / 60.f);
        renderer.render(scene);
        auto shown = renderer.pixels();
        std::size_t changed{};
        for (std::size_t i = 0; i < shown.size(); ++i)
            changed += shown[i] != clean[i];
        require(changed > 5000, "Diagnostic panel and text alter actual Vulkan pixels");
        render::Event inside;
        inside.type = render::Event::Type::MouseDown;
        inside.button = 1;
        inside.x = 30;
        inside.y = 65;
        require(overlay.process_events(std::span(&inside, 1)).empty(),
                "Pointer clicks in diagnostics must not edit the underlying scene");
        inside.type = render::Event::Type::MouseUp;
        inside.x = 600;
        require(overlay.process_events(std::span(&inside, 1)).empty(),
                "A diagnostic pointer gesture remains owned through release outside the panel");
        inside.type = render::Event::Type::MouseDown;
        require(overlay.process_events(std::span(&inside, 1)).size() == 1,
                "Pointer events outside diagnostics reach the retained editor");
        inside.type = render::Event::Type::MouseUp;
        inside.x = 30;
        require(overlay.process_events(std::span(&inside, 1)).size() == 1,
                "Editor-owned release remains forwarded after crossing into diagnostics");
        render::Event move = inside;
        move.type = render::Event::Type::MouseMove;
        render::Event wheel;
        wheel.type = render::Event::Type::Wheel;
        wheel.y = 1;
        const render::Event inside_wheel[] = {move, wheel};
        require(overlay.process_events(inside_wheel).empty(),
                "Move and wheel in one event batch must not scroll the underlying editor");
        move.x = 600;
        const render::Event outside_wheel[] = {move, wheel};
        require(overlay.process_events(outside_wheel).size() == 2,
                "Move outside and wheel immediately return to the retained editor");
        if (argc > 1)
            renderer.capture(argv[1]);
        require(overlay.process_events(std::span(&toggle, 1)).empty() && !overlay.visible(),
                "F12 hides diagnostics again");
        scene.ui_triangles.clear();
        overlay.append(scene, renderer, 1.f / 60.f);
        require(scene.ui_triangles.empty(), "Hidden diagnostics emit no geometry");
        renderer.render(scene);
        require(renderer.pixels() == clean, "Hiding diagnostics restores the underlying frame");
        require(renderer.stats().validation_errors == 0, "Diagnostic overlay Vulkan validation");
        std::cout << "ImGui diagnostics, F12, font atlas, clipping and event isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
