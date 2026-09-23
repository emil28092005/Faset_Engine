#include <faset/render/temporal.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {
using namespace faset::render;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void capability_fallback() {
    constexpr TemporalCapabilities full{true, true, true};
    require(select_effective_temporal_mode(TemporalMode::Off, {}) == TemporalMode::Off,
            "Off must not require temporal GPU capabilities");
    require(temporal_fallback_reason(TemporalMode::Off, {}) == TemporalFallbackReason::None,
            "Explicit Off is not a capability fallback");
    require(select_effective_temporal_mode(TemporalMode::TAA, full) == TemporalMode::TAA,
            "Available TAA must remain active");
    require(select_effective_temporal_mode(TemporalMode::Upscale, full) == TemporalMode::Upscale,
            "Available upscaling must remain active");
    require(select_effective_temporal_mode(TemporalMode::TAA, {false, true, true}) ==
                TemporalMode::Off &&
                temporal_fallback_reason(TemporalMode::TAA, {false, true, true}) ==
                    TemporalFallbackReason::ComputeUnavailable,
            "Missing compute must produce a named Off fallback");
    require(temporal_fallback_reason(TemporalMode::TAA, {true, false, true}) ==
                TemporalFallbackReason::FormatUnavailable,
            "Missing sampled/storage formats must identify the format fallback");
    require(temporal_fallback_reason(TemporalMode::Upscale, {true, true, false}) ==
                TemporalFallbackReason::ExtentUnsupported,
            "An unsupported target extent must identify the extent fallback");
}

TemporalHistoryKey steady_view() {
    TemporalHistoryKey key;
    key.view_id = "main-camera";
    key.output_width = key.internal_width = 320;
    key.output_height = key.internal_height = 240;
    key.scene_rect = {7, 11, 301, 219};
    key.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    key.view_projection = key.projection;
    key.mode = TemporalMode::TAA;
    key.render_scale = 1;
    key.shader_generation = 4;
    return key;
}

void rendered_history_and_camera_motion() {
    const auto previous = steady_view();
    auto current = previous;
    require(evaluate_temporal_history(std::nullopt, current).reason ==
                TemporalResetReason::FirstFrame,
            "A first temporal frame must not claim history");
    current.camera_eye = {0.5f, 0, 0};
    current.view_projection[12] = -0.5f;
    const auto ordinary_motion = evaluate_temporal_history(previous, current);
    require(ordinary_motion.valid && ordinary_motion.reason == TemporalResetReason::None,
            "Ordinary camera motion must retain compatible history");
    current.camera_cut = true;
    require(evaluate_temporal_history(previous, current).reason == TemporalResetReason::CameraCut,
            "An explicit cut must override otherwise compatible history");
    current.camera_cut = false;
    current.camera_eye = {6, 0, 0};
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::CameraDiscontinuity,
            "A large unmarked teleport must invalidate history");
    current = previous;
    current.view_projection[10] = -1;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::CameraDiscontinuity,
            "A large camera turn must invalidate history");
    current = previous;
    current.view_projection[0] = -1;
    current.view_projection[5] = -1;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::CameraDiscontinuity,
            "A 180-degree roll must invalidate history even when forward is unchanged");
    current = previous;
    current.view_projection[0] = 0.98480775f;
    current.view_projection[1] = 0.17364818f;
    current.view_projection[4] = -0.17364818f;
    current.view_projection[5] = 0.98480775f;
    require(evaluate_temporal_history(previous, current).valid,
            "An ordinary small camera roll must retain compatible history");
    current = previous;
    current.view_projection[0] = std::numeric_limits<float>::quiet_NaN();
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::CameraDiscontinuity,
            "Nonfinite camera matrices cannot admit history");
}

void incompatible_view_state() {
    const auto previous = steady_view();
    auto current = previous;
    current.view_id = "other-camera";
    require(evaluate_temporal_history(previous, current).reason == TemporalResetReason::ViewChanged,
            "A second view cannot inherit another view's color history");
    current = previous;
    ++current.output_width;
    require(evaluate_temporal_history(previous, current).reason == TemporalResetReason::Resize,
            "Output resize invalidates history");
    current = previous;
    current.scene_rect[0] += 1;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::ViewportChanged,
            "Moving the editor viewport invalidates history");
    current = previous;
    current.projection[0] += 0.1f;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::ProjectionChanged,
            "FOV or aspect change invalidates history");
    current = previous;
    current.mode = TemporalMode::Upscale;
    current.render_scale = 0.67f;
    current.internal_width = 215;
    current.internal_height = 161;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::ScaleChanged,
            "New internal render scale invalidates full-resolution history");
    current = previous;
    current.mode = TemporalMode::Off;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::ModeChanged,
            "Turning temporal processing off invalidates history");
    current = previous;
    ++current.shader_generation;
    require(evaluate_temporal_history(previous, current).reason ==
                TemporalResetReason::ShaderReload,
            "A changed shading generation invalidates history");
}

void intentionally_disabled_temporal_mode() {
    auto off = steady_view();
    off.mode = TemporalMode::Off;
    const auto first = evaluate_temporal_history(std::nullopt, off);
    require(!first.valid && first.reason == TemporalResetReason::None,
            "Explicit Off has no temporal history to reset or unsupported fallback to report");
    const auto later = evaluate_temporal_history(off, off);
    require(!later.valid && later.reason == TemporalResetReason::None,
            "Continuing in Off must remain a deliberate non-temporal mode");
}

void deterministic_jitter() {
    const auto first = temporal_jitter(0, 320, 240);
    const auto second = temporal_jitter(1, 320, 240);
    require(std::abs(first[0]) < 1e-7f && std::abs(first[1] + 1.f / 720.f) < 1e-7f,
            "First Halton(2,3) sample must be a clip-space offset");
    require(std::abs(second[0] + 1.f / 640.f) < 1e-7f &&
                std::abs(second[1] - 1.f / 720.f) < 1e-7f,
            "Second Halton sample must visit a different subpixel position");
    require(first == temporal_jitter(16, 320, 240),
            "The finite sequence must repeat on frame sixteen");
    require(std::abs(temporal_jitter(0, 640, 480)[1] * 2.f - first[1]) < 1e-7f,
            "Clip jitter must scale inversely with viewport extent");
    for (std::uint64_t frame = 0; frame < 16; ++frame) {
        const auto sample = temporal_jitter(frame, 319, 241);
        require(std::abs(sample[0]) <= 1.f / 319.f &&
                    std::abs(sample[1]) <= 1.f / 241.f,
                "Every jitter sample must stay within half an output pixel");
    }
    bool rejected_zero_extent = false;
    try {
        (void)temporal_jitter(0, 0, 240);
    } catch (const std::invalid_argument&) {
        rejected_zero_extent = true;
    }
    require(rejected_zero_extent, "Zero viewport extent cannot produce finite clip jitter");
}

void render_scale_policy() {
    const auto full = temporal_internal_extent(320, 240, TemporalMode::Off, 1.f);
    require(full == std::array<std::uint32_t, 2>{320, 240},
            "Off keeps scene and output at the same extent");
    require(temporal_internal_extent(319, 241, TemporalMode::TAA, 1.f) ==
                std::array<std::uint32_t, 2>{319, 241},
            "TAA is a one-to-one reconstruction mode");
    require(temporal_internal_extent(320, 240, TemporalMode::Upscale, .67f) ==
                std::array<std::uint32_t, 2>{215, 161},
            "Upscale uses deterministic ceil dimensions for odd pixel products");
    require(temporal_internal_extent(1, 1, TemporalMode::Upscale, .5f) ==
                std::array<std::uint32_t, 2>{1, 1},
            "A supported output always has at least one internal pixel");
    auto rejected = [](TemporalMode mode, float scale) {
        try {
            (void)temporal_internal_extent(320, 240, mode, scale);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    require(rejected(TemporalMode::TAA, .75f) && rejected(TemporalMode::Upscale, 1.f) &&
                rejected(TemporalMode::Upscale, .49f) &&
                rejected(TemporalMode::Upscale, std::numeric_limits<float>::quiet_NaN()) &&
                rejected(TemporalMode::Off, .67f) &&
                rejected(static_cast<TemporalMode>(42), 1.f),
            "Invalid mode/scale combinations must fail before target allocation");
    bool zero_rejected = false;
    try {
        (void)temporal_internal_extent(0, 240, TemporalMode::TAA, 1.f);
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    require(zero_rejected, "Zero output width is not a valid temporal target");
}
} // namespace

int main() {
    capability_fallback();
    intentionally_disabled_temporal_mode();
    rendered_history_and_camera_motion();
    incompatible_view_state();
    deterministic_jitter();
    render_scale_policy();
}
