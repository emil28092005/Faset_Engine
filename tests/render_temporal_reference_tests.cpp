#include <faset/render/temporal_reference.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace faset::render;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

TemporalReferenceFrame solid(std::uint32_t width, std::uint32_t height,
                             float intensity = .5f, float depth = .5f) {
    TemporalReferenceFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(std::size_t(width) * height);
    for (auto& pixel : frame.pixels) {
        pixel.color = {intensity, intensity, intensity, 1.f};
        pixel.depth = depth;
        pixel.previous_depth = depth;
        pixel.motion_valid = true;
    }
    return frame;
}

TemporalReferenceHistory history_from(const TemporalReferenceFrame& frame) {
    TemporalReferenceHistory history;
    history.width = frame.width;
    history.height = frame.height;
    for (const auto& pixel : frame.pixels) {
        history.color.push_back(pixel.color);
        history.depth.push_back(pixel.depth);
    }
    return history;
}

void first_frame_and_malformed_inputs() {
    auto frame = solid(2, 2, .4f);
    const auto first = temporal_reference_resolve(frame, nullptr);
    require(first.accepted_count == 0 && first.accepted.size() == 4,
            "A first frame cannot accept previous color");
    for (std::size_t i = 0; i < frame.pixels.size(); ++i)
        require(first.history.color[i] == frame.pixels[i].color &&
                    first.history.depth[i] == frame.pixels[i].depth && !first.accepted[i],
                "First-frame fallback writes current color and depth exactly");
    auto malformed = frame;
    malformed.pixels.pop_back();
    bool rejected = false;
    try {
        (void)temporal_reference_resolve(malformed, nullptr);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Image extent and pixel count must agree");
    auto wrong_extent = first.history;
    wrong_extent.width = 3;
    rejected = false;
    try {
        (void)temporal_reference_resolve(frame, &wrong_extent);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Incompatible history extent must be reset by the caller");
}

void reprojection_sign_and_depth_rejection() {
    auto frame = solid(4, 1);
    frame.pixels[0].color = {0, 0, 0, 1};
    frame.pixels[1].color = {0, 0, 0, 1};
    frame.pixels[3].color = {1, 1, 1, 1};
    frame.pixels[2].motion = {.25f, 0};
    auto prior = history_from(frame);
    prior.color[1] = {.2f, .2f, .2f, 1};
    prior.color[3] = {.8f, .8f, .8f, 1};
    const auto moved = temporal_reference_resolve(frame, &prior);
    require(moved.accepted[2] && moved.history.color[2][0] < .5f,
            "Positive current-minus-prior motion must sample the pixel to the left");

    prior.depth[1] = .2f;
    const auto revealed = temporal_reference_resolve(frame, &prior);
    require(!revealed.accepted[2] && revealed.history.color[2] == frame.pixels[2].color,
            "A previous-depth disagreement rejects newly exposed background color");
    frame.pixels[2].motion = {2, 0};
    const auto outside = temporal_reference_resolve(frame, &prior);
    require(!outside.accepted[2] && outside.history.color[2] == frame.pixels[2].color,
            "Reprojection outside the old scene rejects history");
}

void neighborhood_clamp_and_reactive_pixels() {
    auto frame = solid(3, 3);
    for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
        const float tone = i % 2 ? .4f : .6f;
        frame.pixels[i].color = {tone, tone, tone, 1};
    }
    frame.pixels[4].color = {.5f, .5f, .5f, 1};
    auto prior = history_from(frame);
    prior.color[4] = {10, 10, 10, 1};
    const auto clipped = temporal_reference_resolve(frame, &prior);
    require(clipped.accepted[4] && clipped.history.color[4][0] > .5f &&
                clipped.history.color[4][0] <= .6f,
            "Neighborhood clamp bounds a bright stale history sample");

    frame.pixels[4].reactive = .5f;
    const auto softened = temporal_reference_resolve(frame, &prior);
    require(softened.accepted[4] && softened.history.color[4][0] > .5f &&
                softened.history.color[4][0] < clipped.history.color[4][0],
            "Partial reactivity reduces the accepted history weight");

    frame.pixels[4].reactive = 1.f;
    const auto reactive = temporal_reference_resolve(frame, &prior);
    require(!reactive.accepted[4] && reactive.history.color[4] == frame.pixels[4].color,
            "Reactive transparency/sprite pixels must use current color");
    frame.pixels[4].reactive = 0;
    frame.pixels[4].motion_valid = false;
    const auto anonymous = temporal_reference_resolve(frame, &prior);
    require(!anonymous.accepted[4] && anonymous.history.color[4] == frame.pixels[4].color,
            "An anonymous or replaced object cannot borrow neighboring history");
    frame.pixels[4].motion_valid = true;
    frame.pixels[4].motion[0] = INFINITY;
    const auto invalid_motion = temporal_reference_resolve(frame, &prior);
    require(!invalid_motion.accepted[4] &&
                invalid_motion.history.color[4] == frame.pixels[4].color,
            "Nonfinite motion rejects history without contaminating output");
}

void nearest_depth_motion_dilation() {
    auto frame = solid(3, 3, .5f, .9f);
    frame.pixels[4].depth = .8f;
    frame.pixels[4].previous_depth = .8f;
    frame.pixels[3].depth = .2f;
    frame.pixels[3].previous_depth = .2f;
    frame.pixels[3].motion = {1.f / 3.f, 0};
    frame.pixels[1].color = {0, 0, 0, 1};
    frame.pixels[5].color = {1, 1, 1, 1};
    auto prior = history_from(frame);
    prior.depth[4] = .1f; // Undilated center motion would fail this depth check.
    prior.depth[3] = .2f;
    prior.color[3] = {.1f, .1f, .1f, 1};
    const auto dilated = temporal_reference_resolve(frame, &prior);
    require(dilated.accepted[4] && dilated.history.color[4][0] < .5f,
            "Nearest-depth foreground motion should fill a valid silhouette pixel");
    frame.pixels[4].motion_valid = false;
    const auto invalid_center = temporal_reference_resolve(frame, &prior);
    require(!invalid_center.accepted[4],
            "Dilation must not resurrect missing per-object previous transforms");
}
} // namespace

int main() {
    first_frame_and_malformed_inputs();
    reprojection_sign_and_depth_rejection();
    neighborhood_clamp_and_reactive_pixels();
    nearest_depth_motion_dilation();
}
