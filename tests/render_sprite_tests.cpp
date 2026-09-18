#include <cmath>
#include <faset/render/renderer.hpp>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace faset::render;
namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    try {
        Renderer renderer({64, 64, "Faset sprite alpha validation", true, true});
        Snapshot scene;
        scene.clear_color = {0, 0, 0, 1};
        Sprite front{{0, 0, .2f}, {2, 2}, {0, 1, 0, 0}};
        Sprite back{{0, 0, .8f}, {2, 2}, {1, 0, 0, 1}};
        auto color = [&](int red, int green, const char* message) {
            renderer.render(scene);
            const auto pixels = renderer.pixels();
            const auto index = (32 * 64 + 32) * 4;
            require(std::abs(int(pixels[index]) - red) <= 2 &&
                        std::abs(int(pixels[index + 1]) - green) <= 2 && pixels[index + 2] <= 2,
                    message);
            require(renderer.stats().validation_errors == 0, "Sprite Vulkan validation error");
        };
        scene.sprites = {front, back};
        color(255, 0, "Fully transparent front sprite must not occlude a later opaque sprite");
        front.color[3] = .5f;
        scene.sprites = {front, back};
        color(128, 128, "Partial alpha must composite over the farther sprite");
        scene.sprites = {back, front};
        color(128, 128, "Unequal-depth compositing must be independent of submission order");
        front.position[2] = .9f;
        front.layer = 5;
        scene.sprites = {front, back};
        color(128, 128, "Explicit higher layer must take priority over sprite depth");
        front.position[2] = back.position[2];
        front.layer = back.layer;
        scene.sprites = {front, back};
        color(255, 0, "Equal-layer/equal-depth sprites must preserve submission order");
        scene.sprites = {back, front};
        color(128, 128, "Later equal-depth sprite must composite on top");
        auto texture = std::make_shared<Texture>();
        texture->width = texture->height = 1;
        texture->rgba = {0, 255, 0, 0};
        front.color = {1, 1, 1, 1};
        front.texture = texture;
        front.position[2] = .2f;
        scene.sprites = {front, back};
        color(255, 0, "Transparent texture pixels must preserve underlying sprites");
        texture->rgba[3] = 128;
        ++texture->revision;
        color(127, 128, "Texture alpha must blend with the already rendered background");
        auto opaque = std::make_shared<Mesh>();
        for (const auto point : {Vec3{-1, -1, .4f}, Vec3{1, -1, .4f}, Vec3{1, 1, .4f},
                                 Vec3{-1, -1, .4f}, Vec3{1, 1, .4f}, Vec3{-1, 1, .4f}})
            opaque->vertices.push_back({point, {0, 0, 0}, {1, 0, 0, 1}});
        scene.draws.push_back({opaque, identity, {1, 1, 1, 1}, .5f, 0, false});
        front.position[2] = .8f;
        scene.sprites = {front};
        color(255, 0, "Sprites behind opaque mesh depth must remain occluded");
        front.position[2] = .2f;
        scene.sprites = {front};
        color(127, 128, "Sprites in front of opaque mesh depth must composite normally");
        std::cout << "Sprite depth/layer ordering, stable ties, color/texture alpha and opaque "
                     "mesh occlusion passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
