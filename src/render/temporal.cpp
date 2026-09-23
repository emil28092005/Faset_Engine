#include <faset/render/temporal.hpp>
#include <cmath>
#include <stdexcept>

namespace faset::render {

TemporalFallbackReason temporal_fallback_reason(TemporalMode requested,
                                                TemporalCapabilities available) noexcept {
    if (requested == TemporalMode::Off)
        return TemporalFallbackReason::None;
    if (!available.compute)
        return TemporalFallbackReason::ComputeUnavailable;
    if (!available.formats)
        return TemporalFallbackReason::FormatUnavailable;
    if (!available.extent)
        return TemporalFallbackReason::ExtentUnsupported;
    return TemporalFallbackReason::None;
}

TemporalMode select_effective_temporal_mode(TemporalMode requested,
                                            TemporalCapabilities available) noexcept {
    return temporal_fallback_reason(requested, available) == TemporalFallbackReason::None
               ? requested : TemporalMode::Off;
}

namespace {
float halton(std::uint32_t index, std::uint32_t base) noexcept {
    float sample = 0.f;
    float place = 1.f / static_cast<float>(base);
    while (index != 0) {
        sample += static_cast<float>(index % base) * place;
        index /= base;
        place /= static_cast<float>(base);
    }
    return sample;
}

bool finite_camera(const TemporalHistoryKey& key) noexcept {
    for (float value : key.camera_eye)
        if (!std::isfinite(value))
            return false;
    for (float value : key.view_projection)
        if (!std::isfinite(value))
            return false;
    return true;
}

std::array<float, 3> view_direction(const TemporalHistoryKey& key) noexcept {
    // A perspective VP encodes view forward in its fourth row. An orthographic
    // projection has a constant fourth row; its third row carries direction.
    std::array<float, 3> direction{key.view_projection[3], key.view_projection[7],
                                   key.view_projection[11]};
    float length_squared = direction[0] * direction[0] + direction[1] * direction[1] +
                           direction[2] * direction[2];
    if (length_squared < 1e-12f) {
        direction = {key.view_projection[2], key.view_projection[6], key.view_projection[10]};
        length_squared = direction[0] * direction[0] + direction[1] * direction[1] +
                         direction[2] * direction[2];
    }
    if (!std::isfinite(length_squared) || length_squared < 1e-12f)
        return {};
    const float reciprocal = 1.f / std::sqrt(length_squared);
    for (float& component : direction)
        component *= reciprocal;
    return direction;
}

bool camera_discontinuity(const TemporalHistoryKey& previous,
                          const TemporalHistoryKey& current) noexcept {
    if (!finite_camera(previous) || !finite_camera(current))
        return true;
    float translation_squared{};
    for (int axis = 0; axis < 3; ++axis) {
        const float delta = current.camera_eye[axis] - previous.camera_eye[axis];
        translation_squared += delta * delta;
    }
    // A conservative world-space threshold catches unmarked teleports. Ordinary
    // camera motion and smaller view changes are handled by motion vectors.
    if (!std::isfinite(translation_squared) || translation_squared > 25.f)
        return true;
    const auto a = view_direction(previous), b = view_direction(current);
    if (a == std::array<float, 3>{} || b == std::array<float, 3>{})
        return true;
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return !std::isfinite(dot) || dot < 0.70710678f; // turn greater than 45 degrees
}
} // namespace

TemporalHistoryDecision
evaluate_temporal_history(const std::optional<TemporalHistoryKey>& previous,
                          const TemporalHistoryKey& current) noexcept {
    auto reset = [](TemporalResetReason reason) { return TemporalHistoryDecision{false, reason}; };
    if (current.camera_cut)
        return reset(TemporalResetReason::CameraCut);
    if (!previous)
        return reset(TemporalResetReason::FirstFrame);
    const auto& before = *previous;
    if (before.view_id != current.view_id)
        return reset(TemporalResetReason::ViewChanged);
    if (before.output_width != current.output_width ||
        before.output_height != current.output_height)
        return reset(TemporalResetReason::Resize);
    if (before.internal_width != current.internal_width ||
        before.internal_height != current.internal_height ||
        before.render_scale != current.render_scale)
        return reset(TemporalResetReason::ScaleChanged);
    if (before.mode != current.mode)
        return reset(TemporalResetReason::ModeChanged);
    if (current.mode == TemporalMode::Off)
        return reset(TemporalResetReason::Unsupported);
    if (before.scene_rect != current.scene_rect)
        return reset(TemporalResetReason::ViewportChanged);
    if (before.projection != current.projection)
        return reset(TemporalResetReason::ProjectionChanged);
    if (before.shader_generation != current.shader_generation)
        return reset(TemporalResetReason::ShaderReload);
    if (camera_discontinuity(before, current))
        return reset(TemporalResetReason::CameraDiscontinuity);
    return {true, TemporalResetReason::None};
}

std::array<float, 2> temporal_jitter(std::uint64_t frame_index,
                                     std::uint32_t viewport_width,
                                     std::uint32_t viewport_height) {
    if (viewport_width == 0 || viewport_height == 0)
        throw std::invalid_argument("temporal jitter requires a nonzero viewport extent");
    const auto phase = static_cast<std::uint32_t>(frame_index % 16) + 1;
    return {(halton(phase, 2) - 0.5f) * (2.f / static_cast<float>(viewport_width)),
            (halton(phase, 3) - 0.5f) * (2.f / static_cast<float>(viewport_height))};
}

} // namespace faset::render
