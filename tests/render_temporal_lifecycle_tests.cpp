#include <faset/render/renderer.hpp>
#include <faset/render/temporal.hpp>

#include <memory>
#include <stdexcept>
#include <string>

using namespace faset::render;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

Snapshot scene(std::shared_ptr<const Mesh> mesh) {
    Snapshot frame;
    frame.view_id = "temporal-lifecycle-main";
    frame.eye = {0, 0, 6};
    frame.projection = perspective(.9f, 1.f, .1f, 50.f);
    frame.view_projection = multiply(frame.projection, look_at(frame.eye, {0, 0, 0}));
    DrawItem cube;
    cube.mesh = std::move(mesh);
    cube.instance_key = "rigid-cube";
    cube.cast_shadow = false;
    frame.draws.push_back(std::move(cube));
    return frame;
}

void previous_transform_does_not_depend_on_hzb(VisibilityMode visibility) {
    RendererConfig config;
    config.width = config.height = 128;
    config.headless = true;
    config.validation = true;
    config.visibility_mode = visibility;
    config.temporal_mode = TemporalMode::TAA;
    config.render_scale = 1.f;
    Renderer renderer(config);
    auto frame = scene(cube_mesh());

    renderer.render(frame);
    require(!renderer.stats().temporal_history_valid &&
                renderer.stats().temporal_valid_motion_instances == 0,
            "First frame must reject temporal color and previous transforms");
    frame.draws[0].model = transform({0.25f, 0, 0});
    renderer.render(frame);
    require(!renderer.stats().hzb_valid,
            "Direct and GPU frustum paths have no previous HZB in this fixture");
    require(renderer.stats().temporal_history_valid &&
                renderer.stats().temporal_valid_motion_instances == 1,
            "A stable opaque draw retains its previous model without HZB history");

    frame.draws[0].mesh = std::make_shared<Mesh>(*frame.draws[0].mesh);
    renderer.render(frame);
    require(renderer.stats().temporal_valid_motion_instances == 0,
            "Changing mesh identity invalidates prior geometry motion");
    require(renderer.stats().validation_errors == 0,
            "Temporal lifecycle must not trigger Vulkan validation errors");
}

void aborted_frame_cannot_advance_history() {
    RendererConfig config;
    config.width = config.height = 128;
    config.headless = true;
    config.visibility_mode = VisibilityMode::Direct;
    config.temporal_mode = TemporalMode::TAA;
    Renderer renderer(config);
    auto frame = scene(cube_mesh());
    renderer.render(frame);
    renderer.render(frame);
    require(renderer.stats().temporal_history_valid,
            "Fixture must have committed history before the aborted frame");
    const auto completed_frame = renderer.stats().frame;

    auto invalid = frame;
    invalid.draws[0].model = transform({1, 0, 0});
    invalid.local_lights.push_back(LocalLight{});
    invalid.local_lights.back().range = -1.f;
    bool rejected = false;
    try {
        renderer.render(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Late light validation must abort before command submission");
    require(renderer.stats().frame == completed_frame,
            "An aborted frame must not count as completed");

    frame.draws[0].model = transform({0.25f, 0, 0});
    renderer.render(frame);
    require(renderer.stats().temporal_history_valid &&
                renderer.stats().temporal_valid_motion_instances == 1,
            "The next successful frame must reuse the last committed transform/history");
}
} // namespace

int main() {
    previous_transform_does_not_depend_on_hzb(VisibilityMode::Direct);
    previous_transform_does_not_depend_on_hzb(VisibilityMode::GpuFrustum);
    aborted_frame_cannot_advance_history();
}
