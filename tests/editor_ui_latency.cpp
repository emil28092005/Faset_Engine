#include <chrono>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using namespace faset;
    using Clock = std::chrono::steady_clock;
    if (argc != 2) {
        std::cerr << "Usage: faset_editor_ui_latency OUTPUT.json\n";
        return 2;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      path_from_utf8("faset-ui-latency-" + new_id());
    try {
        std::filesystem::create_directories(root);
        atomic_write_json(root / "project.faset.json",
                          {{"format", "faset.project"},
                           {"version", 1},
                           {"name", "UI latency fixture"},
                           {"dimension", 3}});
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 800, "Editor input latency", true, true});
        editor::EditorUI ui(session, renderer,
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                            path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        ui.frame({});
        Json samples = Json::array();
        constexpr int warmup = 10, measured = 100;
        for (int index = 0; index < warmup + measured; ++index) {
            render::Event event;
            event.type = render::Event::Type::KeyDown;
            event.key = index % 2 == 0 ? "E" : "W";
            const auto started = Clock::now();
            ui.frame({event});
            renderer.render(ui.snapshot());
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started)
                                     .count();
            const auto target = index % 2 == 0 ? "gizmo-Rotate" : "gizmo-Move";
            if (!ui.widgets().find(target)->selected ||
                renderer.stats().validation_errors != 0 || !std::isfinite(elapsed) ||
                elapsed <= 0)
                throw std::runtime_error("Synthetic input was not visible in a valid frame");
            if (index >= warmup)
                samples.push_back({{"iteration", index - warmup + 1},
                                   {"milliseconds", elapsed},
                                   {"gizmo", event.key == "E" ? "Rotate" : "Move"}});
        }
        atomic_write_json(path_from_utf8(argv[1]),
                          {{"format", "faset.editor-ui-latency"},
                           {"version", 1},
                           {"warmup_frames", warmup},
                           {"measured_frames", measured},
                           {"presentation_mode", "offscreen"},
                           {"method", "KeyDown gizmo toggle through EditorUI::frame and "
                                      "offscreen Vulkan render completion; no window present"},
                           {"device", renderer.stats().device},
                           {"validation_enabled", renderer.stats().validation_enabled},
                           {"validation_errors", renderer.stats().validation_errors},
                           {"samples", samples}});
        std::filesystem::remove_all(root);
        std::cout << "Measured " << measured << " synthetic Editor input frames\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
