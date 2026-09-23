#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace faset::render {

// CPU-only 1:1 oracle for the temporal resolve shader. Color is already
// display-referred; motion is current-minus-previous normalized scene UV.
// The renderer does not call this per-pixel path in production.
struct TemporalReferencePixel {
    std::array<float, 4> color{0, 0, 0, 1};
    float depth{1};
    std::array<float, 2> motion{};
    float previous_depth{1};
    bool motion_valid{};
    float reactive{};
};

struct TemporalReferenceFrame {
    std::uint32_t width{}, height{};
    std::vector<TemporalReferencePixel> pixels;
};

struct TemporalReferenceHistory {
    std::uint32_t width{}, height{};
    std::vector<std::array<float, 4>> color;
    std::vector<float> depth;
};

struct TemporalReferenceResult {
    TemporalReferenceHistory history;
    std::vector<std::uint8_t> accepted;
    std::uint32_t accepted_count{};
};

// A null previous history is the first-frame/cut fallback. Extents must agree
// when history is supplied; reset it instead of reprojecting stale dimensions.
// Input is bounded to four million pixels so synthetic tests cannot consume
// unbounded memory. Invalid motion rejects history for that pixel.
TemporalReferenceResult temporal_reference_resolve(
    const TemporalReferenceFrame& current,
    const TemporalReferenceHistory* previous = nullptr);

} // namespace faset::render
