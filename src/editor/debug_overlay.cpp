#include <algorithm>
#include <cmath>
#include <faset/editor/debug_overlay.hpp>
#include <imgui.h>
#include <stdexcept>
#include <string_view>

namespace faset::editor {
namespace {
struct CurrentContext {
    ImGuiContext* previous{ImGui::GetCurrentContext()};
    explicit CurrentContext(ImGuiContext* context) {
        ImGui::SetCurrentContext(context);
    }
    ~CurrentContext() {
        ImGui::SetCurrentContext(previous);
    }
};
ImGuiKey key(std::string_view name) {
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + name[0] - 'A');
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + name[0] - '0');
    const std::pair<std::string_view, ImGuiKey> names[] = {
        {"Tab", ImGuiKey_Tab},           {"Left", ImGuiKey_LeftArrow},
        {"Right", ImGuiKey_RightArrow},  {"Up", ImGuiKey_UpArrow},
        {"Down", ImGuiKey_DownArrow},    {"PageUp", ImGuiKey_PageUp},
        {"PageDown", ImGuiKey_PageDown}, {"Home", ImGuiKey_Home},
        {"End", ImGuiKey_End},           {"Insert", ImGuiKey_Insert},
        {"Delete", ImGuiKey_Delete},     {"Backspace", ImGuiKey_Backspace},
        {"Space", ImGuiKey_Space},       {"Return", ImGuiKey_Enter},
        {"Escape", ImGuiKey_Escape}};
    for (const auto& [label, value] : names)
        if (label == name)
            return value;
    return ImGuiKey_None;
}
} // namespace
struct DebugOverlay::Impl {
    ImGuiContext* context{};
    bool visible{}, freeze{};
    std::uint32_t overlay_buttons{}, editor_buttons{};
    std::array<float, 2> pointer{-1, -1};
    float scale{};
    std::array<float, 4> window_rect{};
    render::FrameStats displayed;
    std::shared_ptr<render::Texture> atlas;
    Impl() {
        IMGUI_CHECKVERSION();
        auto* previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.BackendPlatformName = "faset_events";
        io.BackendRendererName = "faset_ui_triangles";
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        atlas = std::make_shared<render::Texture>();
        atlas->width = width;
        atlas->height = height;
        atlas->rgba.assign(pixels, pixels + std::size_t(width) * height * 4);
        io.Fonts->SetTexID(ImTextureID{1});
        ImGui::SetCurrentContext(previous);
    }
    ~Impl() {
        ImGui::DestroyContext(context);
    }
};
DebugOverlay::DebugOverlay() : impl_(std::make_unique<Impl>()) {}
DebugOverlay::~DebugOverlay() = default;
bool DebugOverlay::visible() const {
    return impl_->visible;
}
void DebugOverlay::set_visible(bool value) {
    impl_->visible = value;
}
std::vector<render::Event> DebugOverlay::process_events(std::span<const render::Event> events) {
    CurrentContext current(impl_->context);
    auto& io = ImGui::GetIO();
    std::vector<render::Event> forwarded;
    const auto pointer_in_overlay = [&] {
        return impl_->visible && impl_->pointer[0] >= impl_->window_rect[0] &&
               impl_->pointer[1] >= impl_->window_rect[1] &&
               impl_->pointer[0] < impl_->window_rect[0] + impl_->window_rect[2] &&
               impl_->pointer[1] < impl_->window_rect[1] + impl_->window_rect[3];
    };
    const auto capture_pointer = [&] {
        if (impl_->overlay_buttons)
            return true;
        if (impl_->editor_buttons)
            return false;
        return pointer_in_overlay();
    };
    for (const auto& event : events) {
        using Type = render::Event::Type;
        if ((event.type == Type::KeyDown || event.type == Type::KeyUp) && event.key == "F12") {
            if (event.type == Type::KeyDown && !event.repeat)
                impl_->visible = !impl_->visible;
            continue;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, event.control);
        io.AddKeyEvent(ImGuiMod_Shift, event.shift);
        io.AddKeyEvent(ImGuiMod_Alt, event.alt);
        bool captured{};
        bool pointer_event{};
        switch (event.type) {
        case Type::MouseMove:
            io.AddMousePosEvent(event.x, event.y);
            impl_->pointer = {event.x, event.y};
            pointer_event = true;
            captured = capture_pointer();
            break;
        case Type::MouseDown:
        case Type::MouseUp:
            io.AddMousePosEvent(event.x, event.y);
            impl_->pointer = {event.x, event.y};
            pointer_event = true;
            captured = capture_pointer();
            if (event.button >= 1 && event.button <= 5) {
                const int buttons[] = {0, 2, 1, 3, 4};
                io.AddMouseButtonEvent(buttons[event.button - 1], event.type == Type::MouseDown);
                const auto mask = std::uint32_t{1} << (event.button - 1);
                if (event.type == Type::MouseDown) {
                    if (captured)
                        impl_->overlay_buttons |= mask;
                    else
                        impl_->editor_buttons |= mask;
                } else {
                    if (impl_->overlay_buttons & mask)
                        captured = true;
                    else if (impl_->editor_buttons & mask)
                        captured = false;
                    impl_->overlay_buttons &= ~mask;
                    impl_->editor_buttons &= ~mask;
                }
            }
            break;
        case Type::Wheel:
            io.AddMouseWheelEvent(event.x, event.y);
            pointer_event = true;
            captured = capture_pointer();
            break;
        case Type::KeyDown:
        case Type::KeyUp:
            if (const auto mapped = key(event.key); mapped != ImGuiKey_None)
                io.AddKeyEvent(mapped, event.type == Type::KeyDown);
            captured = io.WantCaptureKeyboard;
            break;
        case Type::TextInput:
            io.AddInputCharactersUTF8(event.text.c_str());
            captured = io.WantCaptureKeyboard;
            break;
        case Type::FocusGained:
        case Type::FocusLost:
            io.AddFocusEvent(event.type == Type::FocusGained);
            if (event.type == Type::FocusLost)
                impl_->overlay_buttons = impl_->editor_buttons = 0;
            break;
        default:
            break;
        }
        if (!captured || (!pointer_event && !impl_->visible))
            forwarded.push_back(event);
    }
    return forwarded;
}
void DebugOverlay::append(render::Snapshot& output, render::Renderer& renderer, float delta) {
    CurrentContext current(impl_->context);
    auto& state = *impl_;
    auto& io = ImGui::GetIO();
    io.DisplaySize = {float(renderer.width()), float(renderer.height())};
    io.DisplayFramebufferScale = {1, 1}; // Faset events and geometry already use drawable pixels.
    io.DeltaTime = std::isfinite(delta) && delta > 0 ? std::clamp(delta, .0001f, .1f) : 1.f / 60.f;
    const float scale = renderer.display_scale();
    if (state.scale != scale) {
        auto& style = ImGui::GetStyle();
        style = ImGuiStyle{};
        ImGui::StyleColorsDark();
        // Developer tooling follows the editor's neutral, low-contrast dark palette.
        style.Colors[ImGuiCol_WindowBg] = {0.11f, 0.11f, 0.115f, 0.98f};
        style.Colors[ImGuiCol_TitleBg] = {0.085f, 0.085f, 0.087f, 1.f};
        style.Colors[ImGuiCol_TitleBgActive] = {0.11f, 0.11f, 0.115f, 1.f};
        style.Colors[ImGuiCol_FrameBg] = {0.15f, 0.15f, 0.16f, 1.f};
        style.Colors[ImGuiCol_FrameBgHovered] = {0.19f, 0.19f, 0.20f, 1.f};
        style.Colors[ImGuiCol_Button] = {0.15f, 0.15f, 0.16f, 1.f};
        style.Colors[ImGuiCol_ButtonHovered] = {0.21f, 0.21f, 0.22f, 1.f};
        style.Colors[ImGuiCol_ButtonActive] = {0.24f, 0.23f, 0.28f, 1.f};
        style.Colors[ImGuiCol_Border] = {0.23f, 0.23f, 0.24f, 1.f};
        style.Colors[ImGuiCol_Separator] = style.Colors[ImGuiCol_Border];
        style.Colors[ImGuiCol_Text] = {0.88f, 0.88f, 0.89f, 1.f};
        style.Colors[ImGuiCol_TextDisabled] = {0.60f, 0.60f, 0.62f, 1.f};
        style.Colors[ImGuiCol_CheckMark] = {0.66f, 0.63f, 0.76f, 1.f};
        style.WindowRounding = 5.f;
        style.FrameRounding = 4.f;
        style.WindowBorderSize = 1.f;
        style.ScaleAllSizes(scale);
        style.FontScaleMain = scale;
        if (state.window_rect[2] == 0)
            state.window_rect = {16 * scale, 44 * scale, 420 * scale, 455 * scale};
        state.scale = scale;
    }
    // Complete an ImGui frame while hidden too, so queued input cannot accumulate.
    ImGui::NewFrame();
    if (state.visible) {
        ImGui::SetNextWindowPos({16 * scale, 44 * scale}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({420 * scale,
                                  std::min(455 * scale, std::max(220.f, io.DisplaySize.y - 60.f * scale))},
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Faset diagnostics (F12)", &state.visible,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
            const auto position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            state.window_rect = {position.x, position.y, size.x, size.y};
            if (!state.freeze)
                state.displayed = renderer.stats();
            const auto& stats = state.displayed;
            ImGui::TextUnformatted("Visibility");
            const auto current_mode = renderer.visibility_mode();
            const struct {
                const char* label;
                render::VisibilityMode value;
            } modes[] = {{"Direct", render::VisibilityMode::Direct},
                         {"GPU frustum", render::VisibilityMode::GpuFrustum},
                         {"GPU occlusion", render::VisibilityMode::GpuOcclusion}};
            const float button_width =
                (ImGui::GetContentRegionAvail().x - 2.f * ImGui::GetStyle().ItemSpacing.x) / 3.f;
            for (int i = 0; i < 3; ++i) {
                if (i)
                    ImGui::SameLine();
                const bool selected = current_mode == modes[i].value;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.35f, .33f, .43f, 1.f});
                if (ImGui::Button(modes[i].label, {button_width, 0}))
                    renderer.set_visibility_mode(modes[i].value);
                if (selected)
                    ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("Renderer mode; no scene or export changes");
            ImGui::Separator();
            ImGui::TextUnformatted("Previous completed frame");
            ImGui::SameLine();
            ImGui::Checkbox("Freeze counters", &state.freeze);
            ImGui::TextWrapped("%s", stats.device.c_str());
            ImGui::Text("Frame: %llu", static_cast<unsigned long long>(stats.frame));
            ImGui::Text("Render call (wall): %.3f ms", stats.cpu_ms);
            if (stats.gpu_ms > 0)
                ImGui::Text("GPU: %.3f ms", stats.gpu_ms);
            else
                ImGui::TextUnformatted("GPU timestamps: unavailable");
            ImGui::Text("Readback (wall): %.3f ms", stats.readback_cpu_ms);
            ImGui::Text("Draws: %u   Packed vertices: %u", stats.draw_calls, stats.vertices);
            ImGui::Text("Culled meshes: %u   Textures: %u", stats.culled_meshes,
                        stats.texture_count);
            ImGui::Separator();
            ImGui::TextUnformatted("GPU visibility");
            ImGui::Text("Path: %s", stats.gpu_visibility_active ? "active" : "inactive");
            ImGui::Text("Indirect bins: %u   Visible: %u", stats.gpu_bins,
                        stats.gpu_visible_instances);
            ImGui::Text("Frustum rejected: %u", stats.gpu_frustum_rejected);
            ImGui::Text("HZB history: %s", stats.hzb_valid ? "valid" : "unavailable / invalid");
            ImGui::Text("Deferred: %u   Post visible: %u", stats.gpu_occlusion_deferred,
                        stats.gpu_post_visible);
            ImGui::Text("Prepared LOD: %u / %u / %u / %u+", stats.lod_counts[0],
                        stats.lod_counts[1], stats.lod_counts[2], stats.lod_counts[3]);
            ImGui::Separator();
            ImGui::Text("Vulkan allocations: %.2f MiB",
                        double(stats.gpu_allocated_bytes) / 1048576.0);
            ImGui::Text("Validation: %s   Errors: %u",
                        stats.validation_enabled ? "on" : "unavailable/off",
                        stats.validation_errors);
            ImGui::Text("GPU pass labels: %s (%u)", stats.gpu_labels_enabled ? "on" : "unavailable",
                        stats.gpu_label_count);
        }
        ImGui::End();
    }
    ImGui::Render();
    const auto* data = ImGui::GetDrawData();
    if (!data || !data->Valid)
        return;
    for (const auto* list : data->CmdLists) {
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback) {
                if (command.UserCallback != ImDrawCallback_ResetRenderState)
                    command.UserCallback(list, &command);
                continue;
            }
            if (command.GetTexID() != ImTextureID{1})
                throw std::runtime_error("Unsupported texture in Faset diagnostic overlay");
            const float x = command.ClipRect.x - data->DisplayPos.x;
            const float y = command.ClipRect.y - data->DisplayPos.y;
            const float width = command.ClipRect.z - command.ClipRect.x;
            const float height = command.ClipRect.w - command.ClipRect.y;
            if (width <= 0 || height <= 0)
                continue;
            render::UiTriangles batch;
            batch.texture = state.atlas;
            batch.clip_rect = {x, y, width, height};
            batch.vertices.reserve(command.ElemCount);
            for (unsigned i = 0; i < command.ElemCount; ++i) {
                const auto index = list->IdxBuffer[command.IdxOffset + i] + command.VtxOffset;
                const auto& vertex = list->VtxBuffer[index];
                const auto channel = [&](unsigned shift) {
                    return float((vertex.col >> shift) & 0xffu) / 255.f;
                };
                batch.vertices.push_back(
                    {{vertex.pos.x - data->DisplayPos.x, vertex.pos.y - data->DisplayPos.y, 0},
                     {},
                     {channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT),
                      channel(IM_COL32_B_SHIFT), channel(IM_COL32_A_SHIFT)},
                     {vertex.uv.x, vertex.uv.y}});
            }
            output.ui_triangles.push_back(std::move(batch));
        }
    }
}
} // namespace faset::editor
