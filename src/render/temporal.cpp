#include <faset/render/temporal.hpp>
#include <algorithm>
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

std::array<std::uint32_t, 2> temporal_internal_extent(std::uint32_t output_width,
                                                       std::uint32_t output_height,
                                                       TemporalMode mode, float render_scale) {
    if ((mode != TemporalMode::Off && mode != TemporalMode::TAA &&
         mode != TemporalMode::Upscale) ||
        !output_width || !output_height || !std::isfinite(render_scale) ||
        (mode == TemporalMode::Upscale
             ? render_scale < 0.5f || render_scale >= 1.f
             : render_scale != 1.f))
        throw std::invalid_argument("Invalid temporal mode, scale or output extent");
    if (mode != TemporalMode::Upscale)
        return {output_width, output_height};
    const auto scaled = [&](std::uint32_t extent) {
        return static_cast<std::uint32_t>(
            std::max(1.0, std::ceil(static_cast<double>(extent) * render_scale)));
    };
    return {scaled(output_width), scaled(output_height)};
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

std::array<float, 3> normalized_view_row(const TemporalHistoryKey& key, int row) noexcept {
    std::array<float, 3> direction{key.view_projection[row], key.view_projection[row + 4],
                                   key.view_projection[row + 8]};
    const float length_squared = direction[0] * direction[0] +
                                 direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(length_squared) || length_squared < 1e-12f)
        return {};
    const float reciprocal = 1.f / std::sqrt(length_squared);
    for (float& component : direction)
        component *= reciprocal;
    return direction;
}

bool large_axis_turn(const std::array<float, 3>& a,
                     const std::array<float, 3>& b) noexcept {
    if (a == std::array<float, 3>{} || b == std::array<float, 3>{})
        return true;
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return !std::isfinite(dot) || dot < 0.70710678f; // turn greater than 45 degrees
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
    // Compare horizontal and vertical camera axes too: comparing only forward
    // cannot detect a sudden roll about the unchanged viewing direction.
    for (int row = 0; row < 2; ++row)
        if (large_axis_turn(normalized_view_row(previous, row),
                            normalized_view_row(current, row)))
            return true;
    // Perspective VP encodes forward in row four. Orthographic projection has
    // a constant fourth row, so its third row carries the viewing direction.
    auto a = normalized_view_row(previous, 3);
    auto b = normalized_view_row(current, 3);
    if (a == std::array<float, 3>{} && b == std::array<float, 3>{}) {
        a = normalized_view_row(previous, 2);
        b = normalized_view_row(current, 2);
    }
    return large_axis_turn(a, b);
}
} // namespace

TemporalHistoryDecision
evaluate_temporal_history(const std::optional<TemporalHistoryKey>& previous,
                          const TemporalHistoryKey& current) noexcept {
    auto reset = [](TemporalResetReason reason) { return TemporalHistoryDecision{false, reason}; };
    if (current.mode == TemporalMode::Off)
        return reset(previous && previous->mode != TemporalMode::Off
                         ? TemporalResetReason::ModeChanged : TemporalResetReason::None);
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

std::optional<std::array<float, 2>>
project_motion(const std::array<float, 4>& current_clip,
               const std::array<float, 4>& previous_clip) noexcept {
    for (float coordinate : current_clip)
        if (!std::isfinite(coordinate))
            return std::nullopt;
    for (float coordinate : previous_clip)
        if (!std::isfinite(coordinate))
            return std::nullopt;
    if (current_clip[3] <= 0.f || previous_clip[3] <= 0.f)
        return std::nullopt;
    std::array<float, 2> motion{};
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const float current_uv = current_clip[axis] / current_clip[3] * 0.5f + 0.5f;
        const float previous_uv = previous_clip[axis] / previous_clip[3] * 0.5f + 0.5f;
        motion[axis] = current_uv - previous_uv;
        if (!std::isfinite(motion[axis]))
            return std::nullopt;
    }
    return motion;
}

} // namespace faset::render
