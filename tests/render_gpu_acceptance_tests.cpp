#include <faset/render/renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace faset::render;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

Renderer make_renderer(VisibilityMode mode, std::uint32_t width = 320,
                       std::uint32_t height = 240) {
    RendererConfig config;
    config.width = width;
    config.height = height;
    config.headless = true;
    config.validation = true;
    config.visibility_mode = mode;
    return Renderer(config);
}

struct Frame {
    std::vector<std::uint8_t> rgba;
    FrameStats stats;
};

Frame frame(Renderer& renderer, const Snapshot& scene) {
    renderer.render(scene);
    return {renderer.pixels(), renderer.stats()};
}

void compare(const Frame& reference, const Frame& gpu, std::string_view name,
             bool expect_gpu = true) {
    require(reference.rgba.size() == gpu.rgba.size() && reference.rgba.size() % 4 == 0,
            std::string(name) + ": render target dimensions differ");
    require(reference.stats.validation_errors == 0 && gpu.stats.validation_errors == 0,
            std::string(name) + ": Vulkan validation reported an error");
    if (expect_gpu)
        require(gpu.stats.gpu_visibility_active,
                std::string(name) + ": GPU visibility silently fell back to direct rendering");
    std::size_t bad_pixels = 0;
    std::uint64_t absolute_error = 0;
    for (std::size_t i = 0; i < reference.rgba.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const auto difference = std::abs(int(reference.rgba[i + c]) - int(gpu.rgba[i + c]));
            absolute_error += static_cast<std::uint64_t>(difference);
            worst = std::max(worst, difference);
        }
        if (worst > 16)
            ++bad_pixels;
    }
    const auto pixels = reference.rgba.size() / 4;
    const auto allowed_bad = std::max<std::size_t>(24, pixels / 200);
    const auto average_error = double(absolute_error) / double(pixels * 3);
    require(bad_pixels <= allowed_bad && average_error <= 2.0,
            std::string(name) + ": direct/GPU image mismatch (bad pixels " +
                std::to_string(bad_pixels) + "/" + std::to_string(pixels) +
                ", RGB mean error " + std::to_string(average_error) + ")");
}

void compare_grid_centers(const Frame& reference, const Frame& gpu, std::size_t count,
                          int columns, float stride) {
    // Every isolated cube must contribute at least its front-face center. This catches a
    // dropped instance even when its footprint is smaller than the whole-frame tolerance.
    for (std::size_t i = 0; i < count; ++i) {
        const float x = (float(i % columns) - float(columns - 1) * .5f) * stride;
        const float y = (float(i / columns) - 3.5f) * stride;
        const int px = std::clamp(int((x / 18.f + .5f) * 320.f), 0, 319);
        const int py = std::clamp(int((.5f - y / 16.f) * 240.f), 0, 239);
        const auto offset = (std::size_t(py) * 320 + std::size_t(px)) * 4;
        const auto reference_energy = int(reference.rgba[offset]) +
                                      int(reference.rgba[offset + 1]) +
                                      int(reference.rgba[offset + 2]);
        require(reference_energy > 90,
                "capacity fixture does not cover projected center of instance " +
                    std::to_string(i));
        for (int channel = 0; channel < 3; ++channel)
            require(std::abs(int(reference.rgba[offset + channel]) -
                             int(gpu.rgba[offset + channel])) <= 16,
                    "instance " + std::to_string(i) + " differs at its projected center");
    }
}

DrawItem cube(std::string key, Vec3 position, Vec3 scale = {1, 1, 1},
              Color color = {0.75f, 0.6f, 0.35f, 1}) {
    DrawItem item;
    item.mesh = cube_mesh();
    item.model = transform(position, {}, scale);
    item.color = color;
    item.instance_key = std::move(key);
    return item;
}

Snapshot grid(std::size_t count, int columns = 9, float stride = 1.7f) {
    Snapshot result;
    result.view_id = "acceptance-grid";
    result.eye = {0, 0, 20};
    result.view_projection = multiply(orthographic(-9, 9, -8, 8, .1f, 100),
                                      look_at(result.eye, {0, 0, 0}));
    for (std::size_t i = 0; i < count; ++i) {
        float x = (float(i % columns) - float(columns - 1) * .5f) * stride;
        float y = (float(i / columns) - 3.5f) * stride;
        const Color tint{.35f + .5f * float(i % 3) / 2.f,
                         .3f + .5f * float(i % 5) / 4.f,
                         .3f + .5f * float(i % 7) / 6.f, 1};
        result.draws.push_back(
            cube("grid-" + std::to_string(i), {x, y, 0}, {.55f, .55f, .55f}, tint));
    }
    return result;
}

void empty_and_retirement() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto populated = grid(64);
    frame(gpu, populated);
    auto empty = grid(0);
    const auto expected = frame(direct, empty);
    const auto actual = frame(gpu, empty);
    compare(expected, actual, "empty scene after populated frame");
    require(actual.stats.gpu_visible_instances == 0,
            "empty scene reused an indirect instance count from the previous frame");
}

void exact_capacity() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    for (const std::size_t count : {64u, 65u, 64u}) {
        auto scene = grid(count);
        const auto expected = frame(direct, scene);
        const auto actual = frame(gpu, scene);
        compare(expected, actual, "capacity " + std::to_string(count));
        compare_grid_centers(expected, actual, count, 9, 1.7f);
        require(actual.stats.gpu_visible_instances == count,
                "fixed bin dropped an in-frustum instance at a capacity boundary");
    }
}

void dense_frustum() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto scene = grid(256, 16, 2.0f);
    const auto expected = frame(direct, scene);
    const auto actual = frame(gpu, scene);
    compare(expected, actual, "dense frustum scene");
    require(actual.stats.gpu_frustum_rejected > 0,
            "GPU frustum pass failed to reject offscreen objects");
    require(actual.stats.gpu_bins > 0 && actual.stats.gpu_bins < scene.draws.size(),
            "instances were not submitted through fixed indirect bins");
}

Snapshot doorway(bool open) {
    Snapshot result;
    result.view_id = "acceptance-door";
    result.eye = {0, 0, 8};
    result.view_projection = multiply(perspective(.85f, 320.f / 240.f, .1f, 100),
                                      look_at(result.eye, {0, 0, 0}));
    auto wall = cube("door-wall", {open ? 20.f : 0.f, 0, 0}, {7, 7, .4f},
                     {.65f, .65f, .65f, 1});
    wall.cast_shadow = false;
    result.draws.push_back(std::move(wall));
    auto behind = cube("behind-door", {0, 0, -2}, {1.8f, 1.8f, 1.8f},
                       {.9f, .15f, .12f, 1});
    behind.cast_shadow = false;
    result.draws.push_back(std::move(behind));
    return result;
}

void door_reveal() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuOcclusion);
    auto closed = doorway(false);
    auto open = doorway(true);
    frame(gpu, closed);
    frame(gpu, closed); // Give the previous-view pyramid an ordinary settled frame.
    const auto expected = frame(direct, open);
    const auto actual = frame(gpu, open);
    compare(expected, actual, "door reveal");
    require(actual.stats.gpu_occlusion_deferred > 0 && actual.stats.gpu_post_visible > 0,
            "newly revealed instance did not pass through post-occlusion recovery (deferred=" +
                std::to_string(actual.stats.gpu_occlusion_deferred) + ", post=" +
                std::to_string(actual.stats.gpu_post_visible) + ", hzb_valid=" +
                std::to_string(actual.stats.hzb_valid) + ")");
}

Snapshot shadow_scene(bool caster) {
    Snapshot result;
    result.view_id = "acceptance-shadow";
    result.eye = {0, 5, 8};
    result.view_projection = multiply(orthographic(-2.5f, 2.5f, -2, 2, .1f, 50),
                                      look_at(result.eye, {0, -1, 0}));
    result.draws.push_back(cube("receiver", {0, -1, 0}, {8, .1f, 8},
                                {.8f, .8f, .8f, 1}));
    if (caster)
        result.draws.push_back(cube("offscreen-caster", {3, 1, 0}, {.8f, .8f, .8f},
                                    {.2f, .2f, .8f, 1}));
    return result;
}

void offscreen_shadow() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto with_caster = shadow_scene(true);
    const auto expected = frame(direct, with_caster);
    const auto actual = frame(gpu, with_caster);
    compare(expected, actual, "offscreen shadow caster");
    require(actual.stats.gpu_frustum_rejected > 0,
            "shadow fixture caster must be outside the camera frustum");
    auto without = frame(direct, shadow_scene(false));
    std::size_t shadow_pixels = 0;
    for (std::size_t i = 0; i < expected.rgba.size(); i += 4)
        if (int(without.rgba[i]) > int(expected.rgba[i]) + 12) {
            ++shadow_pixels;
            for (int channel = 0; channel < 3; ++channel)
                require(std::abs(int(expected.rgba[i + channel]) -
                                 int(actual.rgba[i + channel])) <= 16,
                        "GPU culling removed the visible shadow of an offscreen caster");
        }
    require(shadow_pixels > 20,
            "offscreen shadow fixture did not cast a visible shadow on the receiver");
}

void camera_cut() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuOcclusion);
    auto scene = doorway(false);
    frame(gpu, scene);
    scene.eye = {0, 0, -8};
    scene.view_projection = multiply(perspective(.85f, 320.f / 240.f, .1f, 100),
                                     look_at(scene.eye, {0, 0, 0}));
    scene.camera_cut = true;
    compare(frame(direct, scene), frame(gpu, scene), "camera cut");
}

void odd_resize() {
    auto direct = make_renderer(VisibilityMode::Direct, 319, 241);
    auto gpu = make_renderer(VisibilityMode::GpuOcclusion, 319, 241);
    auto scene = doorway(false);
    scene.view_id = "acceptance-resize";
    frame(gpu, scene);
    direct.resize(321, 239);
    gpu.resize(321, 239);
    scene.camera_cut = true;
    scene.view_projection = multiply(perspective(.85f, 321.f / 239.f, .1f, 100),
                                     look_at(scene.eye, {0, 0, 0}));
    const auto expected = frame(direct, scene);
    const auto actual = frame(gpu, scene);
    require(actual.rgba.size() == 321u * 239u * 4u, "odd resize returned stale image extent");
    compare(expected, actual, "odd resize");
}

std::shared_ptr<const Mesh> coarse_cube() {
    static const auto mesh = [] {
        auto result = std::make_shared<Mesh>(*cube_mesh());
        for (auto& vertex : result->vertices)
            for (auto& coordinate : vertex.position)
                coordinate *= .6f;
        return result;
    }();
    return mesh;
}

Snapshot lod_scene(float scale) {
    Snapshot result;
    result.view_id = "acceptance-lod";
    result.eye = {0, 0, 8};
    result.view_projection = multiply(orthographic(-2, 2, -2, 2, .1f, 30),
                                      look_at(result.eye, {0, 0, 0}));
    auto item = cube("lod-object", {0, 0, 0}, {scale, scale, scale});
    item.cast_shadow = false;
    item.lod_meshes.push_back(coarse_cube());
    result.draws.push_back(std::move(item));
    return result;
}

void prepared_lod_hysteresis() {
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto direct = make_renderer(VisibilityMode::Direct);
    auto near = lod_scene(4.f);
    const auto fine = frame(gpu, near);
    require(fine.stats.lod_counts[0] == 1, "near prepared mesh did not use LOD 0");
    near.draws.front().lod_meshes.clear();
    compare(frame(direct, near), fine, "near prepared LOD");

    float transition_scale = 0;
    for (float scale = 3.92f; scale >= .5f; scale *= .98f) {
        auto sample = lod_scene(scale);
        const auto actual = frame(gpu, sample);
        if (actual.stats.lod_counts[1] == 1) {
            transition_scale = scale;
            sample.draws.front().mesh = coarse_cube();
            sample.draws.front().lod_meshes.clear();
            compare(frame(direct, sample), actual, "prepared coarse LOD");
            break;
        }
    }
    require(transition_scale > 0, "prepared mesh never transitioned to LOD 1");

    auto jitter = lod_scene(transition_scale * 1.04f);
    const auto stable = frame(gpu, jitter);
    require(stable.stats.lod_counts[1] == 1,
            "small reverse scale jitter changed LOD despite hysteresis");
    jitter.draws.front().mesh = coarse_cube();
    jitter.draws.front().lod_meshes.clear();
    compare(frame(direct, jitter), stable, "LOD hysteresis");

    auto return_near = lod_scene(transition_scale * 1.4f);
    const auto recovered = frame(gpu, return_near);
    require(recovered.stats.lod_counts[0] == 1,
            "LOD hysteresis failed to return to the fine mesh after a large scale change");
}

void csv_field(std::ostream& output, std::string_view value) {
    output << '"';
    for (char character : value) {
        if (character == '"')
            output << '"';
        output << character;
    }
    output << '"';
}

void benchmark(const std::filesystem::path& output) {
    std::ofstream csv(output);
    require(bool(csv), "cannot open benchmark output: " + output.string());
    csv << "scenario,mode,frame,device,cpu_ms,gpu_ms,readback_cpu_ms,gpu_bytes,draw_calls,"
           "gpu_bins,gpu_visible,gpu_frustum_rejected,gpu_deferred,gpu_post_visible,"
           "lod0,lod1,lod2,lod3,hzb_valid,gpu_visibility_active,validation_errors\n";
    auto frustum = grid(1024, 32, .7f);
    auto open = grid(0);
    open.view_id = "benchmark-open";
    for (std::size_t i = 0; i < 1024; ++i) {
        const float x = (float(i % 32) - 15.5f) * .48f;
        const float y = (float(i / 32) - 15.5f) * .48f;
        open.draws.push_back(cube("open-" + std::to_string(i), {x, y, 0}, {.18f, .18f, .18f}));
    }
    auto occluded = doorway(false);
    occluded.view_id = "benchmark-occluded";
    for (std::size_t i = 0; i < 1024; ++i) {
        const float x = (float(i % 32) - 15.5f) * .13f;
        const float y = (float(i / 32) - 15.5f) * .13f;
        occluded.draws.push_back(
            cube("hidden-" + std::to_string(i), {x, y, -3}, {.1f, .1f, .1f}));
    }
    for (auto* scene : {&frustum, &open, &occluded})
        for (auto& draw : scene->draws)
            draw.cast_shadow = false;
    struct Workload {
        const char* name;
        Snapshot* snapshot;
    };
    for (const auto workload : {Workload{"frustum", &frustum}, Workload{"open", &open},
                                Workload{"occluded", &occluded}}) {
        for (const auto mode : {VisibilityMode::Direct, VisibilityMode::GpuFrustum,
                                VisibilityMode::GpuOcclusion}) {
            auto renderer = make_renderer(mode);
            for (int warmup = 0; warmup < 10; ++warmup)
                renderer.render(*workload.snapshot);
            for (int sample = 0; sample < 30; ++sample) {
                renderer.render(*workload.snapshot);
                const auto& s = renderer.stats();
                const char* name = mode == VisibilityMode::Direct      ? "direct"
                                   : mode == VisibilityMode::GpuFrustum ? "gpu_frustum"
                                                                        : "gpu_occlusion";
                csv << workload.name << ',' << name << ',' << sample << ',';
                csv_field(csv, s.device);
                csv << ',' << s.cpu_ms << ',' << s.gpu_ms << ',' << s.readback_cpu_ms << ','
                    << s.gpu_allocated_bytes << ',' << s.draw_calls << ',' << s.gpu_bins << ','
                    << s.gpu_visible_instances << ',' << s.gpu_frustum_rejected << ','
                    << s.gpu_occlusion_deferred << ',' << s.gpu_post_visible;
                for (auto count : s.lod_counts)
                    csv << ',' << count;
                csv << ',' << (s.hzb_valid ? 1 : 0) << ','
                    << (s.gpu_visibility_active ? 1 : 0) << ',' << s.validation_errors << '\n';
            }
        }
    }
    require(bool(csv), "failed to write benchmark output: " + output.string());
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--benchmark") {
            benchmark(argv[2]);
            return 0;
        }
        require(argc == 3 && std::string_view(argv[1]) == "--case",
                "usage: faset_render_gpu_acceptance_tests --case <name> | --benchmark <csv>");
        const std::string_view name = argv[2];
        if (name == "empty")
            empty_and_retirement();
        else if (name == "capacity")
            exact_capacity();
        else if (name == "dense")
            dense_frustum();
        else if (name == "door")
            door_reveal();
        else if (name == "shadow")
            offscreen_shadow();
        else if (name == "cut")
            camera_cut();
        else if (name == "resize")
            odd_resize();
        else if (name == "lod")
            prepared_lod_hysteresis();
        else
            throw std::invalid_argument("unknown acceptance case: " + std::string(name));
        std::cout << "P2 GPU visibility acceptance: " << name << " passed\n";
    } catch (const std::exception& exception) {
        std::cerr << "P2 GPU visibility acceptance: " << exception.what() << '\n';
        return 1;
    }
}
