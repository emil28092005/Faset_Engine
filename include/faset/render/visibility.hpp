#pragma once
#include <faset/render/renderer.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace faset::render {
struct Bounds {
    Vec3 min{};
    Vec3 max{};
};

// Mesh bounds are computed from all vertices; world bounds transform all eight
// corners of the local AABB, including rotation, reflection and nonuniform scale.
// Empty or nonfinite geometry/matrices are rejected instead of being culled.
Bounds local_bounds(const Mesh& mesh);
Bounds transformed_bounds(const Bounds& local, const Mat4& model);
Bounds transformed_bounds(const Mesh& mesh, const Mat4& model);

// Resolve a requested mode against the current device and target capabilities.
// Missing HZB retains GPU frustum culling, but callers must report that fallback.
VisibilityMode select_effective_visibility_mode(VisibilityMode requested,
                                                bool gpu_available,
                                                bool hzb_available) noexcept;

struct InstanceUpdate {
    std::uint32_t slot{};
    std::uint64_t generation{};
    Mat4 previous_model{identity};
    Bounds previous_bounds{};
    bool previous_valid{};
};

// GPU instance metadata: x bits 0/1 separately mark previous HZB bounds and
// previous temporal transform; the other lanes carry stable slot/generation.
// A zero generation identifies an anonymous, untracked draw.
inline constexpr std::uint32_t gpu_hzb_history_bit = 1U;
inline constexpr std::uint32_t gpu_temporal_history_bit = 2U;
constexpr std::array<std::uint32_t, 4>
gpu_instance_metadata(const InstanceUpdate& update, bool hzb_compatible,
                      bool temporal_compatible = false) noexcept {
    return {update.previous_valid
                ? (hzb_compatible ? gpu_hzb_history_bit : 0U) |
                      (temporal_compatible ? gpu_temporal_history_bit : 0U)
                : 0U,
            update.slot,
            static_cast<std::uint32_t>(update.generation),
            static_cast<std::uint32_t>(update.generation >> 32)};
}

// One update per logical instance per rendered frame. finish_frame() promotes the
// current state to *rendered* history and retires keys absent from that frame.
// Reordering the input never renumbers live instances. Mesh changes retain the
// slot but advance its generation; slot reuse also advances its generation.
class InstanceTracker {
  public:
    // Prefer this overload for engine-owned meshes. Retaining the owner prevents
    // allocator address reuse from impersonating the previously rendered mesh.
    InstanceUpdate update(std::string_view key, std::shared_ptr<const Mesh> mesh_identity,
                          const Mat4& model, const Bounds& world_bounds,
                          std::string_view view_id);
    // Non-owning overload for meshes whose lifetime is guaranteed by the caller.
    InstanceUpdate update(std::string_view key, const Mesh* mesh_identity, const Mat4& model,
                          const Bounds& world_bounds, std::string_view view_id);
    void finish_frame();
    void invalidate_view(std::string_view view_id);

  private:
    struct Record {
        std::uint32_t slot{};
        std::uint64_t generation{};
        const Mesh* mesh{};
        std::shared_ptr<const Mesh> mesh_owner{};
        Mat4 current_model{identity}, previous_model{identity};
        Bounds current_bounds{}, previous_bounds{};
        std::string current_view, previous_view;
        std::uint64_t seen_frame{};
        bool committed{};
    };
    std::unordered_map<std::string, Record> records_;
    std::vector<std::uint32_t> free_slots_;
    std::vector<std::uint64_t> slot_generations_;
    std::unordered_set<std::string> invalid_views_;
    std::uint64_t frame_{1};
};

// thresholds[i] is the projected-pixel boundary between levels i and i+1,
// ordered strictly high-to-low. Moving toward a coarser level requires size <
// threshold*(1-hysteresis); toward a finer level requires size >
// threshold*(1+hysteresis). SIZE_MAX means no prior level and uses raw thresholds.
// available is optional; when given it has thresholds.size()+1 entries, with 0
// marking missing prepared geometry. Nearest available wins; ties prefer finer.
std::size_t select_lod(float projected_pixels, std::size_t previous_level,
                       std::span<const float> thresholds, float hysteresis,
                       std::span<const std::uint8_t> available = {});
} // namespace faset::render
