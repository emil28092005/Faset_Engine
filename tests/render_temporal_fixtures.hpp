#pragma once
#include <faset/render/renderer.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace faset::render::temporal_test {
inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Region {
    std::uint32_t x{}, y{}, width{}, height{};
};

inline double mean_rgb_error(const std::vector<std::uint8_t>& actual,
                             const std::vector<std::uint8_t>& reference,
                             std::uint32_t image_width, std::uint32_t image_height,
                             Region region) {
    require(actual.size() == reference.size() &&
                actual.size() == std::size_t(image_width) * image_height * 4 &&
                region.width && region.height && region.x <= image_width &&
                region.y <= image_height && region.width <= image_width - region.x &&
                region.height <= image_height - region.y,
            "Temporal image metric requires equal images and an in-bounds ROI");
    std::uint64_t difference{};
    for (auto y = region.y; y < region.y + region.height; ++y)
        for (auto x = region.x; x < region.x + region.width; ++x) {
            const auto base = (std::size_t(y) * image_width + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel)
                difference += static_cast<std::uint64_t>(
                    std::abs(int(actual[base + channel]) - int(reference[base + channel])));
        }
    return double(difference) / (double(region.width) * region.height * 3);
}

inline std::array<std::uint8_t, 4> pixel(const std::vector<std::uint8_t>& rgba,
                                         std::uint32_t width, std::uint32_t x,
                                         std::uint32_t y) {
    const auto base = (std::size_t(y) * width + x) * 4;
    require(base + 3 < rgba.size(), "Requested temporal test pixel is outside the image");
    return {rgba[base], rgba[base + 1], rgba[base + 2], rgba[base + 3]};
}

inline DrawItem cube(Vec3 position, Color color, std::string key) {
    DrawItem draw;
    draw.mesh = cube_mesh();
    draw.model = transform(position);
    draw.color = color;
    draw.instance_key = std::move(key);
    draw.cast_shadow = false;
    return draw;
}

inline Snapshot lit_scene(std::uint32_t width, std::uint32_t height) {
    Snapshot frame;
    frame.view_id = "temporal-acceptance-main";
    frame.eye = {0, 0, 6};
    frame.projection = perspective(.9f, float(width) / float(height), .1f, 50.f);
    frame.view_projection = multiply(frame.projection, look_at(frame.eye, {0, 0, 0}));
    frame.authored_lights_present = true;
    frame.sun = SunLight{"test-sun", {-.5f, -1.f, -.3f}, {1, 1, 1, 1}, 3.f, false};
    return frame;
}

inline RendererConfig headless_config(std::uint32_t width, std::uint32_t height,
                                      VisibilityMode visibility) {
    RendererConfig config;
    config.width = width;
    config.height = height;
    config.headless = true;
    config.validation = true;
    config.visibility_mode = visibility;
    return config;
}
} // namespace faset::render::temporal_test
