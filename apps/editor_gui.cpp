#include <algorithm>
#include <chrono>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <faset/editor/mcp.hpp>
#include <thread>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace faset::editor {
namespace {
std::string base64(const std::vector<unsigned char>& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = bytes[i];
        const auto b = i + 1 < bytes.size() ? bytes[i + 1] : 0,
                   c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return out;
}
std::vector<unsigned char> png(render::Renderer& renderer, std::array<float, 4> rectangle) {
    const auto width = renderer.width(), height = renderer.height();
    const auto pixels = renderer.pixels();
    int x = std::clamp(static_cast<int>(rectangle[0]), 0, static_cast<int>(width) - 1),
        y = std::clamp(static_cast<int>(rectangle[1]), 0, static_cast<int>(height) - 1);
    const int w = std::clamp(static_cast<int>(rectangle[2]), 1, static_cast<int>(width) - x),
              h = std::clamp(static_cast<int>(rectangle[3]), 1, static_cast<int>(height) - y);
    std::vector<unsigned char> cropped(static_cast<std::size_t>(w) * h * 4), encoded;
    for (int row = 0; row < h; ++row)
        std::copy_n(pixels.begin() + (static_cast<std::size_t>(row + y) * width + x) * 4,
                    static_cast<std::size_t>(w) * 4,
                    cropped.begin() + static_cast<std::size_t>(row) * w * 4);
    const auto writer = [](void* context, void* data, int count) {
        auto& out = *static_cast<std::vector<unsigned char>*>(context);
        auto* begin = static_cast<unsigned char*>(data);
        out.insert(out.end(), begin, begin + count);
    };
    require(stbi_write_png_to_func(writer, &encoded, w, h, 4, cropped.data(), w * 4) != 0,
            "capture.encode", "Cannot encode editor screenshot");
    return encoded;
}
} // namespace
int run_editor_ui(Session& session, bool enable_mcp, std::uint64_t max_frames,
                  const std::filesystem::path& capture) {
    render::Renderer renderer({1440, 900,
                               "Faset — " + session.project().value("name", std::string("Project")),
                               false, true});
    const auto font = session.config().engine_root / "assets/fonts/NotoSans.ttf";
    const auto theme = session.config().engine_root / "assets/ui/dark.json";
    EditorUI ui(session, renderer, font, theme);
    ui.set_project_switch_enabled(!enable_mcp);
    McpServer server(session.commands());
    StdioTransport transport;
    session.commands().add(
        "faset_editor_capture",
        "Capture the Editor or its authoring viewport as a PNG image. Requires the graphical "
        "Editor and Vulkan; never captures a Player process.",
        Commands::object_schema(
            {{"path", {{"type", "string"}}}, {"viewport_only", {{"type", "boolean"}}}}),
        [&](const Json& arguments) {
            session.poll();
            ui.frame({});
            renderer.render(ui.snapshot());
            auto region =
                arguments.value("viewport_only", true)
                    ? ui.snapshot().scene_rect
                    : std::array<float, 4>{0, 0, float(renderer.width()), float(renderer.height())};
            if (region[2] <= 0 || region[3] <= 0)
                region = {0, 0, float(renderer.width()), float(renderer.height())};
            auto encoded = png(renderer, region);
            const auto relative =
                arguments.value("path", std::string(".faset/screenshots/editor.png"));
            require(path_from_utf8(relative).extension() == ".png", "capture.path",
                    "Editor screenshots must use a .png path");
            atomic_write(
                project_path(session.config().project_root, path_from_utf8(relative)),
                std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
            return Json{{"path", relative},
                        {"mimeType", "image/png"},
                        {"width", static_cast<int>(region[2])},
                        {"height", static_cast<int>(region[3])},
                        {"image_base64", base64(encoded)}};
        },
        true);
    std::uint64_t frame = 0;
    while (!renderer.should_close() && (max_frames == 0 || frame < max_frames)) {
        if (enable_mcp)
            for (const auto& line : transport.poll()) {
                try {
                    const auto reply = server.handle(Json::parse(line));
                    if (reply)
                        transport.send(*reply);
                } catch (const Json::exception&) {
                    transport.send(server.parse_error());
                }
            }
        session.poll();
        ui.frame(renderer.poll_events());
        renderer.render(ui.snapshot());
        if (ui.project_switch_requested())
            return 3; // Application-level request: destroy this Session before opening another.
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!capture.empty())
        renderer.capture(capture);
    return renderer.stats().validation_errors == 0 ? 0 : 2;
}
} // namespace faset::editor
