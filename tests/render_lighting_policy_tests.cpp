#include <faset/render/lighting.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace faset::render;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
Snapshot fixture() {
    Snapshot frame;
    const Vec3 eye{0, 2, 8};
    const auto view = look_at(eye, {0, 0, 0});
    const auto projection = perspective(.9f, 16.f / 9.f, .1f, 120.f);
    frame.eye = eye;
    frame.view_projection = multiply(projection, view);
    frame.projection = projection;
    frame.camera_frustum = CameraFrustum{view, projection, .1f, 120.f, true};
    frame.authored_lights_present = true;
    frame.sun = SunLight{"sun", {-.7f, -.5f, -.3f}, {1, 1, 1, 1}, 1, true};
    return frame;
}
LocalLight point_light(std::string id, int priority = 0) {
    LocalLight light;
    light.kind = LocalLight::Kind::Point;
    light.stable_id = std::move(id);
    light.position = {0, 2, 0};
    light.range = 12;
    light.shadow_priority = priority;
    return light;
}
LocalLight spot_light(std::string id, int priority = 0) {
    auto light = point_light(std::move(id), priority);
    light.kind = LocalLight::Kind::Spot;
    light.direction = {0, -1, 0};
    return light;
}
bool contains_caster(const ShadowView& view, std::uint32_t draw_index) {
    return std::find(view.caster_indices.begin(), view.caster_indices.end(), draw_index) !=
           view.caster_indices.end();
}
void run() {
    auto frame = fixture();
    const auto plan = build_shadow_plan(frame, {}, {});
    require(plan.sun_views.size() == 4 && plan.effective_sun_cascades == 4,
            "Explicit camera receives four usable sun cascades");
    require(plan.sun_views[0].split_near == .1f &&
                std::abs(plan.sun_views.back().split_far - 80.f) < 1e-4f,
            "Practical splits start at camera near and stop at shadow distance");
    for (std::size_t i = 1; i < plan.sun_views.size(); ++i)
        require(plan.sun_views[i].split_near == plan.sun_views[i - 1].split_far &&
                    plan.sun_views[i].split_far > plan.sun_views[i].split_near,
                "Cascade split endpoints are strictly increasing and contiguous");
    require(plan.sun_views[0].tile_index == 0 && plan.sun_views[3].tile_index == 3 &&
                plan.sun_views[0].usable_size == 1020,
            "Four guarded 1024-square tiles fit a 2048-square sun atlas");
    auto shifted = frame;
    shifted.eye[0] += .00001f;
    const auto shifted_view = look_at(shifted.eye, {.00001f, 0, 0});
    shifted.camera_frustum->view = shifted_view;
    shifted.view_projection = multiply(shifted.projection, shifted_view);
    const auto stable = build_shadow_plan(shifted, {}, {});
    require(stable.sun_views[0].snapped_center_x == plan.sun_views[0].snapped_center_x &&
                stable.sun_views[0].snapped_center_y == plan.sun_views[0].snapped_center_y,
            "Subtexel camera translation retains the snapped sun projection origin");
    const auto sun_direction = frame.sun->direction;
    const auto inv_length = 1.f / std::hypot(sun_direction[0], sun_direction[1], sun_direction[2]);
    const Vec3 upstream{-sun_direction[0] * inv_length * 18,
                        -sun_direction[1] * inv_length * 18,
                        -sun_direction[2] * inv_length * 18};
    const ShadowCasterBounds offscreen{{{upstream[0] - .5f, upstream[1] - .5f,
                                          upstream[2] - .5f},
                                         {upstream[0] + .5f, upstream[1] + .5f,
                                          upstream[2] + .5f}}, 7};
    const ShadowCasterBounds outside{{{999, 0, 0}, {1001, 2, 2}}, 8};
    const std::array casters{offscreen, outside};
    const auto with_casters = build_shadow_plan(frame, casters, {});
    require(std::any_of(with_casters.sun_views.begin(), with_casters.sun_views.end(),
                        [](const auto& view) { return contains_caster(view, 7); }),
            "Offscreen upstream caster remains in a receiver's sun shadow view");
    require(std::none_of(with_casters.sun_views.begin(), with_casters.sun_views.end(),
                         [](const auto& view) { return contains_caster(view, 8); }),
            "Caster outside every sun XY footprint is excluded");
    std::vector<ShadowCasterBounds> many(4097, {{{-.1f, -.1f, -.1f}, {.1f, .1f, .1f}}, 0});
    for (std::uint32_t i = 0; i < many.size(); ++i)
        many[i].draw_index = i;
    const auto overdraw = build_shadow_plan(frame, many, {});
    require(overdraw.caster_draws <= 4096 &&
                std::any_of(overdraw.sun_views.begin(), overdraw.sun_views.end(),
                            [](const auto& view) {
                                return !view.valid && view.reason == ShadowDropReason::CasterBudget &&
                                       view.caster_indices.empty();
                            }),
            "A view with 4097 casters is skipped whole rather than partially rendered");
    frame.sun.reset();
    for (int i = 0; i < 15; ++i)
        frame.local_lights.push_back(spot_light("spot-" + std::to_string(i), 10));
    frame.local_lights.push_back(point_light("last-point"));
    const auto capacity = build_shadow_plan(frame, {}, {});
    require(capacity.local_faces_used == 15 && capacity.dropped_point_faces == 6 &&
                capacity.local_faces_used <= 16 && capacity.caster_draws <= 4096,
            "Insufficient room for six point faces drops the complete point shadow");
    require(capacity.submitted_local_indices.size() == 16 &&
                std::none_of(capacity.local_views.begin(), capacity.local_views.end(),
                             [](const auto& view) { return view.light_id == "last-point"; }),
            "Atlas overflow leaves the point light in the lighting list, unshadowed");
    frame.local_lights = {point_light("omnidirectional")};
    const ShadowCasterBounds positive_x{{{3, -.2f, -.2f}, {3.4f, .2f, .2f}}, 19};
    const auto point_faces = build_shadow_plan(frame, std::array{positive_x}, {});
    require(point_faces.local_views.size() == 6 &&
                contains_caster(point_faces.local_views[0], 19) &&
                !contains_caster(point_faces.local_views[1], 19),
            "Point-light caster behind the opposite face is culled from that face");
    frame.local_lights.clear();
    for (int i = 0; i < 15; ++i)
        frame.local_lights.push_back(spot_light("spot-" + std::to_string(i), 10));
    frame.local_lights.push_back(point_light("last-point"));
    auto reversed = frame;
    std::reverse(reversed.local_lights.begin(), reversed.local_lights.end());
    const auto reordered = build_shadow_plan(reversed, {}, {});
    require(reordered.local_views.size() == capacity.local_views.size(),
            "Reversing input lights preserves scheduled view count");
    for (std::size_t i = 0; i < capacity.local_views.size(); ++i)
        require(reordered.local_views[i].light_id == capacity.local_views[i].light_id &&
                    reordered.local_views[i].tile_index == capacity.local_views[i].tile_index,
                "Stable IDs preserve atlas assignments across input reordering");
    frame.local_lights.clear();
    for (int i = 0; i < 128; ++i) {
        auto light = spot_light("ordinary-" + std::to_string(i));
        light.casts_shadow = false;
        frame.local_lights.push_back(light);
    }
    auto important = spot_light("late-high-priority", 5);
    important.casts_shadow = false;
    frame.local_lights.push_back(important);
    const auto ranked = build_shadow_plan(frame, {}, {});
    require(ranked.submitted_local_indices.size() == 128 &&
                ranked.omitted_local_lights == 1 &&
                ranked.submitted_local_indices.front() == 128,
            "Submission selects all 128 by priority and reports one omitted light");
    frame.local_lights.back().range = -1;
    bool invalid_overflow_rejected = false;
    try {
        (void)build_shadow_plan(frame, {}, {});
    } catch (const std::invalid_argument&) {
        invalid_overflow_rejected = true;
    }
    require(invalid_overflow_rejected,
            "All authored lights are validated even when beyond the submission cap");
    frame.local_lights.clear();
    frame.sun.reset();
    const auto no_sun = build_shadow_plan(frame, {}, {});
    require(no_sun.requested_sun_cascades == 0 && no_sun.sun_views.empty(),
            "Authored lights suppress legacy sun even when none is enabled");
    for (int i = 0; i < 20; ++i)
        frame.local_lights.push_back(spot_light("over-cap-" + std::to_string(i)));
    ShadowBudget relaxed;
    relaxed.max_local_faces = 64;
    relaxed.max_local_lights = 256;
    const auto clamped = build_shadow_plan(frame, {}, relaxed);
    require(clamped.local_faces_used == 16 && clamped.dropped_local_faces == 4,
            "Fixed 4x4 local atlas never allocates outside its sixteen tiles");
    frame = fixture();
    frame.camera_frustum.reset();
    const auto legacy = build_shadow_plan(frame, {}, {});
    require(legacy.sun_views.size() == 1 && legacy.effective_sun_cascades == 1,
            "A low-level snapshot without camera frustum uses one reported sun view");
    frame = fixture();
    ShadowBudget unsupported;
    unsupported.sun_atlas_available = false;
    const auto no_atlas = build_shadow_plan(frame, {}, unsupported);
    require(no_atlas.effective_sun_cascades == 0 &&
                no_atlas.dropped_sun_views == 4 &&
                no_atlas.sun_views[0].reason == ShadowDropReason::Unavailable,
            "Unsupported depth atlas yields an explicit unshadowed sun fallback");
}
} // namespace
int main() {
    try {
        run();
        std::cout << "Shadow planning, stable allocation, caster visibility, and budgets passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
