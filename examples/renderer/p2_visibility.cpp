#include <faset/render/renderer.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace faset::render;

namespace {
std::shared_ptr<const Mesh> coarse_mesh() {
    auto mesh = std::make_shared<Mesh>();
    mesh->vertices = {
        Vertex{{-1, -1, -1}, {-1, -1, -1}},
        Vertex{{1, -1, -1}, {1, -1, -1}},
        Vertex{{0, 1, 0}, {0, 1, 0}},
        Vertex{{0, -1, 1}, {0, -1, 1}},
    };
    mesh->indices = {0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3};
    return mesh;
}

VisibilityMode mode_from_argument(std::string_view argument) {
    if (argument == "direct")
        return VisibilityMode::Direct;
    if (argument == "frustum")
        return VisibilityMode::GpuFrustum;
    if (argument == "occlusion")
        return VisibilityMode::GpuOcclusion;
    throw std::invalid_argument("mode must be direct, frustum, or occlusion");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3)
            throw std::invalid_argument("usage: faset_p2_visibility_example "
                                        "[direct|frustum|occlusion] [capture.ppm]");
        RendererConfig config;
        config.width = 640;
        config.height = 360;
        config.headless = true;
        config.visibility_mode = argc > 1 ? mode_from_argument(argv[1])
                                          : VisibilityMode::GpuOcclusion;
        config.visibility_diagnostics = true;
        Renderer renderer(config);

        Snapshot frame;
        frame.view_id = "p2-lod-example-camera";
        frame.eye = {0, 2, 12};
        frame.projection = perspective(.9f, float(config.width) / config.height, .1f, 100);
        frame.view_projection = multiply(frame.projection,
                                         look_at(frame.eye, {0, 0, 0}));
        const auto prepared_coarse = coarse_mesh();
        for (int i = 0; i < 9; ++i) {
            DrawItem item;
            item.mesh = cube_mesh();
            item.lod_meshes = {prepared_coarse};
            item.instance_key = "example/cube/" + std::to_string(i);
            const bool near_detail = i == 2;
            item.model = transform({float(i - 4) * 1.8f, 0,
                                    near_detail ? 6.f : (i % 2 ? -5.f : 0.f)},
                                   {}, near_detail ? Vec3{4, 4, 4} : Vec3{1, 1, 1});
            item.color = {0.28f + .07f * i, .6f, .8f, 1};
            item.cast_shadow = false;
            frame.draws.push_back(std::move(item));
        }

        // The second and later frames have compatible HZB history for this view.
        for (int i = 0; i < 3; ++i)
            renderer.render(frame);
        const auto& stats = renderer.stats();
        renderer.capture(argc > 2 ? std::filesystem::path(argv[2])
                                  : std::filesystem::path("p2-visibility.ppm"));
        std::cout << "GPU path: " << (stats.gpu_visibility_active ? "active" : "unavailable")
                  << ", bins: " << stats.gpu_bins
                  << ", visible: " << stats.gpu_visible_instances
                  << ", LOD0: " << stats.lod_counts[0]
                  << ", LOD1: " << stats.lod_counts[1]
                  << ", validation errors: " << stats.validation_errors << '\n';
        return stats.validation_errors ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
