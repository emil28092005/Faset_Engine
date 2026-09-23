#include <SDL3/SDL.h>
#include <cmath>
#include <faset/render/render_graph.hpp>
#include <faset/render/renderer.hpp>
#include <iostream>
#include <stdexcept>
using namespace faset::render;
void require(bool test, const char* message) {
    if (!test)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--unit") {
            int count{};
            RenderGraph invalid;
            invalid.add("consumer", {"missing"}, {}, [&] { ++count; });
            bool caught{};
            try {
                invalid.execute();
            } catch (const std::runtime_error&) {
                caught = true;
            }
            require(caught && count == 0, "Graph must validate before side effects");
            RenderGraph graph;
            graph.import("external");
            graph.add("first", {"external"}, {"color"}, [&] {
                require(count == 0, "Pass order");
                ++count;
            });
            graph.add("second", {"color"}, {}, [&] { ++count; });
            graph.execute();
            require(count == 2, "Pass execution count");
            auto t = transform({2, 3, 4}, {}, {2, 3, 4});
            require(t[12] == 2 && t[13] == 3 && t[14] == 4, "Transform translation");
            auto m = multiply(identity, t);
            require(m == t, "Matrix multiplication identity");
            require(cube_mesh()->indices.size() == 36, "Cube triangle topology");
            std::cout << "Render graph and math contracts passed\n";
            return 0;
        }
        bool visible = argc > 1 && std::string(argv[1]) == "--visible";
        Renderer renderer({320, 240, "Faset render validation", !visible, true});
        if (!visible) {
            renderer.set_clipboard("Offscreen Café 世界");
            require(renderer.clipboard() == "Offscreen Café 世界",
                    "Offscreen clipboard is local and does not require SDL video");
        }
        Snapshot scene;
        scene.eye = {4, 3, 5};
        scene.view_projection =
            multiply(perspective(.85f, 320.f / 240.f, .1f, 100), look_at(scene.eye, {0, 0, 0}));
        scene.draws.push_back(
            {cube_mesh(), transform({0, 0, 0}), {.2f, .65f, .95f, 1}, .4f, .15f, true});
        scene.draws.push_back({cube_mesh(),
                               transform({0, -1, 0}, {}, {10, 1, 10}),
                               {.45f, .48f, .5f, 1},
                               .8f,
                               0,
                               true});
        scene.ui_quads.push_back({8, 8, 70, 16, {.8f, .1f, .15f, 1}});
        auto texture = std::make_shared<Texture>();
        texture->width = texture->height = 1;
        texture->rgba = {20, 220, 40, 255};
        scene.ui_quads.push_back({260, 8, 40, 20, {1, 1, 1, 1}, texture});
        UiTriangles diagnostic;
        diagnostic.vertices = {{{80, 8, 0}, {}, {0, 0, 1, 1}},
                               {{150, 8, 0}, {}, {0, 0, 1, 1}},
                               {{80, 40, 0}, {}, {0, 0, 1, 1}}};
        diagnostic.clip_rect = {90, 8, 30, 32};
        scene.ui_triangles.push_back(diagnostic);
        renderer.render(scene);
        require(renderer.stats().validation_errors == 0, "Vulkan validation reported an error");
        if (renderer.stats().validation_enabled)
            require(renderer.stats().gpu_labels_enabled,
                    "Validation context exposes GPU pass labels");
        require(!renderer.stats().gpu_labels_enabled || renderer.stats().gpu_label_count >= 3,
                "Every submitted render graph pass receives a GPU label");
        auto pixels = renderer.pixels();
        require(pixels.size() == 320 * 240 * 4, "Readback dimensions");
        auto index = (10 * 320 + 10) * 4;
        require(pixels[index] > 190 && pixels[index + 1] < 50, "Colored UI pixel");
        index = (10 * 320 + 270) * 4;
        require(pixels[index] < 30 && pixels[index + 1] > 200, "Textured UI pixel");
        require(pixels[(12 * 320 + 95) * 4 + 2] > 240 && pixels[(12 * 320 + 85) * 4 + 2] < 200,
                "UI diagnostic triangles obey their per-command clip rectangle");
        texture->srgb = true;
        ++texture->revision;
        renderer.render(scene);
        auto srgb_pixels = renderer.pixels();
        require(std::abs(int(srgb_pixels[index + 1]) - 220) <= 1,
                "Unlit sRGB texture retains its display-space color");
        texture->srgb = false;
        ++texture->revision;
        auto baked_scene = scene;
        auto baked_mesh = std::make_shared<Mesh>(*cube_mesh());
        for (auto& vertex : baked_mesh->vertices) {
            vertex.position[0] *= 10;
            vertex.position[2] *= 10;
        }
        baked_scene.draws[1].mesh = baked_mesh;
        baked_scene.draws[1].model = transform({0, -1, 0});
        renderer.render(baked_scene);
        auto baked_pixels = renderer.pixels();
        std::size_t normal_difference{};
        for (std::size_t i = 0; i < pixels.size(); ++i)
            if (std::abs(int(pixels[i]) - int(baked_pixels[i])) > 2)
                ++normal_difference;
        require(normal_difference < 10,
                "Nonuniformly scaled normals must match baked geometry lighting");
        auto shadowed = pixels;
        if (argc > 2)
            renderer.capture(std::string(argv[2]) + ".shadowed.ppm");
        for (auto& draw : scene.draws)
            draw.cast_shadow = false;
        renderer.render(scene);
        pixels = renderer.pixels();
        std::size_t shadow_difference{};
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] > shadowed[i] + 8)
                ++shadow_difference;
        if (shadow_difference <= 20) {
            std::cerr << "Shadow difference pixels: " << shadow_difference << "\n";
            if (argc > 2)
                renderer.capture(std::string(argv[2]) + ".unshadowed.ppm");
        }
        require(shadow_difference > 20, "Directional shadow must darken rendered surface pixels");
        for (auto& draw : scene.draws)
            draw.cast_shadow = true;
        std::string reload_error;
        require(renderer.reload_shaders(reload_error), "Compatible shader pipeline reload");
        texture->rgba = {40, 30, 230, 255};
        ++texture->revision;
        renderer.render(scene);
        pixels = renderer.pixels();
        require(pixels[index + 2] > 220, "Texture revision upload");
        Snapshot two_lights;
        two_lights.eye = {0, 0, 6};
        two_lights.projection = perspective(.85f, 320.f / 240.f, .1f, 30.f);
        two_lights.view_projection =
            multiply(two_lights.projection, look_at(two_lights.eye, {0, 0, 0}));
        two_lights.authored_lights_present = true;
        two_lights.draws.push_back(
            {cube_mesh(), transform({-1.4f, 0, 0}), {.5f, .5f, .5f, 1}, .6f, 0, false});
        two_lights.draws.back().instance_key = "left-light-receiver";
        two_lights.draws.push_back(
            {cube_mesh(), transform({1.4f, 0, 0}), {.5f, .5f, .5f, 1}, .6f, 0, false});
        two_lights.draws.back().instance_key = "right-light-receiver";
        two_lights.ui_quads.push_back({8, 8, 40, 20, {.8f, .1f, .15f, 1}});
        for (auto mode : {VisibilityMode::Direct, VisibilityMode::GpuFrustum}) {
            renderer.set_visibility_mode(mode);
            renderer.render(two_lights);
            const auto dark = renderer.pixels();
            require(renderer.stats().validation_errors == 0,
                    "Zero-local-light descriptors are initialized");
            auto legacy_lights = two_lights;
            legacy_lights.authored_lights_present = false;
            renderer.render(legacy_lights);
            const auto legacy = renderer.pixels();
            const auto left = (120 * 320 + 99) * 4;
            require(legacy[left] > dark[left] + 15,
                    "Authored-light presence suppresses the legacy sun even without a local light");
            two_lights.local_lights = {
                {LocalLight::Kind::Point, "red", {-1.4f, 0, 1.4f}, {0, 0, -1},
                 {1, 0, 0, 1}, 8, 2.2f, .35f, .7f, false, 0},
                {LocalLight::Kind::Point, "blue", {1.4f, 0, 1.4f}, {0, 0, -1},
                {0, 0, 1, 1}, 8, 2.5f, .35f, .7f, false, 0}};
            renderer.render(two_lights);
            const auto lit = renderer.pixels();
            const auto right = (120 * 320 + 221) * 4;
            const auto ui = (10 * 320 + 10) * 4;
            require(lit[left] > dark[left] + 20 && lit[right + 2] > dark[right + 2] + 20,
                    "Separated red and blue point lights illuminate their receivers");
            require(std::abs(int(lit[left + 2]) - int(dark[left + 2])) < 6 &&
                        std::abs(int(lit[right]) - int(dark[right])) < 6,
                    "Local light range keeps the opposite colored light off each receiver");
            for (int channel = 0; channel < 4; ++channel)
                require(lit[ui + channel] == dark[ui + channel],
                        "Lighting changes leave UI tint unchanged");
            require(renderer.stats().validation_errors == 0,
                    "Direct and GPU local lighting report no Vulkan errors");
            if (mode == VisibilityMode::GpuFrustum)
                require(renderer.stats().effective_visibility_mode == VisibilityMode::GpuFrustum,
                        "Local light image test actually exercises the GPU visibility path");
            two_lights.local_lights[1].kind = LocalLight::Kind::Spot;
            renderer.render(two_lights);
            const auto aimed = renderer.pixels();
            two_lights.local_lights[1].direction = {1, 0, 0};
            renderer.render(two_lights);
            const auto turned = renderer.pixels();
            require(aimed[right + 2] > turned[right + 2] + 20,
                    "Spotlight cone direction changes receiver illumination");
            two_lights.local_lights.clear();
        }
        renderer.set_visibility_mode(VisibilityMode::Direct);
        if (argc > 2)
            renderer.capture(argv[2]);
        renderer.resize(400, 300);
        renderer.poll_events();
        renderer.render(scene);
        require(renderer.width() == 400 && renderer.height() == 300, "Render target resize");
        require(renderer.stats().validation_errors == 0, "Resize validation error");
        if (visible) {
            int window_count{};
            auto windows = SDL_GetWindows(&window_count);
            require(windows && window_count == 1, "Visible test owns exactly one SDL window");
            auto* window = windows[0];
            SDL_free(windows);
            require(SDL_HideWindow(window), "Hide the test window");
            const auto before = renderer.stats().frame;
            // Exhaust any compositor buffers without an application event poll. Captures
            // must still render fresh content when presentation is unavailable.
            for (int frame = 0; frame < 12; ++frame) {
                scene.ui_quads[0].color = frame % 2 ? Color{0, 1, 0, 1} : Color{1, 0, 0, 1};
                renderer.render(scene);
                auto capture = renderer.pixels();
                const auto at = (10 * renderer.width() + 10) * 4;
                require(capture.at(at + (frame % 2 ? 1 : 0)) > 240,
                        "Hidden-window capture must contain the latest frame");
            }
            require(renderer.stats().frame == before + 12,
                    "Hidden-window capture must progress without swapchain images");
            require(renderer.stats().validation_errors == 0, "Hidden-window validation error");
        }
        std::cout << "Vulkan frame, shadow/PBR, atlas upload, readback and resize passed on "
                  << renderer.stats().device << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
