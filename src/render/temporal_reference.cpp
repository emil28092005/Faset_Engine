#include <faset/render/temporal_reference.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace faset::render {
namespace {
constexpr std::uint64_t max_reference_pixels = 4'000'000;

std::size_t checked_pixel_count(std::uint32_t width, std::uint32_t height) {
    const auto count = std::uint64_t(width) * height;
    if (!width || !height || count > max_reference_pixels)
        throw std::invalid_argument("Temporal reference image extent is unsupported");
    return static_cast<std::size_t>(count);
}

bool valid_motion(const TemporalReferencePixel& pixel) noexcept {
    return pixel.motion_valid && pixel.reactive < 1.f &&
           std::isfinite(pixel.motion[0]) && std::isfinite(pixel.motion[1]) &&
           std::isfinite(pixel.previous_depth) && pixel.previous_depth >= 0.f &&
           pixel.previous_depth <= 1.f;
}

float bilinear_channel(const TemporalReferenceHistory& history, float u, float v,
                       std::size_t channel) noexcept {
    const float x = u * history.width - .5f;
    const float y = v * history.height - .5f;
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto sample = [&](int sx, int sy) {
        const auto px = std::clamp(sx, 0, static_cast<int>(history.width) - 1);
        const auto py = std::clamp(sy, 0, static_cast<int>(history.height) - 1);
        return history.color[std::size_t(py) * history.width + px][channel];
    };
    const float top = std::lerp(sample(x0, y0), sample(x0 + 1, y0), tx);
    const float bottom = std::lerp(sample(x0, y0 + 1), sample(x0 + 1, y0 + 1), tx);
    return std::lerp(top, bottom, ty);
}
} // namespace

TemporalReferenceResult temporal_reference_resolve(const TemporalReferenceFrame& current,
                                                   const TemporalReferenceHistory* previous) {
    const auto count = checked_pixel_count(current.width, current.height);
    if (current.pixels.size() != count)
        throw std::invalid_argument("Temporal reference current pixel count differs from extent");
    if (previous &&
        (previous->width != current.width || previous->height != current.height ||
         previous->color.size() != count || previous->depth.size() != count))
        throw std::invalid_argument("Temporal reference history extent or record count differs");
    for (const auto& pixel : current.pixels) {
        if (!std::isfinite(pixel.depth) || pixel.depth < 0.f || pixel.depth > 1.f ||
            !std::isfinite(pixel.reactive) || pixel.reactive < 0.f || pixel.reactive > 1.f)
            throw std::invalid_argument("Temporal reference current depth/reactivity is invalid");
        for (float channel : pixel.color)
            if (!std::isfinite(channel))
                throw std::invalid_argument("Temporal reference current color is nonfinite");
    }
    if (previous)
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(previous->depth[i]) || previous->depth[i] < 0.f ||
                previous->depth[i] > 1.f)
                throw std::invalid_argument("Temporal reference history depth is invalid");
            for (float channel : previous->color[i])
                if (!std::isfinite(channel))
                    throw std::invalid_argument("Temporal reference history color is nonfinite");
        }

    TemporalReferenceResult result;
    result.history.width = current.width;
    result.history.height = current.height;
    result.history.color.resize(count);
    result.history.depth.resize(count);
    result.accepted.resize(count);

    for (std::uint32_t y = 0; y < current.height; ++y)
        for (std::uint32_t x = 0; x < current.width; ++x) {
            const auto i = std::size_t(y) * current.width + x;
            const auto& center = current.pixels[i];
            auto& output = result.history.color[i];
            output = center.color;
            result.history.depth[i] = center.depth;
            if (!previous || !valid_motion(center))
                continue;

            // Dilate the nearest opaque depth's motion at a silhouette, but
            // never resurrect a center pixel with no previous transform.
            const TemporalReferencePixel* selected = &center;
            const auto min_y = y ? y - 1 : y;
            const auto min_x = x ? x - 1 : x;
            const auto max_y = std::min(y + 1, current.height - 1);
            const auto max_x = std::min(x + 1, current.width - 1);
            for (auto sy = min_y; sy <= max_y; ++sy)
                for (auto sx = min_x; sx <= max_x; ++sx) {
                    const auto& candidate = current.pixels[std::size_t(sy) * current.width + sx];
                    if (valid_motion(candidate) && candidate.depth < selected->depth)
                        selected = &candidate;
                }
            const float u = (static_cast<float>(x) + .5f) / current.width - selected->motion[0];
            const float v = (static_cast<float>(y) + .5f) / current.height - selected->motion[1];
            if (!std::isfinite(u) || !std::isfinite(v) || u < 0.f || v < 0.f ||
                u >= 1.f || v >= 1.f)
                continue;
            const auto px = std::min(static_cast<std::uint32_t>(u * previous->width),
                                     previous->width - 1);
            const auto py = std::min(static_cast<std::uint32_t>(v * previous->height),
                                     previous->height - 1);
            const float sampled_depth = previous->depth[std::size_t(py) * previous->width + px];
            const float depth_tolerance = .002f + .01f * selected->previous_depth;
            if (std::abs(sampled_depth - selected->previous_depth) > depth_tolerance)
                continue;

            const float motion_pixels = std::hypot(selected->motion[0] * current.width,
                                                   selected->motion[1] * current.height);
            const float weight = .9f * (1.f - center.reactive) / (1.f + .5f * motion_pixels);
            if (!std::isfinite(weight) || weight <= 0.f)
                continue;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                float neighborhood_min = std::numeric_limits<float>::infinity();
                float neighborhood_max = -neighborhood_min;
                for (auto sy = min_y; sy <= max_y; ++sy)
                    for (auto sx = min_x; sx <= max_x; ++sx) {
                        const auto value = current.pixels[std::size_t(sy) * current.width + sx]
                                               .color[channel];
                        neighborhood_min = std::min(neighborhood_min, value);
                        neighborhood_max = std::max(neighborhood_max, value);
                    }
                const float prior = std::clamp(bilinear_channel(*previous, u, v, channel),
                                               neighborhood_min, neighborhood_max);
                output[channel] = std::lerp(center.color[channel], prior, weight);
            }
            result.accepted[i] = 1;
            ++result.accepted_count;
        }
    return result;
}

} // namespace faset::render
