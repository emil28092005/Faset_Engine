#include "render_temporal_fixtures.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace faset::render;
using namespace faset::render::temporal_test;

namespace {
void pixel_decisions_are_diagnostic_only(VisibilityMode visibility) {
    constexpr std::uint32_t width = 96, height = 72;
    auto config = headless_config(width, height, visibility);
    config.temporal_mode = TemporalMode::TAA;
    Renderer renderer(config);
    Renderer reference(config);
    auto frame = lit_scene(width, height);
    frame.draws.push_back(cube({0, 0, 0}, {.9f, .4f, .2f, 1}, "diagnostic-cube"));

    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 0,
            "Normal TAA must not read back per-pixel history diagnostics");

    renderer.set_temporal_diagnostics(true);
    renderer.render(frame);
    reference.render(frame);
    const auto accepted = renderer.stats().temporal_accepted_pixels;
    const auto rejected = renderer.stats().temporal_rejected_pixels;
    require(renderer.stats().temporal_counters_valid && accepted > 0 &&
                accepted + rejected == width * height,
            "Requested TAA diagnostics count each scene pixel and reuse eligible history");
    require(renderer.pixels() == reference.pixels(),
            "Counting pixels must not alter the TAA image");

    frame.camera_cut = true;
    renderer.render(frame);
    reference.render(frame);
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == width * height,
            "A camera cut rejects every per-pixel history sample");

    frame.camera_cut = false;
    frame.scene_rect = {7, 9, 80, 50};
    renderer.render(frame);
    reference.render(frame);
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 80 * 50,
            "The counter covers the scene rectangle, excluding output chrome pixels");

    renderer.set_temporal_diagnostics(false);
    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 0,
            "Disabled diagnostics expose no stale values and perform no readback");

    renderer.set_temporal_mode(TemporalMode::Off);
    reference.set_temporal_mode(TemporalMode::Off);
    renderer.set_temporal_diagnostics(true);
    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid,
            "Off mode does not claim temporal pixel diagnostics");
    require(renderer.stats().validation_errors == 0,
            "Diagnostic counter transitions pass Vulkan validation");
    require(renderer.pixels() == reference.pixels(),
            "Diagnostic toggles preserve the Off image");
}
} // namespace

int main() {
    try {
        pixel_decisions_are_diagnostic_only(VisibilityMode::Direct);
        pixel_decisions_are_diagnostic_only(VisibilityMode::GpuFrustum);
        std::cout << "Temporal pixel diagnostics are opt-in and exact\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
