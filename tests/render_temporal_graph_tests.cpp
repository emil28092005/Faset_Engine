#include "render_temporal_fixtures.hpp"
#include <faset/render/temporal.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace faset::render;
using namespace faset::render::temporal_test;

namespace {
std::size_t position(const std::vector<std::string>& passes, const char* name) {
    const auto found = std::find(passes.begin(), passes.end(), name);
    require(found != passes.end(), "Required temporal graph pass is absent");
    return static_cast<std::size_t>(found - passes.begin());
}

void offset_scene_and_sharp_ui() {
    constexpr std::uint32_t width = 191, height = 127;
    auto config = headless_config(width, height, VisibilityMode::GpuOcclusion);
    config.temporal_mode = TemporalMode::TAA;
    Renderer taa(config);
    config.temporal_mode = TemporalMode::Off;
    Renderer off(config);

    auto frame = lit_scene(141, 103);
    frame.scene_rect = {13, 7, 141, 103};
    frame.draws.push_back(cube({0, 0, 0}, {.9f, .55f, .25f, 1}, "opaque"));
    frame.draws.push_back(cube({.4f, 0, 1}, {.2f, .6f, .9f, .45f}, "translucent"));
    Sprite sprite;
    sprite.position = {-.7f, -.4f, .7f};
    sprite.size = {.8f, .8f};
    sprite.color = {.3f, .8f, .3f, .8f};
    frame.sprites.push_back(sprite);
    frame.ui_quads.push_back({2, 2, 24, 14, {.95f, .8f, .2f, 1}});
    frame.ui_text.push_back({2, 22, "Temporal UI", {1, 1, 1, 1}, 12});

    taa.render(frame);
    off.render(frame);
    const auto& stats = taa.stats();
    require(stats.effective_temporal_mode == TemporalMode::TAA &&
                stats.effective_visibility_mode == VisibilityMode::GpuOcclusion,
            "Graph acceptance must exercise temporal resolve after P2 occlusion");
    const auto& passes = stats.graph_passes;
    require(position(passes, "PostRasterScene") < position(passes, "TemporalResolve") &&
                position(passes, "TemporalResolve") <
                    position(passes, "TemporalComposite") &&
                position(passes, "TemporalComposite") < position(passes, "UI"),
            "Post-cull scene color/velocity must resolve before final-resolution UI");
    require(pixel(taa.pixels(), width, 4, 4) == pixel(off.pixels(), width, 4, 4),
            "Opaque UI remains pixel-exact and unjittered on an offset scene viewport");
    require(pixel(taa.pixels(), width, 180, 120) == pixel(off.pixels(), width, 180, 120),
            "Pixels outside the offset scene rectangle retain the Off clear result");
    require(stats.validation_errors == 0,
            "Temporal graph attachment store/load and transitions pass Vulkan validation");
    if (stats.gpu_ms > 0)
        require(stats.gpu_temporal_resolve_ms > 0 &&
                    stats.gpu_temporal_composite_ms > 0,
                "Temporal resolve and composite expose independent GPU pass timings");
    require(stats.gpu_allocated_bytes > off.stats().gpu_allocated_bytes,
            "Temporal scene, velocity and output histories count toward live GPU memory");
}
} // namespace

int main() {
    offset_scene_and_sharp_ui();
}
