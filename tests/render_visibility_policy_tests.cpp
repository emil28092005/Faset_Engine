#include <faset/render/renderer.hpp>
#include <faset/render/visibility.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace faset::render;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.0001f;
}
template <class F> void rejects(F&& action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, message);
}
void bounds_cover_rotated_and_reflected_mesh() {
    Mesh mesh;
    mesh.vertices = {{{0, 0, 0}}, {{2, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}}, {{2, 1, 1}}};
    const auto model = transform({5, 7, 11}, {0, 0, 1.57079632679f}, {-2, 3, 4});
    const auto box = transformed_bounds(mesh, model);
    require(near(box.min[0], 2) && near(box.max[0], 5) && near(box.min[1], 3) &&
                near(box.max[1], 7) && near(box.min[2], 11) && near(box.max[2], 15),
            "Rotated and reflected mesh must have an enclosing world AABB");
    mesh.vertices[0].position[0] = std::numeric_limits<float>::infinity();
    rejects([&] { transformed_bounds(mesh, model); },
            "Nonfinite geometry cannot produce a conservative culling bound");
}
void ids_track_rendered_frames_and_generation() {
    auto meshA = std::make_shared<Mesh>();
    auto meshB = std::make_shared<Mesh>();
    const Bounds box{{-1, -1, -1}, {1, 1, 1}};
    const auto moved = transform({3, 0, 0});
    InstanceTracker tracker;
    const auto a0 = tracker.update("a", meshA.get(), identity, box, "game");
    const auto b0 = tracker.update("b", meshA.get(), identity, box, "game");
    require(a0.slot != b0.slot && !a0.previous_valid && !b0.previous_valid,
            "New instances have unique slots and no temporal state");
    const auto duplicate = tracker.update("a", meshA.get(), moved, box, "game");
    require(!duplicate.previous_valid,
            "An update before frame completion cannot become previous rendered state");
    tracker.finish_frame();

    const auto b1 = tracker.update("b", meshA.get(), moved, box, "game");
    const auto a1 = tracker.update("a", meshA.get(), moved, box, "game");
    require(a1.slot == a0.slot && b1.slot == b0.slot && a1.previous_valid &&
                b1.previous_valid && a1.previous_model == moved && b1.previous_model == identity,
            "Reordering does not change slots or the last rendered transforms");
    tracker.finish_frame();

    const auto changed = tracker.update("b", meshB.get(), moved, box, "game");
    require(changed.slot == b0.slot && changed.generation != b0.generation &&
                !changed.previous_valid,
            "Replacing mesh identity invalidates temporal state and bumps generation");
    tracker.finish_frame(); // a was absent and is now retired.
    const auto c0 = tracker.update("c", meshA.get(), identity, box, "game");
    require(c0.slot == a0.slot && c0.generation != a0.generation && !c0.previous_valid,
            "Reused slots cannot inherit a removed object's history");
    tracker.finish_frame();
    const auto c1 = tracker.update("c", meshA.get(), identity, box, "game");
    require(c1.previous_valid, "Same instance in a completed second frame may use history");
    tracker.finish_frame();
    tracker.invalidate_view("game");
    const auto cut = tracker.update("c", meshA.get(), identity, box, "game");
    require(!cut.previous_valid, "Camera cut invalidates view history");
    tracker.finish_frame();
    const auto switched = tracker.update("c", meshA.get(), identity, box, "editor");
    require(!switched.previous_valid, "Different viewport cannot reuse another view's history");
}
void owned_mesh_identity_outlives_reimport_gap() {
    InstanceTracker tracker;
    auto old_mesh = std::make_shared<Mesh>();
    std::weak_ptr<Mesh> old_reference = old_mesh;
    const Bounds box{{-1, -1, -1}, {1, 1, 1}};
    const auto first = tracker.update("imported", old_mesh, identity, box, "game");
    tracker.finish_frame();
    old_mesh.reset();
    require(!old_reference.expired(),
            "Tracker must retain mesh identity until the instance is retired or replaced");
    auto replacement_mesh = std::make_shared<Mesh>();
    const auto replaced = tracker.update("imported", replacement_mesh, identity, box, "game");
    require(replaced.generation != first.generation && !replaced.previous_valid,
            "Reimported mesh cannot reuse the previous mesh's occlusion history");
}
void lod_thresholds_have_hysteresis_and_fallback() {
    const std::array thresholds{100.f, 25.f};
    require(select_lod(95, 0, thresholds, .1f) == 0 &&
                select_lod(89, 0, thresholds, .1f) == 1,
            "LOD downgrade waits for the lower hysteresis edge");
    require(select_lod(105, 1, thresholds, .1f) == 1 &&
                select_lod(111, 1, thresholds, .1f) == 0,
            "LOD upgrade waits for the upper hysteresis edge");
    require(select_lod(24, 1, thresholds, .1f) == 1 &&
                select_lod(22, 1, thresholds, .1f) == 2 &&
                select_lod(27, 2, thresholds, .1f) == 2 &&
                select_lod(28, 2, thresholds, .1f) == 1,
            "Second threshold has the same hysteresis contract");
    require(select_lod(50, std::numeric_limits<std::size_t>::max(), thresholds, .1f) == 1,
            "An instance without a previous level chooses its size without hysteresis");
    const std::array<std::uint8_t, 3> sparse{1, 0, 1};
    require(select_lod(50, 0, thresholds, 0, sparse) == 0,
            "Missing requested LOD prefers the nearest available finer level");
    const std::array<std::uint8_t, 3> coarse_only{0, 0, 1};
    require(select_lod(500, 0, thresholds, 0, coarse_only) == 2,
            "Missing fine LODs fall back to available coarse geometry");
    const std::array<std::uint8_t, 3> none{0, 0, 0};
    rejects([&] { select_lod(50, 0, thresholds, .1f, none); },
            "No available mesh is a content error");
    rejects([&] { select_lod(std::numeric_limits<float>::quiet_NaN(), 0, thresholds, .1f); },
            "Nonfinite projected size cannot silently select a level");
}
void effective_mode_exposes_device_fallback() {
    require(select_effective_visibility_mode(VisibilityMode::Direct, true, true) ==
                VisibilityMode::Direct,
            "Direct request stays Direct even on a fully capable device");
    require(select_effective_visibility_mode(VisibilityMode::GpuFrustum, false, true) ==
                VisibilityMode::Direct,
            "Missing GPU culling profile falls back to Direct");
    require(select_effective_visibility_mode(VisibilityMode::GpuFrustum, true, false) ==
                VisibilityMode::GpuFrustum,
            "GPU frustum does not depend on HZB support");
    require(select_effective_visibility_mode(VisibilityMode::GpuOcclusion, true, false) ==
                VisibilityMode::GpuFrustum,
            "Missing HZB exposes a frustum-only effective mode");
    require(select_effective_visibility_mode(VisibilityMode::GpuOcclusion, true, true) ==
                VisibilityMode::GpuOcclusion,
            "Supported occlusion keeps the requested effective mode");
}
} // namespace
int main() {
    try {
        bounds_cover_rotated_and_reflected_mesh();
        ids_track_rendered_frames_and_generation();
        owned_mesh_identity_outlives_reimport_gap();
        lod_thresholds_have_hysteresis_and_fallback();
        effective_mode_exposes_device_fallback();
        std::cout << "Visibility bounds, instance identity and LOD policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
