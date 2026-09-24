#include "render_temporal_fixtures.hpp"
#include <faset/render/temporal.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

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
    frame.scene_rect = {13, 17, 287, 199};
    upscale.render(frame);
    require(upscale.stats().effective_temporal_mode == TemporalMode::Upscale &&
                upscale.stats().temporal_internal_width == 215 &&
                upscale.stats().temporal_internal_height == 161 &&
                upscale.pixels().size() == std::size_t(width) * height * 4,
            "0.67 upscale rasterizes 215x161 while capture remains 320x240");
    require(upscale.stats().validation_errors == 0,
            "Upscale image resize and composite pass Vulkan validation");
    const auto output_ui = pixel(upscale.pixels(), width, 4, 4);
    upscale.render(frame);
    require(upscale.stats().temporal_history_valid,
            "A compatible upscaled second frame has output-resolution history");
    upscale.set_temporal_mode(TemporalMode::Upscale, .5f);
    upscale.render(frame);
    require(upscale.stats().temporal_internal_width == 160 &&
                upscale.stats().temporal_internal_height == 120 &&
                !upscale.stats().temporal_history_valid &&
                upscale.stats().temporal_reset_reason == TemporalResetReason::ScaleChanged &&
                pixel(upscale.pixels(), width, 4, 4) == output_ui,
            "Changing upscale factor resets history without changing sharp output UI");
    upscale.set_temporal_mode(TemporalMode::TAA);
    upscale.render(frame);
    require(upscale.stats().temporal_reset_reason == TemporalResetReason::ScaleChanged &&
                upscale.stats().temporal_internal_width == width &&
                upscale.stats().temporal_internal_height == height,
            "Switching from upscale to 1:1 TAA resets the changed internal scale");
    upscale.set_temporal_mode(TemporalMode::Off);
    upscale.render(frame);
    require(upscale.stats().effective_temporal_mode == TemporalMode::Off &&
                pixel(upscale.pixels(), width, 4, 4) == output_ui,
            "Switching Off restores the full-resolution baseline and sharp UI");

    upscale.resize(319, 241);
    frame = lit_scene(319, 241);
    frame.scene_rect = {13, 17, 285, 199};
    upscale.set_temporal_mode(TemporalMode::Upscale, .5f);
    upscale.render(frame);
    require(upscale.stats().temporal_internal_width == 160 &&
                upscale.stats().temporal_internal_height == 121 &&
                upscale.pixels().size() == std::size_t(319) * 241 * 4 &&
                upscale.stats().validation_errors == 0,
            "Odd output and offset scene rectangle preserve ceil-rounded internal extent");
}

double frame_variation(const std::vector<std::vector<std::uint8_t>>& frames,
                       std::uint32_t width, Region region) {
    require(frames.size() >= 2, "Temporal variation metric needs multiple frames");
    double sum{};
    for (std::size_t i = 1; i < frames.size(); ++i)
        sum += mean_rgb_error(frames[i], frames[i - 1], width,
                              static_cast<std::uint32_t>(frames[i].size() / (width * 4)),
                              region);
    return sum / double(frames.size() - 1);
}

void static_edge_reduces_jitter_variation() {
    constexpr std::uint32_t width = 160, height = 120;
    auto config = headless_config(width, height, VisibilityMode::Direct);
    config.temporal_mode = TemporalMode::TAA;
    Renderer accumulated(config), spatial(config);
    auto frame = lit_scene(width, height);
    frame.clear_color = {0, 0, 0, 1};
    frame.draws.push_back(cube({0, 0, 0}, {1, 1, 1, 1}, "static-edge"));
    std::vector<std::vector<std::uint8_t>> resolved, unaccumulated;
    for (unsigned phase = 0; phase < 16; ++phase) {
        accumulated.render(frame);
        if (phase >= 4)
            resolved.push_back(accumulated.pixels());
        frame.camera_cut = true; // Same jitter phase, but no prior color may be read.
        spatial.render(frame);
        if (phase >= 4)
            unaccumulated.push_back(spatial.pixels());
        frame.camera_cut = false;
    }
    const Region edge_area{25, 12, 110, 95};
    const double raw = frame_variation(unaccumulated, width, edge_area);
    const double temporal = frame_variation(resolved, width, edge_area);
    std::cerr << "Static-edge mean frame variation: unaccumulated=" << raw
              << " TAA=" << temporal << '\n';
    require(raw > .05 && temporal < raw * .98,
            "After warm-up TAA must reduce static edge shimmer across Halton phases");
    require(accumulated.stats().validation_errors == 0 &&
                spatial.stats().validation_errors == 0,
            "Static-edge temporal sequence must not raise Vulkan validation errors");
}
} // namespace

int main() {
    moving_reveal_and_camera_resets(VisibilityMode::Direct);
    moving_reveal_and_camera_resets(VisibilityMode::GpuFrustum);
    moving_reveal_and_camera_resets(VisibilityMode::GpuOcclusion);
    lower_resolution_scene_and_output_ui();
    static_edge_reduces_jitter_variation();
}
