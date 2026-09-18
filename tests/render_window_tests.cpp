#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <faset/render/renderer.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace faset::render;
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct WindowEvents {
    SDL_WindowID id{};
    unsigned minimized{}, restored{}, resized{}, focus_lost{};
    static bool watch(void* context, SDL_Event* event) {
        auto& state = *static_cast<WindowEvents*>(context);
        if (event->window.windowID == state.id) {
            if (event->type == SDL_EVENT_WINDOW_MINIMIZED)
                ++state.minimized;
            if (event->type == SDL_EVENT_WINDOW_RESTORED)
                ++state.restored;
            if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                ++state.resized;
            if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
                ++state.focus_lost;
        }
        return true;
    }
};
} // namespace

int main(int argc, char** argv) {
    // Some window systems can block inside native window calls. Terminate only this
    // fixture if that happens, so an acceptance test cannot strand its own process.
    std::jthread watchdog([](std::stop_token stop) {
        for (int i = 0; i < 150 && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!stop.stop_requested()) {
            std::cerr << "Window fixture exceeded 30 seconds; terminating its own process\n";
            std::_Exit(70);
        }
    });
    try {
        Renderer renderer({320, 240, "Faset owned window lifecycle test", false, true});
        int count{};
        auto windows = SDL_GetWindows(&count);
        require(windows && count == 1, "Fixture must own exactly one SDL window");
        auto* window = windows[0];
        SDL_free(windows);
        WindowEvents events{SDL_GetWindowID(window)};
        require(SDL_AddEventWatch(WindowEvents::watch, &events), "Register lifecycle event watch");
        struct WatchGuard {
            WindowEvents& events;
            ~WatchGuard() {
                SDL_RemoveEventWatch(WindowEvents::watch, &events);
            }
        } watch{events};
        std::cout << "driver=" << SDL_GetCurrentVideoDriver() << " stage=created\n" << std::flush;
        Snapshot scene;
        scene.ui_quads.push_back({0, 0, 4096, 4096, {1, 0, 0, 1}});
        unsigned frame{};
        auto draw = [&] {
            renderer.poll_events();
            const float native_scale = SDL_GetWindowDisplayScale(window);
            require(std::isfinite(renderer.display_scale()) && renderer.display_scale() > 0.f,
                    "Renderer UI display scale must be finite and positive");
            if (std::isfinite(native_scale) && native_scale > 0.f)
                require(std::abs(renderer.display_scale() - native_scale) < 0.001f,
                        "Renderer UI display scale must follow SDL's live window scale");
            const unsigned channel = frame++ % 2;
            scene.ui_quads[0].color = channel ? Color{0, 1, 0, 1} : Color{1, 0, 0, 1};
            renderer.render(scene);
            const auto pixels = renderer.pixels();
            require(pixels.size() == std::size_t(renderer.width()) * renderer.height() * 4,
                    "Window capture dimensions must match the current target");
            require(pixels.at((8 * renderer.width() + 8) * 4 + channel) > 240,
                    "Window capture must contain the current frame, including while minimized");
            require(renderer.stats().validation_errors == 0, "Vulkan validation error");
        };
        auto await = [&](auto condition, int milliseconds) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
            do {
                draw();
                if (condition())
                    return true;
                SDL_Delay(10);
            } while (Clock::now() < deadline);
            return false;
        };
        for (const auto& [width, height] : {std::pair{480, 270}, std::pair{360, 300}}) {
            renderer.resize(width, height);
            require(await(
                        [&] {
                            int logical_width{}, logical_height{}, pixel_width{}, pixel_height{};
                            SDL_GetWindowSize(window, &logical_width, &logical_height);
                            SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
                            return logical_width == width && logical_height == height &&
                                   renderer.width() == static_cast<unsigned>(pixel_width) &&
                                   renderer.height() == static_cast<unsigned>(pixel_height);
                        },
                        2000),
                    "Window/target must converge to the requested resize");
        }
        std::cout << "stage=resized width=" << renderer.width() << " height=" << renderer.height()
                  << " resize_events=" << events.resized << '\n'
                  << std::flush;
        // Allow initial configure/focus events to settle before requesting a new state.
        for (int i = 0; i < 25; ++i) {
            draw();
            SDL_Delay(10);
        }
        {
            // Do not replace an image/rich clipboard with its text projection.
            // Plain text is restored without ever printing the saved contents.
            std::size_t mime_count{};
            auto** mime_types = SDL_GetClipboardMimeTypes(&mime_count);
            bool plain_text_only = true;
            for (std::size_t i = 0; i < mime_count; ++i) {
                const std::string type = mime_types[i];
                if (!type.starts_with("text/plain") && type != "UTF8_STRING" && type != "TEXT" &&
                    type != "STRING" && type != "COMPOUND_TEXT" && type != "TARGETS" &&
                    type != "TIMESTAMP" && type != "MULTIPLE")
                    plain_text_only = false;
            }
            SDL_free(mime_types);
            if (plain_text_only) {
                char* saved = SDL_GetClipboardText();
                require(saved != nullptr, "Read clipboard before reversible fixture");
                struct ClipboardRestore {
                    std::string saved;
                    bool active{true};
                    ~ClipboardRestore() {
                        if (active)
                            SDL_SetClipboardText(saved.c_str());
                    }
                } restore{saved};
                SDL_free(saved);
                const std::string fixture = "Faset clipboard Café 世界";
                renderer.set_clipboard(fixture);
                require(renderer.clipboard() == fixture, "Native Unicode clipboard roundtrip");
                require(SDL_SetClipboardText(restore.saved.c_str()), "Restore saved clipboard");
                restore.active = false;
                std::cout << "clipboard=unicode_roundtrip_passed\n";
            } else {
                std::cout << "clipboard=skipped_to_preserve_non_text_payload\n";
            }
        }
        renderer.set_text_input(true);
        require(SDL_TextInputActive(window), "Native SDL text input is active");
        renderer.set_text_input_area(31.5f, 22.5f, 160.5f, 24.5f);
        int logical_width{}, logical_height{}, pixel_width{}, pixel_height{}, cursor{};
        SDL_GetWindowSize(window, &logical_width, &logical_height);
        SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
        require(logical_width > 0 && logical_height > 0 && pixel_width > 0 && pixel_height > 0,
                "Text input area has usable native window dimensions");
        SDL_Rect native_area{};
        require(SDL_GetTextInputArea(window, &native_area, &cursor), "Read native text input area");
        const float sx = float(logical_width) / pixel_width,
                    sy = float(logical_height) / pixel_height;
        require(native_area.x == int(31.5f * sx) && native_area.y == int(22.5f * sy) &&
                    native_area.w == std::max(1, int(160.5f * sx)) &&
                    native_area.h == std::max(1, int(24.5f * sy)) && cursor == 0,
                "Drawable-pixel input area must reach SDL in logical window units");
        renderer.set_text_input(false);
        require(!SDL_TextInputActive(window), "Native SDL text input stops");
        std::cout << "text_input_area=coordinate_conversion_passed\n";
        require(SDL_MinimizeWindow(window), "Request minimizing the owned fixture window");
        if (!await([&] { return SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED; }, 2000)) {
            std::cout << "SKIP: window system declined the minimize request\n";
            return 77;
        }
        const auto before = renderer.stats().frame;
        for (int i = 0; i < 12; ++i) {
            draw();
            SDL_Delay(10);
        }
        require(renderer.stats().frame == before + 12, "Minimized capture must keep progressing");
        std::cout << "stage=minimized fresh_frames=12 minimize_events=" << events.minimized
                  << " focus_lost_events=" << events.focus_lost << '\n'
                  << std::flush;
        if (argc > 1) {
            std::filesystem::create_directories(argv[1]);
            renderer.capture(std::filesystem::path(argv[1]) / "minimized.ppm");
        }
        const auto restored_before = events.restored;
        require(SDL_RestoreWindow(window), "Request restoring the owned fixture window");
        bool restored = await(
            [&] {
                return !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) &&
                       events.restored > restored_before;
            },
            1000);
        bool activation_required{};
        if (!restored) {
            activation_required = true;
            require(SDL_RaiseWindow(window), "Request activation of the owned fixture window");
            restored = await(
                [&] {
                    return !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) &&
                           events.restored > restored_before;
                },
                2000);
        }
        if (!restored) {
            std::cout << "SKIP: resize and 12 minimized captures passed; compositor did not "
                         "confirm restore/activation. Programmatic restoration is not supported "
                         "by every Wayland compositor.\n";
            return 77;
        }
        renderer.resize(400, 320);
        require(await(
                    [&] {
                        int w{}, h{}, logical_width{}, logical_height{};
                        SDL_GetWindowSize(window, &logical_width, &logical_height);
                        SDL_GetWindowSizeInPixels(window, &w, &h);
                        return logical_width == 400 && logical_height == 320 &&
                               renderer.width() == static_cast<unsigned>(w) &&
                               renderer.height() == static_cast<unsigned>(h);
                    },
                    2000),
                "Restore followed by resize must rebuild the target");
        for (int i = 0; i < 12; ++i)
            draw();
        if (argc > 1)
            renderer.capture(std::filesystem::path(argv[1]) / "restored.ppm");
        std::cout << "stage=restored restored_events=" << events.restored
                  << " activation_required=" << activation_required
                  << " frames=" << renderer.stats().frame
                  << " validation_enabled=" << renderer.stats().validation_enabled
                  << " validation_errors=" << renderer.stats().validation_errors << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
