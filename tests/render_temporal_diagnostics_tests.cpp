#include "render_temporal_fixtures.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace faset::render;
using namespace faset::render::temporal_test;

namespace {
void pixel_decisions_are_diagnostic_only(VisibilityMode visibility) {
    constexpr std::uint32_t width = 96, height = 72;
    auto config = headless_config(width, height, visibility);
    config.temporal_mode = TemporalMode::TAA;
    Renderer renderer(config);
    Renderer reference(config);
    auto frame = lit_scene(width, height);
    frame.draws.push_back(cube({0, 0, 0}, {.9f, .4f, .2f, 1}, "diagnostic-cube"));

    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 0,
            "Normal TAA must not read back per-pixel history diagnostics");

    renderer.set_temporal_diagnostics(true);
    renderer.render(frame);
    reference.render(frame);
    const auto accepted = renderer.stats().temporal_accepted_pixels;
    const auto rejected = renderer.stats().temporal_rejected_pixels;
    require(renderer.stats().temporal_counters_valid && accepted > 0 &&
                accepted + rejected == width * height,
            "Requested TAA diagnostics count each scene pixel and reuse eligible history");
    require(renderer.pixels() == reference.pixels(),
            "Counting pixels must not alter the TAA image");

    frame.camera_cut = true;
    renderer.render(frame);
    reference.render(frame);
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == width * height,
            "A camera cut rejects every per-pixel history sample");

    frame.camera_cut = false;
    frame.scene_rect = {7, 9, 80, 50};
    renderer.render(frame);
    reference.render(frame);
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 80 * 50,
            "The counter covers the scene rectangle, excluding output chrome pixels");

    renderer.set_temporal_diagnostics(false);
    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels == 0,
            "Disabled diagnostics expose no stale values and perform no readback");

    renderer.set_temporal_mode(TemporalMode::Off);
    reference.set_temporal_mode(TemporalMode::Off);
    renderer.set_temporal_diagnostics(true);
    renderer.render(frame);
    reference.render(frame);
    require(!renderer.stats().temporal_counters_valid,
            "Off mode does not claim temporal pixel diagnostics");
    require(renderer.stats().validation_errors == 0,
            "Diagnostic counter transitions pass Vulkan validation");
    require(renderer.pixels() == reference.pixels(),
            "Diagnostic toggles preserve the Off image");
}

int resize_recreates_swapchain_with_diagnostics() {
#ifndef _WIN32
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        return 77;
#endif
    // A headless Vulkan ICD can render the other diagnostics tests but cannot
    // create the SDL surface needed to exercise swapchain recreation.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return 77;
    Uint32 required_count{};
    const auto* required = SDL_Vulkan_GetInstanceExtensions(&required_count);
    std::uint32_t available_count{};
    const auto enumerated = vkEnumerateInstanceExtensionProperties(
        nullptr, &available_count, nullptr) == VK_SUCCESS;
    std::vector<VkExtensionProperties> available(available_count);
    const bool queried = enumerated &&
        vkEnumerateInstanceExtensionProperties(nullptr, &available_count,
                                               available.data()) == VK_SUCCESS;
    const bool surface_supported = required && queried &&
        std::all_of(required, required + required_count, [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const auto& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        });
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (!surface_supported)
        return 77;
    std::jthread watchdog([](std::stop_token stop) {
        for (int i = 0; i < 150 && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!stop.stop_requested())
            std::_Exit(70);
    });
    RendererConfig config;
    config.width = 320;
    config.height = 240;
    config.headless = false;
    config.validation = true;
    config.temporal_mode = TemporalMode::TAA;
    config.temporal_diagnostics = true;
    Renderer renderer(config);
    auto frame = lit_scene(config.width, config.height);
    frame.draws.push_back(cube({0, 0, 0}, {.9f, .4f, .2f, 1}, "resized-cube"));
    renderer.render(frame);
    require(renderer.stats().temporal_counters_valid,
            "Initial window frame must have valid temporal pixel counts");
    const auto previous_width = renderer.width();
    const auto previous_height = renderer.height();
    renderer.resize(480, 270);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        SDL_PumpEvents();
        int pixel_width{}, pixel_height{};
        int window_count{};
        auto windows = SDL_GetWindows(&window_count);
        if (windows && window_count == 1)
            SDL_GetWindowSizeInPixels(windows[0], &pixel_width, &pixel_height);
        SDL_free(windows);
        if (pixel_width > 0 && pixel_height > 0 &&
            (std::uint32_t(pixel_width) != previous_width ||
             std::uint32_t(pixel_height) != previous_height))
            break;
        SDL_Delay(10);
    }
    if (std::chrono::steady_clock::now() >= deadline)
        return 77;
    renderer.render(frame);
    require(renderer.width() != previous_width || renderer.height() != previous_height,
            "Render must recreate swapchain and temporal targets at the new extent");
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels == 0 &&
                renderer.stats().temporal_rejected_pixels ==
                    renderer.width() * renderer.height() &&
                renderer.stats().validation_errors == 0,
            "Resize frame must reject old history and read back a fresh counter buffer");
    renderer.render(frame);
    require(renderer.stats().temporal_counters_valid &&
                renderer.stats().temporal_accepted_pixels +
                    renderer.stats().temporal_rejected_pixels ==
                    renderer.width() * renderer.height() &&
                renderer.stats().validation_errors == 0,
            "Following frame must retain valid temporal counters after resize");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--window-resize")
            return resize_recreates_swapchain_with_diagnostics();
        require(argc == 1, "Unknown temporal diagnostics test arguments");
        pixel_decisions_are_diagnostic_only(VisibilityMode::Direct);
        pixel_decisions_are_diagnostic_only(VisibilityMode::GpuFrustum);
        std::cout << "Temporal pixel diagnostics are opt-in and exact\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
