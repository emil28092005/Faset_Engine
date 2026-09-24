#include "render_temporal_fixtures.hpp"
#include <faset/render/temporal.hpp>

#include <algorithm>
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

void temporal_tiled_lighting_uses_scene_raster_extent() {
    constexpr std::uint32_t width = 319, height = 241;
    for (const auto temporal_mode : {TemporalMode::TAA, TemporalMode::Upscale}) {
        for (const auto visibility : {VisibilityMode::Direct,
                                      VisibilityMode::GpuFrustum,
                                      VisibilityMode::GpuOcclusion}) {
            auto config = headless_config(width, height, visibility);
            config.temporal_mode = temporal_mode;
            config.render_scale = temporal_mode == TemporalMode::Upscale ? .5f : 1.f;
            config.lighting_mode = LightingMode::Forward;
            Renderer forward(config);
            config.lighting_mode = LightingMode::Tiled;
            Renderer tiled(config);
            auto frame = lit_scene(width, height);
            frame.scene_rect = {11, 9, 297, 223};
            frame.draws.push_back(cube({0, 0, 0}, {.8f, .8f, .8f, 1}, "tile-receiver"));
            for (int i = -3; i <= 3; ++i) {
                LocalLight light;
                light.stable_id = "tile-light-" + std::to_string(i);
                light.position = {float(i) * .32f, .12f, 1.2f};
                light.range = .65f;
                light.intensity = 30.f;
                light.casts_shadow = false;
                frame.local_lights.push_back(light);
            }
            const auto expected_tiles = temporal_mode == TemporalMode::Upscale
                ? 10u * 8u : 20u * 16u;
            for (unsigned phase = 0; phase < 8; ++phase) {
                forward.render(frame);
                tiled.render(frame);
                const auto& stats = tiled.stats();
                require(stats.effective_lighting_path == "tiled" &&
                            stats.light_tile_count == expected_tiles,
                        "Temporal tiled lighting must build the grid at scene raster resolution");
                require(stats.validation_errors == 0 &&
                            forward.stats().validation_errors == 0,
                        "Temporal tiled/forward lighting must pass Vulkan validation");
                const double error = mean_rgb_error(tiled.pixels(), forward.pixels(),
                                                    width, height, {11, 9, 297, 223});
                require(error <= 1.0,
                        "Jittered Direct/P2 tiled lighting must match forward shading near tile boundaries");
            }
        }
    }
}

void tile_membership_tracks_raster_jitter() {
    auto config = headless_config(160, 128, VisibilityMode::Direct);
    config.temporal_mode = TemporalMode::TAA;
    config.lighting_mode = LightingMode::Tiled;
    config.visibility_diagnostics = true;
    Renderer tiled(config);
    Snapshot frame;
    frame.view_id = "jittered-light-tile-membership";
    frame.eye = {0, 0, 6};
    frame.projection = orthographic(-2, 2, -1.6f, 1.6f, .1f, 20.f);
    frame.view_projection = multiply(frame.projection,
                                     look_at(frame.eye, {0, 0, 0}));
    frame.authored_lights_present = true;
    LocalLight edge_light;
    edge_light.stable_id = "boundary-light";
    edge_light.position = {-.2f, .37f, 0};
    edge_light.range = .2f;
    edge_light.intensity = 20;
    edge_light.casts_shadow = false;
    frame.local_lights.push_back(edge_light);
    std::vector<std::uint32_t> memberships;
    for (unsigned phase = 0; phase < 16; ++phase) {
        tiled.render(frame);
        const auto& stats = tiled.stats();
        require(stats.effective_lighting_path == "tiled" &&
                    stats.light_tile_counts_valid &&
                    stats.validation_errors == 0,
                "Jittered tile membership fixture requires diagnostic tile readback");
        memberships.push_back(stats.light_tile_candidate_count);
    }
    require(*std::min_element(memberships.begin(), memberships.end()) <
                *std::max_element(memberships.begin(), memberships.end()),
            "A light grazing a tile edge must follow the scene raster jitter");
}

void occlusion_upscale_hzb_debug_uses_internal_extent() {
    constexpr std::uint32_t width = 319, height = 241;
    auto config = headless_config(width, height, VisibilityMode::GpuOcclusion);
    config.temporal_mode = TemporalMode::Upscale;
    config.render_scale = .5f;
    Renderer renderer(config);
    auto frame = lit_scene(width, height);
    frame.draws.push_back(cube({0, 0, 0}, {.8f, .6f, .2f, 1}, "hzb-cube"));
    renderer.render(frame);
    require(renderer.stats().effective_visibility_mode == VisibilityMode::GpuOcclusion &&
                renderer.stats().temporal_internal_width == 160 &&
                renderer.stats().temporal_internal_height == 121,
            "HZB debug regression requires active occlusion and an odd internal extent");
    const auto mip0 = renderer.hzb_debug_image(0);
    const auto mip1 = renderer.hzb_debug_image(1);
    require(mip0 && mip0->width == 256 && mip0->height == 128 &&
                mip0->rgba.size() == std::size_t(256) * 128 * 4 &&
                mip1 && mip1->width == 128 && mip1->height == 64 &&
                mip1->rgba.size() == std::size_t(128) * 64 * 4,
            "HZB debug readback must copy the allocated internal pyramid extent per mip");
    renderer.render(frame);
    require(renderer.stats().validation_errors == 0,
            "HZB debug readback followed by occlusion render must pass Vulkan validation");
}
} // namespace

int main() {
    moving_reveal_and_camera_resets(VisibilityMode::Direct);
    moving_reveal_and_camera_resets(VisibilityMode::GpuFrustum);
    moving_reveal_and_camera_resets(VisibilityMode::GpuOcclusion);
    lower_resolution_scene_and_output_ui();
    static_edge_reduces_jitter_variation();
    temporal_tiled_lighting_uses_scene_raster_extent();
    tile_membership_tracks_raster_jitter();
    occlusion_upscale_hzb_debug_uses_internal_extent();
}
