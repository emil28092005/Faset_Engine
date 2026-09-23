#include <algorithm>
#include <cmath>
#include <cstdint>
#include <faset/render/renderer.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace faset::render;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

RendererConfig config(VisibilityMode mode) {
    RendererConfig result;
    result.width = 320;
    result.height = 240;
    result.headless = true;
    result.validation = true;
    result.visibility_mode = mode;
    return result;
}

Snapshot scene() {
    Snapshot frame;
    frame.view_id = "gpu-visibility-test";
    frame.eye = {4, 3, 6};
    frame.view_projection =
        multiply(perspective(.85f, 320.f / 240.f, .1f, 100.f), look_at(frame.eye, {0, 0, 0}));
    DrawItem visible;
    visible.mesh = cube_mesh();
    visible.model = transform({0, 0, 0});
    visible.color = {.8f, .6f, .3f, 1};
    visible.instance_key = "visible-cube";
    frame.draws.push_back(visible);
    DrawItem hidden = visible;
    hidden.model = transform({100, 0, 0});
    hidden.instance_key = "outside-cube";
    frame.draws.push_back(hidden);
    return frame;
}

std::size_t different_pixels(const std::vector<std::uint8_t>& a,
                             const std::vector<std::uint8_t>& b, int tolerance) {
    require(a.size() == b.size(), "Image sizes differ");
    std::size_t result{};
    for (std::size_t i = 0; i < a.size(); i += 4)
        if (std::abs(int(a[i]) - int(b[i])) > tolerance ||
            std::abs(int(a[i + 1]) - int(b[i + 1])) > tolerance ||
            std::abs(int(a[i + 2]) - int(b[i + 2])) > tolerance)
            ++result;
    return result;
}
} // namespace

int main() {
    try {
        Renderer direct(config(VisibilityMode::Direct));
        Renderer gpu(config(VisibilityMode::GpuFrustum));
        auto frame = scene();
        direct.render(frame);
        gpu.render(frame);
        require(gpu.stats().gpu_visibility_active, "GPU visibility path did not run");
        require(gpu.stats().gpu_bins == 1 && gpu.stats().gpu_visible_instances == 1 &&
                    gpu.stats().gpu_frustum_rejected == 1,
                "GPU frustum/indirect counts are wrong");
        require(gpu.stats().validation_errors == 0, "Vulkan validation rejected GPU path");
        require(different_pixels(direct.pixels(), gpu.pixels(), 5) < 160,
                "GPU and direct opaque images differ");

        frame.draws.clear();
        gpu.render(frame);
        require(gpu.stats().gpu_visible_instances == 0 && gpu.stats().gpu_bins == 0,
                "Empty frame must reset indirect counts");
        require(gpu.stats().validation_errors == 0, "Empty GPU frame failed validation");
        std::cout << "GPU frustum, fixed indirect and direct-image comparison passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
