#include <faset/render/renderer.hpp>
#include <faset/render/temporal.hpp>
#include <faset/render/visibility.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

using namespace faset::render;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void motion_uses_current_minus_previous_scene_uv() {
    const auto motion = project_motion(std::array<float, 4>{0.2f, -0.2f, 0.6f, 1.f},
                                       std::array<float, 4>{0.f, 0.f, 0.4f, 1.f});
    require(motion && std::abs((*motion)[0] - 0.1f) < 1e-6f &&
                std::abs((*motion)[1] + 0.1f) < 1e-6f,
            "Motion sign and units must be current minus previous normalized scene UV");
    const auto perspective = project_motion(std::array<float, 4>{0.6f, 0.f, 0.8f, 2.f},
                                            std::array<float, 4>{0.2f, 0.f, 0.5f, 1.f});
    require(perspective && std::abs((*perspective)[0] - 0.05f) < 1e-6f,
            "Motion must divide each frame's clip coordinates by its own W");
    require(!project_motion({0, 0, 0, 1}, {0, 0, 0, 0}),
            "A previous vertex on the eye plane cannot carry valid motion");
    require(!project_motion({0, 0, 0, 1}, {0, 0, 0, -1}),
            "A previous vertex behind the camera cannot carry valid motion");
    require(!project_motion({INFINITY, 0, 0, 1}, {0, 0, 0, 1}),
            "Nonfinite clip coordinates cannot enter temporal history");
}

void hzb_and_color_history_have_independent_bits() {
    auto mesh = cube_mesh();
    auto replacement = std::make_shared<Mesh>(*mesh);
    const Bounds bounds{{-1, -1, -1}, {1, 1, 1}};
    InstanceTracker tracker;
    const auto model_a = transform({0, 0, 0});
    const auto model_b = transform({1, 0, 0});
    const auto first = tracker.update("cube", mesh, model_a, bounds, "main");
    require(gpu_instance_metadata(first, true, true)[0] == 0,
            "A first-frame tracked instance has neither prior HZB nor color history");
    tracker.finish_frame();
    const auto moved = tracker.update("cube", mesh, model_b, bounds, "main");
    require(moved.previous_valid && moved.previous_model == model_a,
            "A stable instance must retain its prior model in Direct or GPU mode");
    require(gpu_instance_metadata(moved, true, false)[0] == 1,
            "Only prior HZB eligibility sets bit zero");
    require(gpu_instance_metadata(moved, false, true)[0] == 2,
            "Temporal transform eligibility must not require HZB history");
    require(gpu_instance_metadata(moved, true, true)[0] == 3,
            "Both independent history bits may be valid in GPU occlusion mode");
    require(gpu_instance_metadata(moved, false, false)[0] == 0,
            "Neither history bit may leak after an incompatible frame");

    tracker.finish_frame();
    const auto replaced = tracker.update("cube", replacement, model_b, bounds, "main");
    require(!replaced.previous_valid && gpu_instance_metadata(replaced, true, true)[0] == 0,
            "A mesh identity change must reject both previous bounds and motion");
    require(gpu_instance_metadata({}, true, true)[0] == 0,
            "An anonymous draw cannot inherit another draw's transform");
}
} // namespace

int main() {
    motion_uses_current_minus_previous_scene_uv();
    hzb_and_color_history_have_independent_bits();
}
