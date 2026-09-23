#include <algorithm>
#include <cmath>
#include <cstdint>
#include <faset/render/renderer.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace faset::render;

namespace {
void require(bool condition, const std::string& message) {
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
    result.visibility_diagnostics = true;
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
        if (gpu.stats().gpu_ms > 0)
            require(gpu.stats().gpu_main_cull_ms > 0 &&
                        gpu.stats().gpu_main_raster_ms > 0,
                    "GPU visibility pass timings are missing");
        require(different_pixels(direct.pixels(), gpu.pixels(), 5) < 160,
                "GPU and direct opaque images differ");

        frame.draws.clear();
        gpu.render(frame);
        require(gpu.stats().gpu_visible_instances == 0 && gpu.stats().gpu_bins == 0,
                "Empty frame must reset indirect counts");
        require(gpu.stats().validation_errors == 0, "Empty GPU frame failed validation");
        gpu.set_visibility_diagnostics(false);
        gpu.render(scene());
        require(!gpu.stats().visibility_counters_valid &&
                    gpu.stats().gpu_visibility_active,
                "Normal GPU rendering must not read diagnostic visibility counters");
        gpu.set_visibility_diagnostics(true);
        gpu.render(scene());
        require(gpu.stats().visibility_counters_valid &&
                    gpu.stats().gpu_visible_instances == 1,
                "Visibility counters can be enabled for diagnostics");
        Renderer occlusion(config(VisibilityMode::GpuOcclusion));
        occlusion.render(scene());
        auto hzb = occlusion.hzb_debug_image(0);
        if (occlusion.stats().gpu_ms > 0)
            require(occlusion.stats().gpu_hzb_ms > 0 &&
                        occlusion.stats().gpu_post_cull_ms > 0 &&
                        occlusion.stats().gpu_post_raster_ms > 0,
                    "HZB/post pass timings are missing");
        require(hzb && hzb->width == 512 && hzb->height == 256 &&
                    hzb->rgba.size() == std::size_t(hzb->width) * hzb->height * 4,
                "Current HZB debug image has wrong dimensions");
        require(std::any_of(hzb->rgba.begin(), hzb->rgba.end(),
                            [](std::uint8_t value) { return value < 250; }),
                "Current HZB debug image contains no scene depth (min=" +
                    std::to_string(*std::min_element(hzb->rgba.begin(), hzb->rgba.end())) +
                    ")");
        std::cout << "GPU frustum, fixed indirect and direct-image comparison passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
