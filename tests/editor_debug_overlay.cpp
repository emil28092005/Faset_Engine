#include <faset/editor/debug_overlay.hpp>
#include <algorithm>
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

        render::RendererConfig hzb_config;
        hzb_config.width = 640;
        hzb_config.height = 700;
        hzb_config.title = "Faset HZB diagnostics test";
        hzb_config.headless = true;
        hzb_config.validation = true;
        hzb_config.visibility_mode = render::VisibilityMode::GpuOcclusion;
        render::Renderer hzb_renderer(hzb_config);
        render::Snapshot hzb_scene;
        hzb_scene.view_id = "overlay-hzb-scene";
        hzb_scene.eye = {4, 3, 6};
        hzb_scene.view_projection = render::multiply(
            render::perspective(.85f, 640.f / 700.f, .1f, 100.f),
            render::look_at(hzb_scene.eye, {0, 0, 0}));
        render::DrawItem cube;
        cube.mesh = render::cube_mesh();
        cube.instance_key = "hzb-cube";
        hzb_scene.draws.push_back(cube);
        hzb_renderer.render(hzb_scene);
        require(hzb_renderer.hzb_debug_image(3).has_value(),
                "Fixture creates a current HZB for the preview");
        editor::DebugOverlay hzb_overlay;
        hzb_overlay.set_visible(true);
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        require(!hzb_scene.ui_triangles.empty(), "HZB panel emits its normal font texture");
        const auto font_texture = hzb_scene.ui_triangles.front().texture;
        require(std::all_of(hzb_scene.ui_triangles.begin(), hzb_scene.ui_triangles.end(),
                            [&](const auto& batch) { return batch.texture == font_texture; }),
                "HZB image is absent while preview toggle is off");
        render::Event preview_click;
        preview_click.button = 1;
        preview_click.x = 34;
        preview_click.y = 405;
        const auto click_preview = [&] {
            preview_click.type = render::Event::Type::MouseDown;
            require(hzb_overlay.process_events(std::span(&preview_click, 1)).empty(),
                    "HZB preview checkbox captures pointer down");
            hzb_scene.ui_triangles.clear();
            hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
            preview_click.type = render::Event::Type::MouseUp;
            require(hzb_overlay.process_events(std::span(&preview_click, 1)).empty(),
                    "HZB preview checkbox captures pointer up");
            hzb_scene.ui_triangles.clear();
            hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        };
        click_preview();
        hzb_scene.ui_triangles.clear();
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        const auto preview = std::find_if(hzb_scene.ui_triangles.begin(),
                                          hzb_scene.ui_triangles.end(),
                                          [&](const auto& batch) {
                                              return batch.texture != font_texture;
                                          });
        require(preview != hzb_scene.ui_triangles.end() && preview->texture->width == 128 &&
                    preview->texture->height == 128 && preview->texture->revision > 0,
                "Enabled HZB preview emits the selected mip as a revised texture");
        click_preview();
        hzb_scene.ui_triangles.clear();
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        require(std::all_of(hzb_scene.ui_triangles.begin(), hzb_scene.ui_triangles.end(),
                            [&](const auto& batch) { return batch.texture == font_texture; }),
                "Disabling HZB preview removes its texture");
        click_preview();
        hzb_scene.ui_triangles.clear();
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        hzb_renderer.render(hzb_scene);
        require(hzb_renderer.stats().validation_errors == 0 &&
                    hzb_renderer.stats().visibility_counters_valid,
                "HZB preview renders cleanly and open diagnostics enable GPU counters");
        hzb_scene.ui_triangles.clear();
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        hzb_renderer.render(hzb_scene);
        if (argc > 2)
            hzb_renderer.capture(argv[2]);
        hzb_overlay.set_visible(false);
        hzb_scene.ui_triangles.clear();
        hzb_overlay.append(hzb_scene, hzb_renderer, 1.f / 60.f);
        require(hzb_scene.ui_triangles.empty(), "Hidden panel emits no HZB preview");
        hzb_renderer.render(hzb_scene);
        require(!hzb_renderer.stats().visibility_counters_valid,
                "Closing diagnostics disables GPU visibility readback");
        std::cout << "ImGui diagnostics, F12, font atlas, clipping and event isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
