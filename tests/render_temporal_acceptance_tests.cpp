#include "render_temporal_fixtures.hpp"
#include <faset/render/temporal.hpp>

#include <array>
#include <cstdint>
#include <memory>

using namespace faset::render;
using namespace faset::render::temporal_test;

namespace {
void moving_reveal_and_camera_resets(VisibilityMode visibility) {
    constexpr std::uint32_t width = 160, height = 120;
    auto config = headless_config(width, height, visibility);
    config.temporal_mode = TemporalMode::TAA;
    Renderer taa(config);
    config.temporal_mode = TemporalMode::Off;
    Renderer off(config);

    auto frame = lit_scene(width, height);
    frame.draws.push_back(cube({0, 0, -2}, {.1f, .95f, .2f, 1}, "background"));
    frame.draws.push_back(cube({0, 0, 1}, {.95f, .1f, .1f, 1}, "door"));
    frame.ui_quads.push_back({2, 2, 25, 12, {.8f, .7f, .25f, 1}});
    taa.render(frame);
    require(!taa.stats().temporal_history_valid &&
                taa.stats().temporal_reset_reason == TemporalResetReason::FirstFrame,
            "The first TAA frame must use only current color");
    taa.render(frame);
    require(taa.stats().temporal_history_valid,
            "An unchanged second frame must accept eligible temporal history");

    frame.draws[1].model = transform({3, 0, 1});
    taa.render(frame);
    off.render(frame);
    require(mean_rgb_error(taa.pixels(), off.pixels(), width, height, {75, 55, 10, 10}) <= 8.0,
            "Opening a foreground door reveals current background without old-color trail");
    require(taa.stats().validation_errors == 0,
            "Moving-disocclusion resolve must pass Vulkan validation");
    require(pixel(taa.pixels(), width, 4, 4) == pixel(off.pixels(), width, 4, 4),
            "Moving scene and TAA must leave UI pixel-exact");

    frame.camera_cut = true;
    frame.eye = {1, 0, 6};
    frame.view_projection = multiply(frame.projection, look_at(frame.eye, {0, 0, 0}));
    taa.render(frame);
    off.render(frame);
    require(!taa.stats().temporal_history_valid &&
                taa.stats().temporal_reset_reason == TemporalResetReason::CameraCut &&
                mean_rgb_error(taa.pixels(), off.pixels(), width, height,
                               {75, 55, 10, 10}) <= 8.0,
            "Explicit camera cut must discard stale color immediately");
    frame.camera_cut = false;
    frame.eye = {7, 0, 6};
    frame.view_projection = multiply(frame.projection, look_at(frame.eye, {0, 0, 0}));
    taa.render(frame);
    require(!taa.stats().temporal_history_valid &&
                taa.stats().temporal_reset_reason ==
                    TemporalResetReason::CameraDiscontinuity,
            "An unmarked large camera teleport also discards history");

    frame.eye = {0, 0, 6};
    frame.view_projection = multiply(frame.projection, look_at(frame.eye, {0, 0, 0}));
    frame.draws[0].mesh = std::make_shared<Mesh>(*frame.draws[0].mesh);
    taa.render(frame);
    require(taa.stats().validation_errors == 0,
            "Mesh identity change and camera return must keep temporal output valid");
}

void lower_resolution_scene_and_output_ui() {
    constexpr std::uint32_t width = 320, height = 240;
    auto config = headless_config(width, height, VisibilityMode::Direct);
    config.temporal_mode = TemporalMode::Upscale;
    config.render_scale = .67f;
    Renderer upscale(config);
    auto frame = lit_scene(width, height);
    frame.draws.push_back(cube({0, 0, 0}, {.8f, .6f, .25f, 1}, "thin-scene"));
    frame.ui_quads.push_back({2, 2, 25, 12, {.8f, .7f, .25f, 1}});
    upscale.render(frame);
    require(upscale.stats().effective_temporal_mode == TemporalMode::Upscale &&
                upscale.stats().temporal_internal_width == 215 &&
                upscale.stats().temporal_internal_height == 161 &&
                upscale.pixels().size() == std::size_t(width) * height * 4,
            "0.67 upscale rasterizes 215x161 while capture remains 320x240");
    require(upscale.stats().validation_errors == 0,
            "Upscale image resize and composite pass Vulkan validation");
}
} // namespace

int main() {
    moving_reveal_and_camera_resets(VisibilityMode::Direct);
    moving_reveal_and_camera_resets(VisibilityMode::GpuFrustum);
    moving_reveal_and_camera_resets(VisibilityMode::GpuOcclusion);
    lower_resolution_scene_and_output_ui();
}
