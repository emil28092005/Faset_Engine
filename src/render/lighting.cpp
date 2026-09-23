#include <faset/render/lighting.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace faset::render {
namespace {
Vec3 add(Vec3 a, Vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 subtract(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 scale(Vec3 a, float factor) { return {a[0] * factor, a[1] * factor, a[2] * factor}; }
float dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 unit(Vec3 a) {
    const float magnitude = length(a);
    if (!std::isfinite(magnitude) || magnitude < 1e-6f)
        throw std::invalid_argument("Shadow light direction must be finite and nonzero");
    return scale(a, 1.f / magnitude);
}
Vec3 project(const Mat4& matrix, Vec3 value) {
    return {matrix[0] * value[0] + matrix[4] * value[1] + matrix[8] * value[2] + matrix[12],
            matrix[1] * value[0] + matrix[5] * value[1] + matrix[9] * value[2] + matrix[13],
            matrix[2] * value[0] + matrix[6] * value[1] + matrix[10] * value[2] + matrix[14]};
}
std::array<float, 4> clip(const Mat4& matrix, Vec3 value) {
    return {matrix[0] * value[0] + matrix[4] * value[1] + matrix[8] * value[2] + matrix[12],
            matrix[1] * value[0] + matrix[5] * value[1] + matrix[9] * value[2] + matrix[13],
            matrix[2] * value[0] + matrix[6] * value[1] + matrix[10] * value[2] + matrix[14],
            matrix[3] * value[0] + matrix[7] * value[1] + matrix[11] * value[2] + matrix[15]};
}
std::array<Vec3, 8> corners(const Bounds& bounds) {
    std::array<Vec3, 8> result{};
    for (unsigned i = 0; i < 8; ++i)
        result[i] = {i & 1 ? bounds.max[0] : bounds.min[0],
                     i & 2 ? bounds.max[1] : bounds.min[1],
                     i & 4 ? bounds.max[2] : bounds.min[2]};
    return result;
}
Vec3 camera_to_world(const Mat4& view, Vec3 camera) {
    // CameraFrustum::view is an unscaled, orthonormal look_at matrix.
    return {view[0] * (camera[0] - view[12]) + view[1] * (camera[1] - view[13]) +
                view[2] * (camera[2] - view[14]),
            view[4] * (camera[0] - view[12]) + view[5] * (camera[1] - view[13]) +
                view[6] * (camera[2] - view[14]),
            view[8] * (camera[0] - view[12]) + view[9] * (camera[1] - view[13]) +
                view[10] * (camera[2] - view[14])};
}
std::array<Vec3, 8> frustum_slice(const CameraFrustum& camera, float near_distance,
                                  float far_distance) {
    std::array<Vec3, 8> result{};
    for (unsigned i = 0; i < 8; ++i) {
        const float distance = i & 4 ? far_distance : near_distance;
        const float x = i & 1 ? 1.f : -1.f;
        const float y = i & 2 ? 1.f : -1.f;
        Vec3 local{};
        if (camera.perspective)
            local = {x * distance / camera.projection[0],
                     y * distance / camera.projection[5], -distance};
        else
            local = {(x - camera.projection[12]) / camera.projection[0],
                     (y - camera.projection[13]) / camera.projection[5], -distance};
        result[i] = camera_to_world(camera.view, local);
    }
    return result;
}
bool overlaps_xy(const Bounds& bounds, const Mat4& light_view, float left, float right,
                 float bottom, float top) {
    float min_x = std::numeric_limits<float>::infinity();
    float max_x = -min_x, min_y = min_x, max_y = -min_x;
    for (const auto point : corners(bounds)) {
        const auto light = project(light_view, point);
        min_x = std::min(min_x, light[0]);
        max_x = std::max(max_x, light[0]);
        min_y = std::min(min_y, light[1]);
        max_y = std::max(max_y, light[1]);
    }
    return max_x >= left && min_x <= right && max_y >= bottom && min_y <= top;
}
bool intersects_frustum(const Bounds& bounds, const Mat4& view_projection) {
    std::array<unsigned, 7> rejected{};
    for (const auto point : corners(bounds)) {
        const auto p = clip(view_projection, point);
        if (!std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); }))
            return true; // Invalid projection fails open so no caster is lost silently.
        rejected[0] += p[0] < -p[3];
        rejected[1] += p[0] > p[3];
        rejected[2] += p[1] < -p[3];
        rejected[3] += p[1] > p[3];
        rejected[4] += p[2] < 0;
        rejected[5] += p[2] > p[3];
        rejected[6] += p[3] <= 0;
    }
    return std::none_of(rejected.begin(), rejected.end(), [](unsigned count) { return count == 8; });
}
void tile(ShadowView& view, std::uint32_t atlas_size, std::uint32_t tiles_across,
          std::uint32_t index) {
    constexpr std::uint32_t guard = 2;
    const auto size = atlas_size / tiles_across;
    view.tile_index = index;
    view.tile_origin_x = (index % tiles_across) * size;
    view.tile_origin_y = (index / tiles_across) * size;
    view.tile_size = size;
    view.usable_size = size - 2 * guard;
    const auto reciprocal = 1.f / float(atlas_size);
    view.atlas_scale_offset = {float(view.usable_size) * reciprocal,
                               float(view.usable_size) * reciprocal,
                               float(view.tile_origin_x + guard) * reciprocal,
                               float(view.tile_origin_y + guard) * reciprocal};
    view.guarded_clamp = {(float(view.tile_origin_x + guard) + 1.5f) * reciprocal,
                          (float(view.tile_origin_y + guard) + 1.5f) * reciprocal,
                          (float(view.tile_origin_x + size - guard) - 1.5f) * reciprocal,
                          (float(view.tile_origin_y + size - guard) - 1.5f) * reciprocal};
}
std::vector<std::uint32_t> visible_casters(const Mat4& view_projection,
                                           std::span<const ShadowCasterBounds> casters) {
    std::vector<std::uint32_t> result;
    for (const auto& caster : casters)
        if (intersects_frustum(caster.world, view_projection))
            result.push_back(caster.draw_index);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
bool supported_atlas(std::uint32_t size, bool available) {
    return available && (size == 2048 || size == 1024);
}
void validate_local(const LocalLight& light) {
    const auto invalid = [&](const char* field) {
        throw std::invalid_argument("Local light " + light.stable_id + " has invalid " + field);
    };
    if (light.stable_id.empty())
        invalid("stable_id");
    if (!std::all_of(light.position.begin(), light.position.end(),
                     [](float v) { return std::isfinite(v); }))
        invalid("position");
    if (!std::all_of(light.color.begin(), light.color.end(),
                     [](float v) { return std::isfinite(v) && v >= 0; }))
        invalid("color");
    if (!std::isfinite(light.intensity) || light.intensity < 0)
        invalid("intensity");
    if (!std::isfinite(light.range) || light.range <= 0)
        invalid("range");
    if (light.kind == LocalLight::Kind::Spot) {
        if (!std::isfinite(light.inner_angle) || !std::isfinite(light.outer_angle) ||
            light.inner_angle < 0 || light.inner_angle > light.outer_angle ||
            light.outer_angle >= std::numbers::pi_v<float> / 2 || light.outer_angle <= 0)
            invalid("inner_angle/outer_angle");
        if (!std::all_of(light.direction.begin(), light.direction.end(),
                         [](float v) { return std::isfinite(v); }) ||
            length(light.direction) < 1e-6f)
            invalid("direction");
    }
}
float projected_influence(const LocalLight& light, const Snapshot& frame) {
    const auto distance = length(subtract(light.position, frame.eye));
    const auto projection_scale =
        std::max(std::abs(frame.projection[0]), std::abs(frame.projection[5]));
    return light.range * projection_scale / std::max(distance, .1f);
}
ShadowView sun_view(const Snapshot& frame, const SunLight& sun,
                    std::span<const ShadowCasterBounds> casters, float split_near,
                    float split_far, std::uint32_t index, std::uint32_t atlas_size) {
    ShadowView result;
    result.kind = ShadowView::Kind::Sun;
    result.light_id = sun.stable_id;
    result.face_index = index;
    result.split_near = split_near;
    result.split_far = split_far;
    tile(result, atlas_size, 2, index);
    const auto direction = unit(sun.direction);
    const auto up = std::abs(direction[1]) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const auto light_origin_view = look_at({0, 0, 0}, direction, up);
    if (!frame.camera_frustum) {
        const auto light_eye = scale(direction, -30);
        result.view_projection = multiply(orthographic(-20, 20, -20, 20, .1f, 80),
                                          look_at(light_eye, {0, 0, 0}, up));
        result.caster_indices = visible_casters(result.view_projection, casters);
        return result;
    }
    const auto receivers = frustum_slice(*frame.camera_frustum, split_near, split_far);
    Vec3 center{};
    for (const auto corner : receivers)
        center = add(center, scale(corner, 1.f / 8));
    float radius{};
    for (const auto corner : receivers)
        radius = std::max(radius, length(subtract(corner, center)));
    radius = std::max(.25f, std::ceil(radius * 16.f) / 16.f);
    const auto center_light = project(light_origin_view, center);
    const auto texel = (2 * radius) / float(result.usable_size);
    result.snapped_center_x = std::round(center_light[0] / texel) * texel;
    result.snapped_center_y = std::round(center_light[1] / texel) * texel;
    const float left = result.snapped_center_x - radius;
    const float right = result.snapped_center_x + radius;
    const float bottom = result.snapped_center_y - radius;
    const float top = result.snapped_center_y + radius;
    float nearest_ray = std::numeric_limits<float>::infinity();
    float furthest_ray = -nearest_ray;
    for (const auto corner : receivers) {
        const auto ray = dot(corner, direction);
        nearest_ray = std::min(nearest_ray, ray);
        furthest_ray = std::max(furthest_ray, ray);
    }
    for (const auto& caster : casters) {
        if (!overlaps_xy(caster.world, light_origin_view, left, right, bottom, top))
            continue;
        result.caster_indices.push_back(caster.draw_index);
        for (const auto corner : corners(caster.world)) {
            const auto ray = dot(corner, direction);
            nearest_ray = std::min(nearest_ray, ray);
            furthest_ray = std::max(furthest_ray, ray);
        }
    }
    std::sort(result.caster_indices.begin(), result.caster_indices.end());
    result.caster_indices.erase(
        std::unique(result.caster_indices.begin(), result.caster_indices.end()),
        result.caster_indices.end());
    const auto light_eye = scale(direction, nearest_ray - 1.f);
    const auto view = look_at(light_eye, add(light_eye, direction), up);
    const auto depth = std::max(2.f, furthest_ray - nearest_ray + 2.f);
    result.view_projection = multiply(orthographic(left, right, bottom, top, .1f, depth),
                                      view);
    return result;
}
ShadowView local_view(const LocalLight& light, std::uint32_t face,
                      std::span<const ShadowCasterBounds> casters) {
    ShadowView result;
    result.kind = light.kind == LocalLight::Kind::Point ? ShadowView::Kind::Point
                                                        : ShadowView::Kind::Spot;
    result.light_id = light.stable_id;
    result.face_index = face;
    static constexpr std::array<Vec3, 6> axes{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                                {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    static constexpr std::array<Vec3, 6> ups{{{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                               {0, 0, -1}, {0, -1, 0}, {0, -1, 0}}};
    const auto direction = light.kind == LocalLight::Kind::Point ? axes.at(face)
                                                                 : unit(light.direction);
    const auto up = light.kind == LocalLight::Kind::Point ? ups.at(face)
                    : std::abs(direction[1]) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const auto near_plane = std::max(.0001f, std::min(.05f, light.range * .1f));
    // Slight face overlap keeps the dominant-axis choice inside both adjacent
    // projections at a cubemap seam; the guarded tile still prevents PCF bleed.
    const auto fov = light.kind == LocalLight::Kind::Point
        ? std::numbers::pi_v<float> / 2 + .04f : light.outer_angle * 2;
    result.view_projection = multiply(
        perspective(fov, 1, near_plane, light.range),
        look_at(light.position, add(light.position, direction), up));
    result.caster_indices = visible_casters(result.view_projection, casters);
    return result;
}
} // namespace

ShadowPlan build_shadow_plan(const Snapshot& frame,
                             std::span<const ShadowCasterBounds> casters,
                             const ShadowBudget& budget) {
    for (const auto& caster : casters)
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(caster.world.min[axis]) ||
                !std::isfinite(caster.world.max[axis]) ||
                caster.world.min[axis] > caster.world.max[axis])
                throw std::invalid_argument("Shadow caster world bounds must be finite and ordered");
    ShadowPlan plan;
    plan.sun_atlas_size = supported_atlas(budget.sun_atlas_size, budget.sun_atlas_available)
        ? budget.sun_atlas_size : 0;
    plan.local_atlas_size = supported_atlas(budget.local_atlas_size, budget.local_atlas_available)
        ? budget.local_atlas_size : 0;
    std::unordered_set<std::string> ids;
    std::vector<std::pair<std::size_t, float>> ranked;
    ranked.reserve(frame.local_lights.size());
    for (std::size_t i = 0; i < frame.local_lights.size(); ++i) {
        const auto& light = frame.local_lights[i];
        validate_local(light); // Validate overflow records too, before truncation.
        if (!ids.insert(light.stable_id).second)
            throw std::invalid_argument("Duplicate local light stable_id: " + light.stable_id);
        ranked.emplace_back(i, projected_influence(light, frame));
    }
    std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
        const auto& left = frame.local_lights[a.first];
        const auto& right = frame.local_lights[b.first];
        if (left.shadow_priority != right.shadow_priority)
            return left.shadow_priority > right.shadow_priority;
        if (a.second != b.second)
            return a.second > b.second;
        return left.stable_id < right.stable_id;
    });
    const auto selected = std::min<std::size_t>(ranked.size(),
                                                std::min(budget.max_local_lights, 128u));
    plan.omitted_local_lights = static_cast<std::uint32_t>(ranked.size() - selected);
    for (std::size_t i = 0; i < selected; ++i)
        plan.submitted_local_indices.push_back(ranked[i].first);

    std::optional<SunLight> sun = frame.sun;
    if (!sun && !frame.authored_lights_present && frame.local_lights.empty())
        sun = SunLight{"legacy-sun", frame.light_direction, {1, 1, 1, 1}, 1, true};
    if (sun && sun->casts_shadow) {
        plan.requested_sun_cascades = frame.camera_frustum
            ? std::min(4u, budget.max_sun_views) : 1u;
        if (frame.camera_frustum &&
            (frame.camera_frustum->near_plane <= 0 ||
             frame.camera_frustum->far_plane <= frame.camera_frustum->near_plane ||
             frame.camera_frustum->projection[0] == 0 ||
             frame.camera_frustum->projection[5] == 0))
            throw std::invalid_argument("Shadow camera frustum is invalid");
        const float near_plane = frame.camera_frustum ? frame.camera_frustum->near_plane : .1f;
        const float far_plane = frame.camera_frustum
            ? std::min(frame.camera_frustum->far_plane, budget.max_shadow_distance) : 80.f;
        if (far_plane <= near_plane)
            throw std::invalid_argument("Shadow distance does not reach the camera near plane");
        float previous = near_plane;
        for (std::uint32_t i = 0; i < plan.requested_sun_cascades; ++i) {
            const auto ratio = float(i + 1) / float(plan.requested_sun_cascades);
            const auto logarithmic = near_plane * std::pow(far_plane / near_plane, ratio);
            const auto uniform = near_plane + (far_plane - near_plane) * ratio;
            const auto split = i + 1 == plan.requested_sun_cascades
                ? far_plane : .5f * (logarithmic + uniform);
            ShadowView view;
            if (plan.sun_atlas_size)
                view = sun_view(frame, *sun, casters, previous, split, i, plan.sun_atlas_size);
            else {
                view.kind = ShadowView::Kind::Sun;
                view.light_id = sun->stable_id;
                view.face_index = i;
                view.split_near = previous;
                view.split_far = split;
                view.reason = ShadowDropReason::Unavailable;
            }
            if (plan.sun_atlas_size &&
                view.caster_indices.size() <=
                    budget.max_caster_draws - std::min(plan.caster_draws, budget.max_caster_draws)) {
                view.valid = true;
                plan.caster_draws += static_cast<std::uint32_t>(view.caster_indices.size());
                ++plan.effective_sun_cascades;
            } else {
                if (plan.sun_atlas_size)
                    view.reason = ShadowDropReason::CasterBudget;
                view.caster_indices.clear();
                ++plan.dropped_sun_views;
            }
            plan.sun_views.push_back(std::move(view));
            previous = split;
        }
    }
    for (const auto source : plan.submitted_local_indices) {
        const auto& light = frame.local_lights[source];
        LocalShadowAssignment assignment;
        assignment.source_index = source;
        assignment.first_view = static_cast<std::uint32_t>(plan.local_views.size());
        const auto faces = light.kind == LocalLight::Kind::Point ? 6u : 1u;
        if (!light.casts_shadow || light.intensity == 0) {
            plan.local_assignments.push_back(assignment);
            continue;
        }
        plan.local_faces_requested += faces;
        if (!plan.local_atlas_size)
            assignment.reason = ShadowDropReason::Unavailable;
        else if (const auto capacity = std::min(budget.max_local_faces, 16u);
                 faces > capacity - std::min(plan.local_faces_used, capacity))
            assignment.reason = ShadowDropReason::TileBudget;
        else {
            std::vector<ShadowView> group;
            std::uint32_t group_draws{};
            for (std::uint32_t face = 0; face < faces; ++face) {
                auto view = local_view(light, face, casters);
                tile(view, plan.local_atlas_size, 4,
                     plan.local_faces_used + face);
                group_draws += static_cast<std::uint32_t>(view.caster_indices.size());
                group.push_back(std::move(view));
            }
            if (group_draws > budget.max_caster_draws -
                                  std::min(plan.caster_draws, budget.max_caster_draws))
                assignment.reason = ShadowDropReason::CasterBudget;
            else {
                assignment.valid = true;
                assignment.face_count = faces;
                plan.local_faces_used += faces;
                plan.caster_draws += group_draws;
                for (auto& view : group) {
                    view.valid = true;
                    plan.local_views.push_back(std::move(view));
                }
            }
        }
        if (!assignment.valid) {
            plan.dropped_local_faces += faces;
            if (light.kind == LocalLight::Kind::Point)
                plan.dropped_point_faces += 6;
        }
        plan.local_assignments.push_back(assignment);
    }
    return plan;
}
} // namespace faset::render
