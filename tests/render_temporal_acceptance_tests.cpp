#include "render_temporal_fixtures.hpp"
#include <faset/render/temporal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
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

double roi_rgb_sum(const std::vector<std::uint8_t>& image,
                   std::uint32_t width, Region area) {
    double result{};
    for (auto y = area.y; y < area.y + area.height; ++y)
        for (auto x = area.x; x < area.x + area.width; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel)
                result += image[(std::size_t(y) * width + x) * 4 + channel];
    return result;
}

double roi_peak(const std::vector<std::uint8_t>& image,
                std::uint32_t width, Region area) {
    std::uint8_t result{};
    for (auto y = area.y; y < area.y + area.height; ++y)
        for (auto x = area.x; x < area.x + area.width; ++x)
            for (std::size_t channel = 0; channel < 3; ++channel)
                result = std::max(result,
                    image[(std::size_t(y) * width + x) * 4 + channel]);
    return result;
}

std::size_t pixels_over_error(const std::vector<std::uint8_t>& first,
                              const std::vector<std::uint8_t>& second,
                              std::uint32_t width, Region area, int threshold) {
    std::size_t count{};
    for (auto y = area.y; y < area.y + area.height; ++y)
        for (auto x = area.x; x < area.x + area.width; ++x) {
            const auto base = (std::size_t(y) * width + x) * 4;
            bool changed = false;
            for (std::size_t channel = 0; channel < 3; ++channel)
                changed |= std::abs(int(first[base + channel]) -
                                    int(second[base + channel])) > threshold;
            count += changed;
        }
    return count;
}

void preserve_thin_wire_contrast_without_reveal_halo() {
    constexpr std::uint32_t width = 160, height = 120;
    constexpr Region wire_roi{25, 12, 110, 95};
    constexpr Region door_center{75, 55, 10, 10};
    constexpr Region door_edge{65, 45, 30, 35};
    for (const auto mode : {TemporalMode::TAA, TemporalMode::Upscale}) {
        auto config = headless_config(width, height, VisibilityMode::Direct);
        config.temporal_mode = mode;
        config.render_scale = mode == TemporalMode::Upscale ? .67f : 1.f;
        Renderer resolved(config), current_only(config);
        auto wire = lit_scene(width, height);
        wire.view_id = "contrast-wire";
        wire.clear_color = {0, 0, 0, 1};
        auto draw = cube({0, 0, 0}, {1, 1, 1, 1}, "wire");
        draw.model = transform({}, {0, 0, .35f}, {.025f, 1.4f, .05f});
        wire.draws.push_back(draw);
        std::vector<std::vector<std::uint8_t>> raw_frames, resolved_frames;
        double raw_energy{}, resolved_energy{}, raw_peak{}, resolved_peak{};
        for (unsigned phase = 0; phase < 16; ++phase) {
            resolved.render(wire);
            if (phase >= 4) {
                auto image = resolved.pixels();
                resolved_energy += roi_rgb_sum(image, width, wire_roi);
                resolved_peak += roi_peak(image, width, wire_roi);
                resolved_frames.push_back(std::move(image));
            }
            wire.camera_cut = true;
            current_only.render(wire);
            if (phase >= 4) {
                auto image = current_only.pixels();
                raw_energy += roi_rgb_sum(image, width, wire_roi);
                raw_peak += roi_peak(image, width, wire_roi);
                raw_frames.push_back(std::move(image));
            }
            wire.camera_cut = false;
        }
        const auto raw_variation = frame_variation(raw_frames, width, wire_roi);
        const auto resolved_variation = frame_variation(resolved_frames, width, wire_roi);
        std::cerr << "Wire contrast " << (mode == TemporalMode::TAA ? "TAA" : "Upscale")
                  << ": variation " << raw_variation << " -> " << resolved_variation
                  << ", energy " << raw_energy << " -> " << resolved_energy
                  << ", peak " << raw_peak << " -> " << resolved_peak << '\n';
        const bool wire_quality = resolved_variation <= raw_variation * .95 &&
            resolved_energy >= raw_energy * .95 &&
            resolved_energy <= raw_energy * 1.05 &&
            resolved_peak >= raw_peak * .9;

        auto door = lit_scene(width, height);
        door.view_id = "contrast-door";
        door.clear_color = {0, 0, 0, 1};
        door.draws.push_back(cube({0, 0, -2}, {.1f, .9f, .2f, 1}, "background"));
        door.draws.push_back(cube({0, 0, 1}, {.9f, .1f, .1f, 1}, "door"));
        Renderer background_only(config);
        bool reveal_quality = true;
        for (unsigned phase = 0; phase < 4; ++phase) {
            if (phase == 2)
                door.draws[1].model = transform({3, 0, 1});
            resolved.render(door);
            const auto image = resolved.pixels();
            auto without_door = door;
            without_door.draws.pop_back();
            background_only.render(without_door);
            const auto steady_background = background_only.pixels();
            door.camera_cut = true;
            current_only.render(door);
            const auto current = current_only.pixels();
            door.camera_cut = false;
            if (phase == 2) {
                require(mean_rgb_error(image, current, width, height,
                                       door_center) <= 1.0,
                        "Newly revealed center must be current background immediately");
            }
            if (phase >= 2) {
                std::size_t old_red_pixels{};
                for (auto y = door_edge.y; y < door_edge.y + door_edge.height; ++y)
                    for (auto x = door_edge.x; x < door_edge.x + door_edge.width; ++x) {
                        const auto base = (std::size_t(y) * width + x) * 4;
                        old_red_pixels += image[base] > image[base + 1] + 20 &&
                            image[base] > image[base + 2] + 20 && image[base] > 25;
                    }
                reveal_quality &= old_red_pixels == 0;
            }
            if (phase == 3) {
                const auto versus_current = pixels_over_error(
                    image, current, width, door_edge, 8);
                const auto versus_steady = pixels_over_error(
                    image, steady_background, width, door_edge, 8);
                const auto mean_steady = mean_rgb_error(
                    image, steady_background, width, height, door_edge);
                std::cerr << "Door edge >8: current-only=" << versus_current
                          << " steady-background=" << versus_steady
                          << " mean=" << mean_steady << '\n';
                // The remaining difference must be confined to a small AA edge,
                // not a colored image of the old door in the exposed center.
                reveal_quality &= versus_steady <= 32 && mean_steady <= .75;
            }
        }
        require(wire_quality,
                "Temporal wire stabilization must retain coverage energy and contrast");
        require(reveal_quality,
                "The opened door must converge to an unobstructed temporal background");
    }
}

void capture_quality_sequences(const std::filesystem::path& output) {
    std::filesystem::create_directories(output / "captures");
    std::ofstream csv(output / "frames.csv");
    require(bool(csv), "Could not open temporal quality frame CSV");
    csv << "sequence,mode,phase,width,height,internal_width,internal_height,device,"
           "cpu_ms,gpu_ms,readback_cpu_ms,gpu_main_raster_ms,gpu_post_raster_ms,"
           "gpu_temporal_resolve_ms,gpu_temporal_composite_ms,gpu_ui_ms,"
           "gpu_allocated_bytes,history_valid,reset_reason,validation_errors,image\n";
    constexpr std::uint32_t width = 160, height = 120;
    struct Mode { const char* name; TemporalMode temporal; float scale; bool current_only; };
    constexpr std::array modes{
        Mode{"off", TemporalMode::Off, 1.f, false},
        Mode{"current", TemporalMode::TAA, 1.f, true},
        Mode{"taa", TemporalMode::TAA, 1.f, false},
        Mode{"upscale-current", TemporalMode::Upscale, .67f, true},
        Mode{"upscale", TemporalMode::Upscale, .67f, false}};
    struct Sequence { const char* name; unsigned frames; };
    constexpr std::array sequences{
        Sequence{"wire-static", 16}, Sequence{"pan", 16},
        Sequence{"moving-cube", 16}, Sequence{"door-background", 16},
        Sequence{"door-open", 4},
        Sequence{"cut", 2}, Sequence{"resize", 2},
        Sequence{"ui-alpha", 2}};
    for (const auto& mode : modes) {
        auto config = headless_config(width, height, VisibilityMode::Direct);
        config.temporal_mode = mode.temporal;
        config.render_scale = mode.scale;
        Renderer renderer(config);
        for (const auto& sequence : sequences) {
            for (unsigned phase = 0; phase < sequence.frames; ++phase) {
                const bool resized = std::string(sequence.name) == "resize" && phase == 1;
                const auto frame_width = resized ? 319u : width;
                const auto frame_height = resized ? 241u : height;
                if (renderer.width() != frame_width || renderer.height() != frame_height)
                    renderer.resize(frame_width, frame_height);
                auto frame = lit_scene(frame_width, frame_height);
                frame.view_id = sequence.name;
                frame.clear_color = {0, 0, 0, 1};
                frame.camera_cut = mode.current_only;
                const std::string name = sequence.name;
                if (name == "wire-static" || name == "pan") {
                    auto wire = cube({0, 0, 0}, {1, 1, 1, 1}, "wire");
                    wire.model = transform({}, {0, 0, .35f}, {.025f, 1.4f, .05f});
                    frame.draws.push_back(wire);
                    if (name == "pan") {
                        frame.eye[0] = float(phase) * .012f;
                        frame.view_projection = multiply(
                            frame.projection, look_at(frame.eye, {0, 0, 0}));
                    }
                } else if (name == "moving-cube") {
                    frame.draws.push_back(cube(
                        {-.5f + float(phase) * .065f, 0, 0},
                        {.9f, .7f, .2f, 1}, "moving"));
                    frame.draws.push_back(cube({0, 0, -2}, {.2f, .5f, .8f, 1},
                                               "moving-background"));
                } else if (name == "door-open" || name == "door-background") {
                    frame.draws.push_back(cube({0, 0, -2}, {.1f, .9f, .2f, 1},
                                               "door-background"));
                    if (name == "door-open")
                        frame.draws.push_back(cube(
                            {phase < 2 ? 0.f : 3.f, 0, 1},
                            {.9f, .1f, .1f, 1}, "door"));
                } else {
                    frame.draws.push_back(cube({0, 0, 0}, {.8f, .6f, .25f, 1},
                                               "static-cube"));
                    if (name == "cut" && phase == 1) {
                        frame.camera_cut = true;
                        frame.eye = {1, 0, 6};
                        frame.view_projection = multiply(
                            frame.projection, look_at(frame.eye, {0, 0, 0}));
                    }
                    if (name == "ui-alpha") {
                        frame.draws.push_back(cube({.4f, 0, 1},
                                                   {.2f, .6f, .9f, .45f}, "alpha"));
                        frame.ui_quads.push_back({2, 2, 25, 12,
                                                  {.8f, .7f, .25f, 1}});
                    }
                }
                renderer.render(frame);
                const auto& stats = renderer.stats();
                require(stats.validation_errors == 0,
                        "Temporal quality capture reported Vulkan validation errors");
                std::ostringstream filename;
                filename << sequence.name << '-' << mode.name << '-'
                         << std::setfill('0') << std::setw(2) << phase << ".ppm";
                const auto image = (std::filesystem::path("captures") /
                                    filename.str()).generic_string();
                renderer.capture(output / image);
                csv << sequence.name << ',' << mode.name << ',' << phase << ','
                    << frame_width << ',' << frame_height << ','
                    << stats.temporal_internal_width << ','
                    << stats.temporal_internal_height << ','
                    << std::quoted(stats.device) << ','
                    << stats.cpu_ms << ',' << stats.gpu_ms << ','
                    << stats.readback_cpu_ms << ','
                    << stats.gpu_main_raster_ms << ','
                    << stats.gpu_post_raster_ms << ','
                    << stats.gpu_temporal_resolve_ms << ','
                    << stats.gpu_temporal_composite_ms << ','
                    << stats.gpu_ui_ms << ','
                    << stats.gpu_allocated_bytes << ','
                    << int(stats.temporal_history_valid) << ','
                    << int(stats.temporal_reset_reason) << ','
                    << stats.validation_errors << ',' << image << '\n';
            }
        }
    }
}

void profile_720p(const std::filesystem::path& output) {
    std::ofstream csv(output);
    require(bool(csv), "Could not open temporal 720p profile CSV");
    csv << "mode,frame,width,height,internal_width,internal_height,device,"
           "cpu_ms,gpu_ms,readback_cpu_ms,gpu_main_raster_ms,"
           "gpu_temporal_resolve_ms,gpu_temporal_composite_ms,gpu_ui_ms,"
           "gpu_allocated_bytes,validation_errors\n";
    constexpr std::uint32_t width = 1280, height = 720;
    constexpr std::array mode_names{"off", "taa", "upscale"};
    std::array<std::unique_ptr<Renderer>, 3> renderers;
    for (std::size_t i = 0; i < renderers.size(); ++i) {
        auto config = headless_config(width, height, VisibilityMode::Direct);
        config.temporal_mode = i == 0 ? TemporalMode::Off
            : i == 1 ? TemporalMode::TAA : TemporalMode::Upscale;
        config.render_scale = i == 2 ? .67f : 1.f;
        renderers[i] = std::make_unique<Renderer>(config);
    }
    auto frame = lit_scene(width, height);
    frame.view_id = "temporal-720p-fixed-scene";
    frame.clear_color = {0, 0, 0, 1};
    frame.draws.push_back(cube({0, 0, 0}, {.8f, .6f, .25f, 1}, "profile-cube"));
    auto wire = cube({0, 0, -1}, {1, 1, 1, 1}, "profile-wire");
    wire.model = transform({.9f, 0, -1}, {0, 0, .35f}, {.025f, 1.4f, .05f});
    frame.draws.push_back(wire);
    for (unsigned phase = 0; phase < 40; ++phase)
        for (std::size_t sequence = 0; sequence < renderers.size(); ++sequence) {
            const std::size_t i = (sequence + phase) % renderers.size();
            renderers[i]->render(frame);
            const auto& stats = renderers[i]->stats();
            require(stats.validation_errors == 0,
                    "720p temporal profile reported Vulkan validation errors");
            if (phase < 10)
                continue;
            csv << mode_names[i] << ',' << (phase - 10) << ','
                << width << ',' << height << ','
                << stats.temporal_internal_width << ','
                << stats.temporal_internal_height << ','
                << std::quoted(stats.device) << ','
                << stats.cpu_ms << ',' << stats.gpu_ms << ','
                << stats.readback_cpu_ms << ','
                << stats.gpu_main_raster_ms << ','
                << stats.gpu_temporal_resolve_ms << ','
                << stats.gpu_temporal_composite_ms << ','
                << stats.gpu_ui_ms << ','
                << stats.gpu_allocated_bytes << ','
                << stats.validation_errors << '\n';
        }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--profile-720p") {
        profile_720p(argv[2]);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--capture-quality") {
        capture_quality_sequences(argv[2]);
        return 0;
    }
    require(argc == 1,
            "Usage: temporal acceptance [--capture-quality DIR|--profile-720p CSV]");
    moving_reveal_and_camera_resets(VisibilityMode::Direct);
    moving_reveal_and_camera_resets(VisibilityMode::GpuFrustum);
    moving_reveal_and_camera_resets(VisibilityMode::GpuOcclusion);
    lower_resolution_scene_and_output_ui();
    static_edge_reduces_jitter_variation();
    temporal_tiled_lighting_uses_scene_raster_extent();
    tile_membership_tracks_raster_jitter();
    occlusion_upscale_hzb_debug_uses_internal_extent();
    preserve_thin_wire_contrast_without_reveal_halo();
}
