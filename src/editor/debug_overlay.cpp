#include <algorithm>
#include <bit>
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
const char* visibility_label(render::VisibilityMode mode) {
    switch (mode) {
    case render::VisibilityMode::Direct:
        return "Direct";
    case render::VisibilityMode::GpuFrustum:
        return "GPU frustum";
    case render::VisibilityMode::GpuOcclusion:
        return "GPU occlusion";
    }
    return "Unknown";
}
const char* temporal_label(render::TemporalMode mode) {
    switch (mode) {
    case render::TemporalMode::Off: return "Off";
    case render::TemporalMode::TAA: return "TAA";
    case render::TemporalMode::Upscale: return "Upscale";
    }
    return "Unknown";
}
const char* temporal_fallback_label(render::TemporalFallbackReason reason) {
    using Reason = render::TemporalFallbackReason;
    switch (reason) {
    case Reason::None: return "none";
    case Reason::ComputeUnavailable: return "compute unavailable";
    case Reason::FormatUnavailable: return "format unavailable";
    case Reason::ExtentUnsupported: return "extent unsupported";
    }
    return "unknown";
}
const char* temporal_reset_label(render::TemporalResetReason reason) {
    using Reason = render::TemporalResetReason;
    switch (reason) {
    case Reason::None: return "none";
    case Reason::FirstFrame: return "first frame";
    case Reason::CameraCut: return "camera cut";
    case Reason::CameraDiscontinuity: return "camera discontinuity";
    case Reason::ViewChanged: return "view changed";
    case Reason::ViewportChanged: return "viewport changed";
    case Reason::ProjectionChanged: return "projection changed";
    case Reason::Resize: return "resize";
    case Reason::ModeChanged: return "mode changed";
    case Reason::ScaleChanged: return "scale changed";
    case Reason::ShaderReload: return "shader reload";
    case Reason::Unsupported: return "unsupported";
    }
    return "unknown";
}
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
    bool visible{}, freeze{}, show_hzb{};
    bool hzb_sampled{}, hzb_available{};
    int hzb_mip{3}, hzb_last_mip{-1};
    std::uint64_t hzb_frame{};
    std::string hzb_error;
    std::uint32_t overlay_buttons{}, editor_buttons{};
    std::array<float, 2> pointer{-1, -1};
    float scale{};
    float last_upscale_scale{.67f};
    std::array<float, 4> window_rect{};
    render::FrameStats displayed;
    std::shared_ptr<render::Texture> atlas;
    std::shared_ptr<render::Texture> hzb_preview;
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
    if (!value) {
        impl_->hzb_preview.reset();
        impl_->hzb_available = impl_->hzb_sampled = false;
    }
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
            if (event.type == Type::KeyDown && !event.repeat) {
                impl_->visible = !impl_->visible;
                if (!impl_->visible) {
                    impl_->hzb_preview.reset();
                    impl_->hzb_available = impl_->hzb_sampled = false;
                }
            }
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
            const auto selected_mode = renderer.visibility_mode();
            ImGui::TextDisabled("Renderer mode; no scene or export changes");
            ImGui::TextUnformatted("Temporal");
            const auto current_temporal = renderer.temporal_mode();
            if (current_temporal == render::TemporalMode::Upscale)
                state.last_upscale_scale = renderer.render_scale();
            const struct {
                const char* label;
                render::TemporalMode value;
            } temporal_modes[] = {{"Off", render::TemporalMode::Off},
                                  {"TAA", render::TemporalMode::TAA},
                                  {"Upscale", render::TemporalMode::Upscale}};
            for (int i = 0; i < 3; ++i) {
                if (i)
                    ImGui::SameLine();
                const bool selected = current_temporal == temporal_modes[i].value;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{.35f, .33f, .43f, 1.f});
                if (ImGui::Button(temporal_modes[i].label, {button_width, 0}))
                    renderer.set_temporal_mode(temporal_modes[i].value,
                                               temporal_modes[i].value == render::TemporalMode::Upscale
                                                   ? state.last_upscale_scale : 1.f);
                if (selected)
                    ImGui::PopStyleColor();
            }
            if (renderer.temporal_mode() == render::TemporalMode::Upscale) {
                int percent = static_cast<int>(std::lround(renderer.render_scale() * 100.f));
                if (ImGui::SliderInt("Render scale", &percent, 50, 99, "%d%%")) {
                    state.last_upscale_scale = float(percent) / 100.f;
                    renderer.set_temporal_mode(render::TemporalMode::Upscale,
                                               state.last_upscale_scale);
                }
            }
            ImGui::TextDisabled("Viewport only; exported Player settings are separate");
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
            ImGui::Text("Effective path: %s", visibility_label(stats.effective_visibility_mode));
            if (stats.requested_visibility_mode != stats.effective_visibility_mode)
                ImGui::TextDisabled("Fallback from %s",
                                    visibility_label(stats.requested_visibility_mode));
            ImGui::Text("Indirect bins: %u", stats.gpu_bins);
            if (stats.gpu_visibility_active && stats.visibility_counters_valid) {
                ImGui::Text("Visible: %u   Frustum rejected: %u",
                            stats.gpu_visible_instances, stats.gpu_frustum_rejected);
                ImGui::Text("Deferred: %u   Post visible: %u", stats.gpu_occlusion_deferred,
                            stats.gpu_post_visible);
            } else {
                ImGui::TextDisabled("Visibility counters: %s",
                                    stats.gpu_visibility_active
                                        ? state.freeze ? "frozen before sample" : "awaiting sample"
                                        : "GPU path inactive");
            }
            if (stats.gpu_visibility_active && stats.gpu_ms > 0) {
                ImGui::Text("Pass ms: cull %.2f   raster %.2f", stats.gpu_main_cull_ms,
                            stats.gpu_main_raster_ms);
                if (stats.gpu_hzb_ms > 0 || stats.gpu_post_cull_ms > 0 ||
                    stats.gpu_post_raster_ms > 0)
                    ImGui::Text("HZB %.2f   post cull %.2f   raster %.2f",
                                stats.gpu_hzb_ms, stats.gpu_post_cull_ms,
                                stats.gpu_post_raster_ms);
            }
            ImGui::Text("Previous HZB history: %s", stats.hzb_valid ? "valid" : "invalid");
            if (ImGui::Checkbox("Show HZB", &state.show_hzb)) {
                state.hzb_sampled = false;
                state.hzb_error.clear();
                if (state.show_hzb) {
                    const float expanded = std::min(
                        620.f * scale, std::max(220.f, io.DisplaySize.y - 60.f * scale));
                    if (ImGui::GetWindowSize().y < expanded)
                        ImGui::SetWindowSize({ImGui::GetWindowSize().x, expanded});
                } else {
                    state.hzb_available = false;
                    state.hzb_preview.reset();
                }
            }
            if (state.show_hzb && state.visible &&
                selected_mode == render::VisibilityMode::GpuOcclusion) {
                const auto max_extent = std::max(renderer.width(), renderer.height());
                const int max_mip = static_cast<int>(std::bit_width(std::bit_ceil(max_extent))) - 1;
                state.hzb_mip = std::clamp(state.hzb_mip, 0, max_mip);
                ImGui::SliderInt("Mip", &state.hzb_mip, 0, max_mip);
                const auto frame = renderer.stats().frame;
                if (!state.hzb_sampled || state.hzb_frame != frame ||
                    state.hzb_last_mip != state.hzb_mip) {
                    state.hzb_sampled = true;
                    state.hzb_frame = frame;
                    state.hzb_last_mip = state.hzb_mip;
                    try {
                        if (auto image = renderer.hzb_debug_image(
                                static_cast<std::uint32_t>(state.hzb_mip))) {
                            if (!state.hzb_preview)
                                state.hzb_preview = std::make_shared<render::Texture>();
                            state.hzb_preview->width = image->width;
                            state.hzb_preview->height = image->height;
                            state.hzb_preview->rgba = std::move(image->rgba);
                            ++state.hzb_preview->revision;
                            state.hzb_available = true;
                        } else {
                            state.hzb_available = false;
                            state.hzb_preview.reset();
                        }
                    } catch (const std::exception& error) {
                        state.hzb_available = false;
                        state.hzb_preview.reset();
                        state.hzb_error = error.what();
                        state.show_hzb = false;
                    }
                }
                if (state.hzb_available && state.hzb_preview) {
                    float width = ImGui::GetContentRegionAvail().x;
                    float height = width * float(state.hzb_preview->height) /
                                   float(state.hzb_preview->width);
                    if (height > 180.f * scale) {
                        height = 180.f * scale;
                        width = height * float(state.hzb_preview->width) /
                                float(state.hzb_preview->height);
                    }
                    ImGui::Image(ImTextureID{2}, {width, height});
                    ImGui::TextDisabled("Current HZB | mip %d (%u x %u)", state.hzb_mip,
                                        state.hzb_preview->width, state.hzb_preview->height);
                } else if (state.show_hzb) {
                    ImGui::TextDisabled("Current HZB unavailable for this frame");
                }
            } else if (state.show_hzb) {
                state.hzb_available = false;
                state.hzb_preview.reset();
                state.hzb_sampled = false;
                ImGui::TextDisabled("Switch to GPU occlusion to view the HZB");
            }
            if (!state.hzb_error.empty())
                ImGui::TextWrapped("HZB preview error: %s", state.hzb_error.c_str());
            ImGui::Text("Prepared LOD: %u / %u / %u / %u+", stats.lod_counts[0],
                        stats.lod_counts[1], stats.lod_counts[2], stats.lod_counts[3]);
            ImGui::Separator();
            ImGui::TextUnformatted("Temporal reconstruction");
            ImGui::Text("Requested: %s   Effective: %s",
                        temporal_label(stats.requested_temporal_mode),
                        temporal_label(stats.effective_temporal_mode));
            if (stats.requested_temporal_mode != stats.effective_temporal_mode)
                ImGui::TextDisabled("Fallback: %s",
                                    temporal_fallback_label(stats.temporal_fallback_reason));
            if (stats.effective_temporal_mode != render::TemporalMode::Off) {
                ImGui::Text("Internal: %u x %u   Output: %u x %u",
                            stats.temporal_internal_width, stats.temporal_internal_height,
                            renderer.width(), renderer.height());
                ImGui::Text("History: %s   Reset: %s",
                            stats.temporal_history_valid ? "valid" : "invalid",
                            temporal_reset_label(stats.temporal_reset_reason));
                if (stats.gpu_ms > 0)
                    ImGui::Text("GPU: resolve %.2f  composite %.2f  UI %.2f ms",
                                stats.gpu_temporal_resolve_ms,
                                stats.gpu_temporal_composite_ms, stats.gpu_ui_ms);
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Lighting and shadows");
            ImGui::Text("Lighting path: %s", stats.effective_lighting_path.c_str());
            ImGui::Text("Light tiles: %u; GPU build %.2f ms",
                        stats.light_tile_count, stats.gpu_light_tiles_ms);
            if (stats.light_tile_counts_valid)
                ImGui::Text("Tile entries: %u; overflow tiles: %u",
                            stats.light_tile_candidate_count,
                            stats.light_tile_overflow_count);
            else if (stats.light_tile_count)
                ImGui::TextDisabled("Tile entry counts unavailable until diagnostics readback");
            ImGui::Text("Local lights: %u submitted, %u omitted",
                        stats.submitted_local_lights, stats.omitted_local_lights);
            ImGui::Text("Sun cascades: %u / %u effective",
                        stats.requested_sun_cascades, stats.effective_sun_cascades);
            ImGui::Text("Local faces: %u requested, %u rasterized (%u tiles)",
                        stats.requested_local_shadow_faces, stats.local_shadow_faces,
                        stats.local_shadow_tiles);
            ImGui::Text("Dropped faces: %u (point %u, atlas %u, draw budget %u, unavailable %u)",
                        stats.dropped_shadow_faces, stats.dropped_point_shadow_faces,
                        stats.shadow_atlas_full_drops, stats.shadow_caster_budget_drops,
                        stats.shadow_unavailable_drops);
            ImGui::Text("Shadow caster draws: %u / 4096", stats.shadow_caster_draws);
            ImGui::Text("Atlas memory: sun %.1f MiB, local %.1f MiB",
                        double(stats.sun_shadow_atlas_bytes) / 1048576.0,
                        double(stats.local_shadow_atlas_bytes) / 1048576.0);
            if (stats.gpu_ms > 0)
                ImGui::Text("Shadow GPU: sun %.2f ms, local %.2f ms",
                            stats.gpu_sun_shadow_ms, stats.gpu_local_shadow_ms);
            ImGui::Separator();
            ImGui::Text("Vulkan allocations: %.2f MiB",
                        double(stats.gpu_allocated_bytes) / 1048576.0);
            ImGui::Text("Validation: %s   Errors: %u",
                        stats.validation_enabled ? "on" : "unavailable/off",
                        stats.validation_errors);
            ImGui::Text("GPU pass labels: %s (%u)", stats.gpu_labels_enabled ? "on" : "unavailable",
                        stats.gpu_label_count);
            const auto current_position = ImGui::GetWindowPos();
            const auto current_size = ImGui::GetWindowSize();
            state.window_rect = {current_position.x, current_position.y,
                                 current_size.x, current_size.y};
        }
        ImGui::End();
    }
    if (!state.visible) {
        state.hzb_available = false;
        state.hzb_preview.reset();
        state.hzb_sampled = false;
    }
    // GPU counters are a diagnostics readback, never a normal renderer dependency.
    renderer.set_visibility_diagnostics(state.visible);
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
            const auto texture = command.GetTexID() == ImTextureID{1}
                                     ? state.atlas
                                     : command.GetTexID() == ImTextureID{2} ? state.hzb_preview
                                                                             : nullptr;
            if (!texture)
                throw std::runtime_error("Unsupported texture in Faset diagnostic overlay");
            const float x = command.ClipRect.x - data->DisplayPos.x;
            const float y = command.ClipRect.y - data->DisplayPos.y;
            const float width = command.ClipRect.z - command.ClipRect.x;
            const float height = command.ClipRect.w - command.ClipRect.y;
            if (width <= 0 || height <= 0)
                continue;
            render::UiTriangles batch;
            batch.texture = texture;
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
