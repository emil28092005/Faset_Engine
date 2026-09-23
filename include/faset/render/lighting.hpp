#pragma once
#include <faset/render/visibility.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace faset::render {

enum class ShadowDropReason { None, Unavailable, TileBudget, CasterBudget };
enum class ShadowRedrawReason { EveryFrame };

struct ShadowBudget {
    std::uint32_t max_sun_views{4};
    std::uint32_t max_local_faces{16};
    std::uint32_t max_caster_draws{4096};
    std::uint32_t max_local_lights{128};
    std::uint32_t sun_atlas_size{2048};
    std::uint32_t local_atlas_size{2048};
    float max_shadow_distance{80};
    bool sun_atlas_available{true};
    bool local_atlas_available{true};
};

struct ShadowCasterBounds {
    Bounds world;
    std::uint32_t draw_index{}; // Index of the original source LOD-0 DrawItem.
};

struct ShadowView {
    enum class Kind { Sun, Spot, Point };
    Kind kind{Kind::Sun};
    std::string light_id;
    std::uint32_t face_index{};
    std::uint32_t tile_index{};
    std::uint32_t tile_origin_x{}, tile_origin_y{}, tile_size{}, usable_size{};
    Mat4 view_projection{identity};
    std::array<float, 4> atlas_scale_offset{};
    std::array<float, 4> guarded_clamp{};
    float split_near{}, split_far{};
    float snapped_center_x{}, snapped_center_y{};
    std::vector<std::uint32_t> caster_indices;
    bool valid{};
    ShadowDropReason reason{ShadowDropReason::None};
    ShadowRedrawReason redraw_reason{ShadowRedrawReason::EveryFrame};
};

struct LocalShadowAssignment {
    std::size_t source_index{};
    std::uint32_t first_view{};
    std::uint32_t face_count{};
    bool valid{};
    ShadowDropReason reason{ShadowDropReason::None};
};

struct ShadowPlan {
    std::vector<ShadowView> sun_views; // Requested slots, including explicitly invalid ones.
    std::vector<ShadowView> local_views; // Only complete, valid spot/point allocations.
    std::vector<std::size_t> submitted_local_indices; // Priority/influence/stable-ID order.
    std::vector<LocalShadowAssignment> local_assignments;
    std::uint32_t requested_sun_cascades{}, effective_sun_cascades{};
    std::uint32_t local_faces_requested{}, local_faces_used{};
    std::uint32_t dropped_sun_views{}, dropped_local_faces{}, dropped_point_faces{};
    std::uint32_t omitted_local_lights{}, caster_draws{};
    std::uint32_t sun_atlas_size{}, local_atlas_size{};
};

// Stateless: every scheduled tile is cleared/redrawn; no prior-frame depth or
// ownership is reused. Shadow casters are source LOD-0 world bounds, independent
// of the camera/P2 visibility decision. This function performs no Vulkan work.
ShadowPlan build_shadow_plan(const Snapshot& frame,
                             std::span<const ShadowCasterBounds> casters,
                             const ShadowBudget& budget = {});

} // namespace faset::render
