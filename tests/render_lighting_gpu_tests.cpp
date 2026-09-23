#include <faset/render/renderer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
struct Frame {
    std::vector<std::uint8_t> pixels;
    FrameStats stats;
};
Frame capture(Renderer& renderer, const Snapshot& scene) {
    renderer.render(scene);
    return {renderer.pixels(), renderer.stats()};
}
Renderer make_renderer(VisibilityMode mode) {
    RendererConfig config;
    config.width = 320;
    config.height = 240;
    config.headless = true;
    config.validation = true;
    config.visibility_mode = mode;
    config.visibility_diagnostics = true;
    return Renderer(config);
}
void compare_frames(const Frame& direct, const Frame& gpu) {
    require(direct.pixels.size() == gpu.pixels.size(), "Lighting image dimensions match");
    std::uint64_t error{};
    std::size_t bad{};
    for (std::size_t i = 0; i < direct.pixels.size(); i += 4) {
        int worst{};
        for (int channel = 0; channel < 3; ++channel) {
            const int difference = std::abs(int(direct.pixels[i + channel]) -
                                            int(gpu.pixels[i + channel]));
            error += difference;
            worst = std::max(worst, difference);
        }
        bad += worst > 16;
    }
    const auto count = direct.pixels.size() / 4;
    require(bad <= std::max<std::size_t>(24, count / 200) &&
                double(error) / double(count * 3) <= 2.0,
            "Direct and GPU sun lighting images agree (bad=" + std::to_string(bad) +
                ", mean=" + std::to_string(double(error) / double(count * 3)) + ")");
}
Snapshot scene(bool caster) {
    Snapshot result;
    result.view_id = "p3-offscreen-sun";
    result.eye = {0, 5, 8};
    const auto view = look_at(result.eye, {0, -1, 0});
    const auto projection = orthographic(-2.5f, 2.5f, -2, 2, .1f, 50);
    result.projection = projection;
    result.view_projection = multiply(projection, view);
    result.camera_frustum = CameraFrustum{view, projection, .1f, 50.f, false};
    DrawItem receiver;
    receiver.mesh = cube_mesh();
    receiver.model = transform({0, -1, 0}, {}, {8, .1f, 8});
    receiver.color = {.8f, .8f, .8f, 1};
    receiver.instance_key = "receiver";
    result.draws.push_back(receiver);
    if (caster) {
        DrawItem shadow_caster;
        shadow_caster.mesh = cube_mesh();
        shadow_caster.model = transform({3, 1, 0}, {}, {.8f, .8f, .8f});
        shadow_caster.color = {.2f, .2f, .8f, 1};
        shadow_caster.instance_key = "offscreen-caster";
        result.draws.push_back(shadow_caster);
    }
    return result;
}
void sun() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto occlusion = make_renderer(VisibilityMode::GpuOcclusion);
    auto with_caster = scene(true);
    const auto direct_frame = capture(direct, with_caster);
    const auto gpu_frame = capture(gpu, with_caster);
    const auto occlusion_frame = capture(occlusion, with_caster);
    require(direct_frame.stats.effective_sun_cascades == 4 &&
                gpu_frame.stats.effective_sun_cascades == 4 &&
                occlusion_frame.stats.effective_sun_cascades == 4,
            "Explicit 3D camera renders four sun cascades on every graphics path");
    require(direct_frame.stats.requested_sun_cascades == 4 &&
                direct_frame.stats.sun_shadow_caster_draws > 0 &&
                direct_frame.stats.sun_shadow_caster_draws <= 4096 &&
                direct_frame.stats.sun_shadow_atlas_bytes > 0 &&
                direct_frame.stats.gpu_sun_shadow_ms > 0,
            "Sun cascade stats describe bounded actual raster work and GPU time");
    require(gpu_frame.stats.gpu_frustum_rejected > 0,
            "Offscreen caster fixture is outside GPU camera frustum");
    require(direct_frame.stats.validation_errors == 0 &&
                gpu_frame.stats.validation_errors == 0 &&
                occlusion_frame.stats.validation_errors == 0,
            "Sun atlas rendering reports no Vulkan validation errors");
    compare_frames(direct_frame, gpu_frame);
    compare_frames(direct_frame, occlusion_frame);
    auto without = scene(false);
    const auto no_caster = capture(direct, without);
    std::size_t darkened{};
    for (std::size_t i = 0; i < direct_frame.pixels.size(); i += 4)
        darkened += int(no_caster.pixels[i]) > int(direct_frame.pixels[i]) + 12;
    require(darkened > 20,
            "Offscreen source-LOD0 caster darkens visible receiver (count=" +
                std::to_string(darkened) + ")");
    auto coarser = with_caster;
    auto degenerate_lod = std::make_shared<Mesh>(*cube_mesh());
    for (auto& vertex : degenerate_lod->vertices)
        vertex.position = {0, 0, 0};
    coarser.draws.back().lod_meshes.push_back(degenerate_lod);
    const auto source_lod_shadow = capture(gpu, coarser);
    std::size_t lod_darkened{};
    for (std::size_t i = 0; i < source_lod_shadow.pixels.size(); i += 4)
        lod_darkened += int(no_caster.pixels[i]) >
                        int(source_lod_shadow.pixels[i]) + 12;
    require(source_lod_shadow.stats.lod_counts[1] > 0 && lod_darkened > 20,
            "Shadow raster uses source LOD0 even when camera chooses a coarse LOD");
    auto no_shadow = with_caster;
    no_shadow.authored_lights_present = true;
    no_shadow.sun = SunLight{"sun", no_shadow.light_direction, {1, 1, 1, 1}, 1, false};
    const auto disabled = capture(direct, no_shadow);
    require(disabled.stats.effective_sun_cascades == 0,
            "Disabled sun shadow does no shadow raster work");
    require(disabled.stats.sun_shadow_caster_draws == 0 &&
                disabled.stats.gpu_sun_shadow_ms == 0,
            "Disabled sun does not draw a hidden legacy shadow pass");
    auto legacy = with_caster;
    legacy.camera_frustum.reset();
    const auto fallback = capture(direct, legacy);
    require(fallback.stats.effective_sun_cascades == 1,
            "Low-level snapshot without explicit camera retains one reported shadow view");
    Snapshot sprite_only;
    sprite_only.sprites.push_back({{0, 0, 0}, {1, 1}});
    const auto two_d = capture(direct, sprite_only);
    require(two_d.stats.effective_sun_cascades == 0,
            "Sprite-only scene skips the sun atlas raster");
    require(two_d.stats.sun_shadow_caster_draws == 0 &&
                two_d.stats.gpu_sun_shadow_ms == 0,
            "Sprite-only rendering spends no sun shadow GPU work");
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2 || std::string(argv[1]) != "--sun")
            throw std::invalid_argument("Expected --sun");
        sun();
        std::cout << "Sun cascade atlas and Direct/GPU lighting parity passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
