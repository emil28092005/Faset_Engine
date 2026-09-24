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
Renderer make_renderer(VisibilityMode mode, LightingMode lighting = LightingMode::Auto) {
    RendererConfig config;
    config.width = 320;
    config.height = 240;
    config.headless = true;
    config.validation = true;
    config.visibility_mode = mode;
    config.lighting_mode = lighting;
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
Snapshot local_scene(LocalLight::Kind kind, bool caster_shadow) {
    Snapshot result;
    result.view_id = "p3-local-shadow";
    result.eye = {0, 5, 8};
    const auto view = look_at(result.eye, {0, -1, 0});
    const auto projection = orthographic(-3, 3, -2.25f, 2.25f, .1f, 50);
    result.projection = projection;
    result.view_projection = multiply(projection, view);
    result.camera_frustum = CameraFrustum{view, projection, .1f, 50, false};
    result.authored_lights_present = true;
    DrawItem floor;
    floor.mesh = cube_mesh();
    floor.model = transform({0, -1, 0}, {}, {8, .1f, 8});
    floor.color = {.8f, .8f, .8f, 1};
    floor.instance_key = "floor";
    result.draws.push_back(floor);
    DrawItem caster;
    caster.mesh = cube_mesh();
    caster.model = transform({0, .7f, 0}, {}, {.8f, .8f, .8f});
    caster.color = {.4f, .4f, .4f, 1};
    caster.cast_shadow = caster_shadow;
    caster.instance_key = "caster";
    result.draws.push_back(caster);
    LocalLight light;
    light.kind = kind;
    light.stable_id = "local";
    light.position = {0, 3, 0};
    light.direction = {0, -1, 0};
    light.color = {1, .85f, .65f, 1};
    light.intensity = 80;
    light.range = 8;
    light.inner_angle = .3f;
    light.outer_angle = .7f;
    result.local_lights.push_back(light);
    return result;
}
std::size_t darker_pixels(const Frame& shadowed, const Frame& unshadowed) {
    std::size_t count{};
    for (std::size_t i = 0; i < shadowed.pixels.size(); i += 4)
        count += int(unshadowed.pixels[i]) > int(shadowed.pixels[i]) + 12;
    return count;
}
Snapshot point_face_scene(Vec3 axis, bool caster_shadow) {
    Snapshot result;
    result.view_id = "point-six-faces";
    const Vec3 lateral = std::abs(axis[1]) > .9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    result.eye = {-axis[0] * .4f + lateral[0] * 2,
                  -axis[1] * .4f + lateral[1] * 2,
                  -axis[2] * .4f + lateral[2] * 2};
    const Vec3 target{axis[0] * 3, axis[1] * 3, axis[2] * 3};
    const auto view = look_at(result.eye, target);
    const auto projection = orthographic(-2, 2, -2, 2, .1f, 20);
    result.projection = projection;
    result.view_projection = multiply(projection, view);
    result.camera_frustum = CameraFrustum{view, projection, .1f, 20, false};
    result.authored_lights_present = true;
    DrawItem receiver;
    receiver.mesh = cube_mesh();
    receiver.model = transform(target, {}, {1.5f, 1.5f, 1.5f});
    receiver.color = {.8f, .8f, .8f, 1};
    receiver.instance_key = "point-receiver";
    result.draws.push_back(receiver);
    DrawItem caster;
    caster.mesh = cube_mesh();
    caster.model = transform({axis[0] * 1.5f, axis[1] * 1.5f, axis[2] * 1.5f},
                             {}, {.5f, .5f, .5f});
    caster.cast_shadow = caster_shadow;
    caster.instance_key = "point-caster";
    result.draws.push_back(caster);
    LocalLight light;
    light.stable_id = "point-face";
    light.position = {0, 0, 0};
    light.range = 8;
    light.intensity = 90;
    result.local_lights.push_back(light);
    return result;
}
void local() {
    auto direct = make_renderer(VisibilityMode::Direct);
    auto gpu = make_renderer(VisibilityMode::GpuFrustum);
    auto occlusion = make_renderer(VisibilityMode::GpuOcclusion);
    for (auto kind : {LocalLight::Kind::Point, LocalLight::Kind::Spot}) {
        const auto scene_with_shadow = local_scene(kind, true);
        const auto shadowed = capture(direct, scene_with_shadow);
        const auto gpu_shadowed = capture(gpu, scene_with_shadow);
        const auto occlusion_shadowed = capture(occlusion, scene_with_shadow);
        const auto unshadowed = capture(direct, local_scene(kind, false));
        const auto faces = kind == LocalLight::Kind::Point ? 6u : 1u;
        require(shadowed.stats.local_shadow_faces == faces &&
                    shadowed.stats.requested_local_shadow_faces == faces &&
                    shadowed.stats.shadow_caster_draws <= 4096 &&
                    shadowed.stats.local_shadow_atlas_bytes > 0 &&
                    shadowed.stats.gpu_local_shadow_ms > 0,
                "Point/spot views render within atlas and caster budgets");
        require(darker_pixels(shadowed, unshadowed) > 20,
                "Caster darkens point/spot-lit receiver (count=" +
                    std::to_string(darker_pixels(shadowed, unshadowed)) + ")");
        require(shadowed.stats.validation_errors == 0 &&
                    gpu_shadowed.stats.validation_errors == 0 &&
                    occlusion_shadowed.stats.validation_errors == 0,
                "Local shadow rendering passes Vulkan validation");
        compare_frames(shadowed, gpu_shadowed);
        compare_frames(shadowed, occlusion_shadowed);
    }
    for (const Vec3 axis : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                            Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1},
                            Vec3{.7071068f, .7071068f, 0}}) {
        const auto shadowed = capture(direct, point_face_scene(axis, true));
        const auto unshadowed = capture(direct, point_face_scene(axis, false));
        require(shadowed.stats.local_shadow_faces == 6 &&
                    darker_pixels(shadowed, unshadowed) > 5,
                "A point light shadows each face direction and the adjacent-face seam");
    }
    auto crowded = local_scene(LocalLight::Kind::Point, true);
    const auto point = crowded.local_lights.front();
    crowded.local_lights.clear();
    for (int i = 0; i < 15; ++i) {
        LocalLight filler;
        filler.kind = LocalLight::Kind::Spot;
        filler.stable_id = "filler-" + std::to_string(i);
        filler.position = {100, 100, 100};
        filler.direction = {0, -1, 0};
        filler.range = 8;
        filler.intensity = 1;
        filler.shadow_priority = 10;
        crowded.local_lights.push_back(filler);
    }
    const auto without_point = capture(direct, crowded);
    crowded.local_lights.push_back(point);
    const auto overflow = capture(direct, crowded);
    require(overflow.stats.requested_local_shadow_faces == 21 &&
                overflow.stats.dropped_point_shadow_faces == 6 &&
                overflow.stats.shadow_atlas_full_drops == 6 &&
                overflow.stats.local_shadow_tiles <= 16,
            "Fifteen occupied tiles drop the complete six-face point shadow");
    std::size_t brightened{};
    for (std::size_t i = 0; i < overflow.pixels.size(); i += 4)
        brightened += int(overflow.pixels[i]) > int(without_point.pixels[i]) + 12;
    require(brightened > 20,
            "Point light with dropped atlas faces still illuminates unshadowed");
}
void tiled() {
    for (auto visibility : {VisibilityMode::Direct, VisibilityMode::GpuFrustum,
                            VisibilityMode::GpuOcclusion}) {
        auto forward = make_renderer(visibility, LightingMode::Forward);
        auto tiles = make_renderer(visibility, LightingMode::Tiled);
        auto fixture = local_scene(LocalLight::Kind::Point, false);
        auto no_lights = fixture;
        no_lights.local_lights.clear();
        const auto empty_tiled = capture(tiles, no_lights);
        require(empty_tiled.stats.effective_lighting_path == "forward" &&
                    empty_tiled.stats.light_tile_count == 0,
                "Forced tiles correctly fall back when no local lights are submitted");
        fixture.scene_rect = {32, 24, 256, 192};
        fixture.local_lights.front().casts_shadow = false;
        auto spot = fixture.local_lights.front();
        spot.kind = LocalLight::Kind::Spot;
        spot.stable_id = "second-spot";
        spot.position = {1.5f, 2, 0};
        spot.direction = {0, -1, 0};
        spot.intensity = 7;
        spot.range = 4;
        fixture.local_lights.push_back(spot);
        auto outside = spot;
        outside.stable_id = "offscreen-light";
        outside.position = {100, 100, 100};
        outside.range = 2;
        fixture.local_lights.push_back(outside);
        const auto expected = capture(forward, fixture);
        const auto actual = capture(tiles, fixture);
        require(expected.stats.effective_lighting_path == "forward" &&
                    actual.stats.effective_lighting_path == "tiled" &&
                    actual.stats.gpu_light_tiles_ms > 0 &&
                    actual.stats.light_tile_count > 0,
                "Forced 16x16 tile construction reports its actual GPU work");
        require(actual.stats.validation_errors == 0,
                "Forward+ tile build and fragment reads pass Vulkan validation");
        require(actual.stats.light_tile_overflow_count == 0 &&
                    actual.stats.light_tile_candidate_count <
                        actual.stats.light_tile_count * 3,
                "Depth-free tile lists exclude an offscreen light without overflow");
        compare_frames(expected, actual);

        auto near_plane = local_scene(LocalLight::Kind::Point, true);
        near_plane.local_lights.front().position = {0, 5, 7.95f};
        near_plane.local_lights.front().range = 15;
        const auto near_forward = capture(forward, near_plane);
        const auto near_tiled = capture(tiles, near_plane);
        require(near_tiled.stats.effective_lighting_path == "tiled" &&
                    near_tiled.stats.light_tile_counts_valid,
                "Near-plane crossing light and its shadow use actual tile lists");
        compare_frames(near_forward, near_tiled);

        forward.resize(336, 256);
        tiles.resize(336, 256);
        fixture.scene_rect = {40, 32, 248, 176};
        const auto resized_forward = capture(forward, fixture);
        const auto resized_tiled = capture(tiles, fixture);
        require(resized_tiled.stats.light_tile_count == 21 * 16,
                "Forward+ rebuilds its grid after a drawable resize");
        compare_frames(resized_forward, resized_tiled);

        // Eighty coincident lights cover the same central tiles. A 64-index tile
        // must evaluate the entire submitted list instead of losing late lights.
        fixture.local_lights.clear();
        for (int i = 0; i < 80; ++i) {
            auto light = point_face_scene({0, 0, 1}, false).local_lights.front();
            light.stable_id = "overflow-" + std::to_string(i);
            light.position = {0, 3, 0};
            light.intensity = .45f;
            light.range = 8;
            light.casts_shadow = false;
            fixture.local_lights.push_back(light);
        }
        const auto all_forward = capture(forward, fixture);
        const auto all_tiled = capture(tiles, fixture);
        auto automatic = make_renderer(visibility, LightingMode::Auto);
        const auto dense_auto = capture(automatic, fixture);
        require(dense_auto.stats.effective_lighting_path == "forward" &&
                    dense_auto.stats.light_tile_count == 0,
                "Auto avoids tile construction for unmeasured dense overlap");
        require(all_tiled.stats.submitted_local_lights == 80 &&
                    all_tiled.stats.effective_lighting_path == "tiled" &&
                    all_tiled.stats.light_tile_overflow_count > 0,
                "Overflow fixture submits all eighty lights through Forward+");
        compare_frames(all_forward, all_tiled);
        fixture.local_lights.resize(64);
        const auto first_sixty_four = capture(forward, fixture);
        std::size_t extra_light_pixels{};
        for (std::size_t i = 0; i < all_forward.pixels.size(); i += 4)
            extra_light_pixels += int(all_forward.pixels[i]) >
                                  int(first_sixty_four.pixels[i]) + 2;
        require(extra_light_pixels > 20,
                "Overflow fixture visibly depends on lights past index 63");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("Expected --sun, --local, or --tiled");
        if (std::string(argv[1]) == "--sun")
            sun();
        else if (std::string(argv[1]) == "--local")
            local();
        else if (std::string(argv[1]) == "--tiled")
            tiled();
        else
            throw std::invalid_argument("Expected --sun, --local, or --tiled");
        std::cout << "Shadow atlas and Direct/GPU lighting parity passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
