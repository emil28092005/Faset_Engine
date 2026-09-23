#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace faset::render {

enum class TemporalMode { Off, TAA, Upscale };

struct TemporalCapabilities {
    bool compute{};
    bool formats{};
    bool extent{};
};

enum class TemporalFallbackReason {
    None,
    ComputeUnavailable,
    FormatUnavailable,
    ExtentUnsupported
};

TemporalFallbackReason temporal_fallback_reason(TemporalMode requested,
                                                TemporalCapabilities available) noexcept;
TemporalMode select_effective_temporal_mode(TemporalMode requested,
                                            TemporalCapabilities available) noexcept;

enum class TemporalResetReason {
    None,
    FirstFrame,
    CameraCut,
    CameraDiscontinuity,
    ViewChanged,
    ViewportChanged,
    ProjectionChanged,
    Resize,
    ModeChanged,
    ScaleChanged,
    ShaderReload,
    Unsupported
};

// Snapshot matrices/rect stay unjittered. This is independent of HZB history:
// Direct rendering and GPU frustum mode can still accumulate temporal color.
// A missing previous key means no completed frame is available to reuse.
struct TemporalHistoryKey {
    std::string view_id;
    std::uint32_t output_width{}, output_height{};
    std::uint32_t internal_width{}, internal_height{};
    std::array<float, 4> scene_rect{};
    std::array<float, 16> projection{}, view_projection{};
    std::array<float, 3> camera_eye{};
    TemporalMode mode{TemporalMode::Off};
    float render_scale{1};
    std::uint64_t shader_generation{};
    bool camera_cut{};
};

struct TemporalHistoryDecision {
    bool valid{};
    TemporalResetReason reason{TemporalResetReason::FirstFrame};
};

TemporalHistoryDecision
evaluate_temporal_history(const std::optional<TemporalHistoryKey>& previous,
                          const TemporalHistoryKey& current) noexcept;

// Sixteen-phase Halton(2,3) offset in clip-space units for scene rasterization.
// UI, picking and history keys use unjittered space. Culling must account for
// this jitter with a conservative edge; HZB depth must match jittered geometry.
// A zero viewport extent throws std::invalid_argument.
std::array<float, 2> temporal_jitter(std::uint64_t frame_index,
                                     std::uint32_t viewport_width,
                                     std::uint32_t viewport_height);

// Scene-local normalized UV motion, current minus previous. Both clips use
// their own jittered scene VP and the same local vertex. Invalid/behind-eye
// clips have no usable history; the shader writes velocity validity zero.
std::optional<std::array<float, 2>>
project_motion(const std::array<float, 4>& current_clip,
               const std::array<float, 4>& previous_clip) noexcept;

} // namespace faset::render
