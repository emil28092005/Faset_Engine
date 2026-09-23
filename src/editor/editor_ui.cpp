#include <algorithm>
#include <chrono>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <faset/player/SceneView.hpp>
#include <limits>
#include <map>
#include <set>

namespace faset::editor {
namespace {
using render::Mat4;
using render::Vec3;
using ui::Kind;
using ui::Widget;
float dot(Vec3 a, Vec3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 add(Vec3 a, Vec3 b) {
    for (int i = 0; i < 3; ++i)
        a[i] += b[i];
    return a;
}
Vec3 sub(Vec3 a, Vec3 b) {
    for (int i = 0; i < 3; ++i)
        a[i] -= b[i];
    return a;
}
Vec3 mul(Vec3 a, float s) {
    for (auto& x : a)
        x *= s;
    return a;
}
Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 normalize(Vec3 a) {
    return mul(a, 1 / std::max(.000001f, std::sqrt(dot(a, a))));
}
Vec3 point(const Mat4& m, Vec3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
Vec3 vector(const Mat4& m, Vec3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2], m[1] * p[0] + m[5] * p[1] + m[9] * p[2],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2]};
}
Vec3 euler_xyz(const Mat4& rotation) {
    // The authoring transform is Rz * Ry * Rx. Reconstruct that convention
    // after composing an intrinsic rotation instead of adding Euler channels.
    const auto cy = std::hypot(rotation[0], rotation[1]);
    if (cy > .00001f)
        return {std::atan2(rotation[6], rotation[10]), std::atan2(-rotation[2], cy),
                std::atan2(rotation[1], rotation[0])};
    return {std::atan2(-rotation[9], rotation[5]), std::atan2(-rotation[2], cy), 0};
}
bool inverse(const Mat4& m, Mat4& out) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            a[r][c] = m[c * 4 + r];
        a[r][r + 4] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
                pivot = r;
        if (std::abs(a[pivot][c]) < 1e-10)
            return false;
        for (int j = 0; j < 8; ++j)
            std::swap(a[c][j], a[pivot][j]);
        const auto d = a[c][c];
        for (auto& v : a[c])
            v /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const auto f = a[r][c];
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = float(a[r][c + 4]);
    return true;
}
Vec3 homogeneous(const Mat4& m, float x, float y, float z) {
    const auto w = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (std::abs(w) < 1e-8f)
        return {};
    return {(m[0] * x + m[4] * y + m[8] * z + m[12]) / w,
            (m[1] * x + m[5] * y + m[9] * z + m[13]) / w,
            (m[2] * x + m[6] * y + m[10] * z + m[14]) / w};
}
const Json* entity(const Json& scene, const std::string& id) {
    for (const auto& e : scene.at("entities"))
        if (e.at("id") == id)
            return &e;
    return nullptr;
}
const Json* component(const Json& e, const std::string& type) {
    for (const auto& c : e.at("components"))
        if (c.at("type") == type)
            return &c;
    return nullptr;
}
Vec3 vec(const Json& j, Vec3 fallback = {}) {
    if (!j.is_array() || j.size() < 3)
        return fallback;
    for (int i = 0; i < 3; ++i)
        fallback[i] = j[i].get<float>();
    return fallback;
}
Mat4 world(const Json& scene, const Json& e, int depth = 0) {
    if (depth > 256)
        return render::identity;
    Mat4 local = render::identity;
    if (auto* t = component(e, "faset.transform"); t && t->value("version", 1) == 1) {
        const auto& f = t->at("fields");
        local =
            render::transform(vec(f.value("position", Json{})), vec(f.value("rotation", Json{})),
                              vec(f.value("scale", Json{}), {1, 1, 1}));
    }
    if (e.contains("parent") && e["parent"].is_string())
        if (auto* p = entity(scene, e["parent"].get<std::string>()))
            return render::multiply(world(scene, *p, depth + 1), local);
    return local;
}
bool ray_triangle(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c, float& distance) {
    const auto e1 = sub(b, a), e2 = sub(c, a), h = cross(direction, e2);
    const auto d = dot(e1, h);
    if (std::abs(d) < 1e-7f)
        return false;
    const auto s = sub(origin, a);
    const auto u = dot(s, h) / d;
    if (u < 0 || u > 1)
        return false;
    const auto q = cross(s, e1);
    const auto v = dot(direction, q) / d;
    if (v < 0 || u + v > 1)
        return false;
    const auto t = dot(e2, q) / d;
    if (t < 0 || t >= distance)
        return false;
    distance = t;
    return true;
}
void trim_children(Widget& w, const std::set<std::string>& keep) {
    std::erase_if(w.children, [&](const auto& child) { return !keep.contains(child->id); });
}
void label(Widget& p, const std::string& id, const std::string& value, float width = -1) {
    auto& w = p.add(Kind::Label, id, value);
    w.layout.width = width;
}
} // namespace
struct EditorUI::Impl {
    Session& session;
    render::Renderer& renderer;
    ui::Context ui;
    ui::DockLayout dock;
    player::SceneView view;
    render::Snapshot rendered;
    std::string picks_key, resolved_stamp, last_document;
    std::set<std::string> seen_view_diagnostics;
    Json instance_selection = Json::array(), template_conflicts = Json::array();
    std::string document, selected, source_file, asset_filter,
        active_bottom = "assets", menu, command_name = "faset_documents", status = "Ready",
        gizmo_mode = "Move", expanded_job_log;
    Json current, resolved, files = Json::array(), assets = Json::array(), schemas = Json::object(),
                            recovery = Json::array();
    std::uint64_t shown_revision = std::numeric_limits<std::uint64_t>::max();
    std::map<std::string, std::uint64_t> edit_revisions;
    std::map<std::string, Json> preview_fields;
    std::map<std::string, std::string> import_retries;
    std::filesystem::path layout_path;
    std::filesystem::path theme_source_path, layout_source_path;
    std::string attempted_theme, attempted_layout, applied_theme, applied_layout,
        presentation_error;
    std::chrono::steady_clock::time_point last_presentation_poll{};
    std::chrono::steady_clock::time_point last_schema_poll{};
    Json schema_state;
    bool simulation_open = false;
    bool project_settings_open = false;
    Json project_settings_state;
    int project_settings_dimension = 3;
    std::string project_settings_error;
    bool project_switch_enabled = true, project_switch_requested = false,
         project_switch_warning = false;
    bool palette = false, component_menu = false, assets_dirty = true, paused = false;
    float yaw = .65f, pitch = .42f, distance = 12, ortho = 12, mouse_x = 0, mouse_y = 0, last_x = 0,
          last_y = 0;
    Vec3 target{};
    int camera_drag = 0, gizmo_axis = -1;
    float gizmo_down_x = 0, gizmo_down_y = 0;
    Json gizmo_original;
    std::uint64_t gizmo_revision = 0;
    std::string gizmo_component;
    ui::Rect viewport;
    float ui_scale = 1;
    unsigned layout_width = 0, layout_height = 0;
    float logical_width() const {
        return float(renderer.width()) / ui_scale;
    }
    float logical_height() const {
        return float(renderer.height()) / ui_scale;
    }
    Vec3 gizmo_origin{};
    render::Vec2 gizmo_drag_start{}, gizmo_drag_end{};
    float gizmo_world_length = 1;
    std::array<render::Vec2, 4> gizmo_screen{};
    bool gizmo_valid = false;
    struct Pick {
        std::string id;
        render::DrawItem item;
    };
    std::vector<Pick> picks;
    std::chrono::steady_clock::time_point last_assets{};
    Impl(Session& s, render::Renderer& r, const std::filesystem::path& font,
         const std::filesystem::path& styles)
        : session(s), renderer(r), ui(font), view(s.config().project_root / ".faset/cache") {
        theme_source_path = styles;
        layout_source_path = styles.parent_path() / "editor-layout.json";
        attempted_theme = applied_theme = read_text(theme_source_path);
        attempted_layout = applied_layout = read_text(layout_source_path);
        ui.set_theme(ui::Theme::from_json(Json::parse(applied_theme)));
        ui.apply_layout(Json::parse(applied_layout));
        last_presentation_poll = std::chrono::steady_clock::now();
        layout_path = session.config().project_root / ".faset/editor-layout.json";
        dock.move("assets", "bottom", 0);
        dock.move("console", "bottom", 1);
        dock.move("jobs", "bottom", 2);
        dock.move("conflicts", "bottom", 3);
        if (std::filesystem::exists(layout_path))
            try {
                dock.load(layout_path);
            } catch (const std::exception& e) {
                session.log(std::string("Layout reset: ") + e.what());
            }
        ui.find("scene_panel")->layout.width = dock.size("scene", 238);
        ui.find("inspector_panel")->layout.width = dock.size("inspector", 312);
        ui.find("bottom_panel")->layout.height = dock.size("bottom", 190);
        ui.set_clipboard([this] { return renderer.clipboard(); },
                         [this](const std::string& text) { renderer.set_clipboard(text); });
        ui.set_ime([this](bool enabled) { renderer.set_text_input(enabled); },
                   [this](ui::Rect rect) {
                       renderer.set_text_input_area(rect.x, rect.y, rect.width, rect.height);
                   });
        ui.set_docking(&dock, [this] { persist_layout(); });
        for (const auto* id : {"left_divider", "right_divider", "bottom_divider"})
            ui.find(id)->on_commit = [this](Widget&) { persist_layout(); };
        const auto recovered = call("faset_recovery_list");
        if (!recovered.is_null())
            for (const auto& item : recovered.at("recovery"))
                if (item.value("dirty", false))
                    recovery.push_back(item);
        build_static();
        const auto docs = session.authoring().documents();
        if (!docs.empty())
            document = docs.front().at("id");
        else
            document = session.authoring()
                           .create("Untitled", session.project().value("dimension", 3))
                           .at("id");
        ui_scale = std::clamp(renderer.display_scale(), .5f, 4.f);
        refresh();
    }
    void persist_layout() {
        dock.set_size("scene", ui.find("scene_panel")->rect.width / ui_scale);
        dock.set_size("inspector", ui.find("inspector_panel")->rect.width / ui_scale);
        dock.set_size("bottom", ui.find("bottom_panel")->rect.height / ui_scale);
        try {
            dock.save(layout_path);
        } catch (const std::exception& e) {
            report(e.what());
        }
    }
    void poll_presentation() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_presentation_poll < std::chrono::milliseconds(500))
            return;
        last_presentation_poll = now;
        try {
            const auto theme_text = read_text(theme_source_path);
            const auto layout_text = read_text(layout_source_path);
            if (theme_text == attempted_theme && layout_text == attempted_layout) {
                if (!presentation_error.empty() && theme_text == applied_theme &&
                    layout_text == applied_layout) {
                    presentation_error.clear();
                    report("UI theme/layout sources restored");
                }
                return;
            }
            attempted_theme = theme_text;
            attempted_layout = layout_text;
            const auto theme = ui::Theme::from_json(Json::parse(theme_text));
            const auto layout = Json::parse(layout_text);
            // Both candidates are fully validated before changing either. Retained
            // IDs, callbacks and dirty field edits stay in the existing Context.
            ui.validate_layout(layout);
            if (layout_text != applied_layout)
                ui.apply_layout(layout);
            if (theme_text != applied_theme)
                ui.set_theme(theme);
            applied_theme = theme_text;
            applied_layout = layout_text;
            presentation_error.clear();
            report("UI theme/layout reloaded");
        } catch (const std::exception& error) {
            const auto message = std::string("UI reload kept last working styles: ") + error.what();
            if (message != presentation_error) {
                presentation_error = message;
                report(message);
            }
        }
    }
    void report(const std::string& message) {
        status = message;
        session.log(message);
    }
    Json call(const std::string& name, Json args = Json::object()) {
        try {
            auto result = session.commands().call(name, args);
            if (result.contains("job"))
                status = "Started " + name + " (see Jobs)";
            return result;
        } catch (const std::exception& e) {
            report(e.what());
            return nullptr;
        }
    }
    bool transaction(Json operations,
                     std::uint64_t revision = std::numeric_limits<std::uint64_t>::max()) {
        if (document.empty())
            return false;
        const auto result = call("faset_scene_edit",
                                 {{"document", document},
                                  {"revision", revision == std::numeric_limits<std::uint64_t>::max()
                                                   ? current.at("revision").get<std::uint64_t>()
                                                   : revision},
                                  {"operations", operations}});
        if (result.is_null())
            return false;
        current = result;
        shown_revision = std::numeric_limits<std::uint64_t>::max();
        status = "Edited " + current.at("name").get<std::string>();
        return true;
    }
    void history(bool redo) {
        if (document.empty())
            return;
        auto result = call(redo ? "faset_redo" : "faset_undo",
                           {{"document", document}, {"revision", current.at("revision")}});
        if (!result.is_null()) {
            current = result;
            preview_fields.clear();
            shown_revision = std::numeric_limits<std::uint64_t>::max();
        }
    }
    void save() {
        ui.clear_focus();
        const auto path = current.value("path", std::string());
        if (path.empty()) {
            menu = "File";
            ui.update_text("save-path",
                           "Scenes/" + current.at("name").get<std::string>() + ".scene.json", true);
            return;
        }
        auto result = call("faset_document_save", {{"document", document}});
        if (!result.is_null()) {
            current = result;
            status = "Saved " + path;
        }
    }
    Widget& button(Widget& row, const std::string& id, const std::string& text,
                   std::function<void()> action, float width = -1) {
        auto& b = row.add(Kind::Button, id, text);
        b.layout.width = width;
        b.on_click = [action = std::move(action)](Widget&) { action(); };
        return b;
    }
    void create_object(const std::string& type) {
        const auto id = new_id();
        Json object = authoring::make_entity(session.authoring().schemas(), type);
        object["id"] = id;
        if (type == "Cube" || type == "Plane")
            object["components"].push_back({{"id", new_id()},
                                            {"type", "faset.mesh"},
                                            {"version", 1},
                                            {"fields",
                                             {{"asset", ""},
                                              {"color", {.65, .65, .68, 1.0}},
                                              {"primitive", type == "Plane" ? "plane" : "cube"}}}});
        else if (type == "Sprite")
            object["components"].push_back(
                {{"id", new_id()},
                 {"type", "faset.sprite"},
                 {"version", 1},
                 {"fields", session.authoring().schemas().default_fields("faset.sprite")}});
        if (transaction(Json::array({{{"op", "entity.create"}, {"entity", object}}})))
            select(id);
    }
    void build_static() {
        auto& menurow = ui.find("menubar")->add(Kind::Row, "menuitems");
        menurow.layout.gap = 0;
        for (const std::string name : {"Faset", "File", "Edit", "Scene", "View", "Help"}) {
            auto& item = button(
                menurow, "menu-" + name, name, [this, name] { menu = menu == name ? "" : name; },
                name == "Faset" ? 66 : name == "Scene" ? 58 : 50);
            item.appearance = ui::Appearance::Quiet;
            if (name == "Faset")
                item.font_size = 15;
        }
        menurow.add(Kind::Label, "menu-space").layout.flex = 1;
        auto& project = menurow.add(Kind::Label, "project-title",
                                    session.project().value("name", std::string("Project")));
        project.layout.width = 350;
        project.font_size = 12;
        project.enabled = false;
        auto& toolbar = ui.find("toolbar")->add(Kind::Row, "tools");
        toolbar.layout.gap = 3;
        auto& file_tools = toolbar.add(Kind::Row, "file-tools");
        file_tools.layout.width = 180;
        file_tools.layout.gap = 3;
        button(file_tools, "save", "Save", [this] { save(); }, 58).appearance =
            ui::Appearance::Quiet;
        button(file_tools, "undo", "Undo", [this] { history(false); }, 56).appearance =
            ui::Appearance::Quiet;
        button(file_tools, "redo", "Redo", [this] { history(true); }, 56).appearance =
            ui::Appearance::Quiet;
        toolbar.add(Kind::Label, "left-tools-space").layout.flex = 1;
        auto& play_tools = toolbar.add(Kind::Row, "play-tools");
        play_tools.layout.width = 265;
        play_tools.layout.gap = 4;
        button(play_tools, "play", "Play", [this] { call("faset_play", {{"document", document}}); },
               66)
            .appearance = ui::Appearance::Primary;
        button(
            play_tools, "pause", "Pause",
            [this] {
                if (!call("faset_play_control", {{"command", paused ? "resume" : "pause"}})
                         .is_null())
                    paused = !paused;
            },
            68)
            .appearance = ui::Appearance::Quiet;
        button(
            play_tools, "stop", "Stop",
            [this] {
                call("faset_stop");
                paused = false;
            },
            60)
            .appearance = ui::Appearance::Quiet;
        button(
            play_tools, "step", "Step",
            [this] { call("faset_play_control", {{"command", "step"}}); }, 56)
            .appearance = ui::Appearance::Quiet;
        toolbar.add(Kind::Label, "right-tools-space").layout.flex = 1;
        auto& build_tools = toolbar.add(Kind::Row, "build-tools");
        build_tools.layout.width = 354;
        build_tools.layout.gap = 3;
        button(build_tools, "build", "Build", [this] { call("faset_build"); }, 76).appearance =
            ui::Appearance::Quiet;
        button(
            build_tools, "export", "Export",
            [this] { call("faset_export", {{"document", document}, {"output", "Exports"}}); }, 72)
            .appearance = ui::Appearance::Quiet;
        button(
            build_tools, "simulation-settings", "Simulation",
            [this] { simulation_open = !simulation_open; }, 98)
            .appearance = ui::Appearance::Quiet;
        button(build_tools, "command-palette", "Commands", [this] { palette = !palette; }, 99)
            .appearance = ui::Appearance::Quiet;
        auto& scene = *ui.find("scene_panel");
        scene.add(Kind::Tab, "scene-tab", "Scene").selected = true;
        auto& tools = scene.add(Kind::Row, "scene-tools");
        tools.layout.height = 32;
        tools.layout.padding = 3;
        tools.layout.gap = 3;
        button(tools, "add-object", "+ Object", [this] { create_object("Object"); }, 80)
            .appearance = ui::Appearance::Quiet;
        button(tools, "add-cube", "Cube", [this] { create_object("Cube"); }, 55).appearance =
            ui::Appearance::Quiet;
        button(tools, "add-sprite", "Sprite", [this] { create_object("Sprite"); }, 58).appearance =
            ui::Appearance::Quiet;
        auto& tree = scene.add(Kind::Column, "scene-tree");
        tree.layout.flex = 1;
        tree.layout.scroll = true;
        tree.layout.gap = 0;
        tree.on_drop = [this](Widget&, const Json& payload) {
            if (payload.value("kind", std::string()) == "entity")
                reparent(payload.at("id"), "");
        };
        auto& inspector = *ui.find("inspector_panel");
        inspector.add(Kind::Tab, "inspector-tab", "Inspector").selected = true;
        auto& body = inspector.add(Kind::Column, "properties");
        body.layout.flex = 1;
        body.layout.padding = 10;
        body.layout.gap = 6;
        body.layout.scroll = true;
        auto& vp = *ui.find("viewport");
        auto& tool_surface = vp.add(Kind::Panel, "viewport-tool-surface");
        tool_surface.layout.absolute = true;
        tool_surface.layout.x = 8;
        tool_surface.layout.y = 8;
        tool_surface.layout.width = 352;
        tool_surface.layout.height = 36;
        tool_surface.layout.padding = 4;
        auto& vptools = tool_surface.add(Kind::Row, "viewport-tools");
        vptools.layout.height = 28;
        vptools.layout.gap = 4;
        for (const std::string mode : {"Move", "Rotate", "Scale"})
            button(vptools, "gizmo-" + mode, mode, [this, mode] { gizmo_mode = mode; }, 64)
                .appearance = ui::Appearance::Quiet;
        button(
            vptools, "back-document", "Back",
            [this] {
                if (!last_document.empty())
                    choose_document(last_document);
            },
            54)
            .appearance = ui::Appearance::Quiet;
        button(vptools, "frame-selection", "Frame", [this] { frame_selection(); }, 64)
            .appearance = ui::Appearance::Quiet;
        vp.on_drop = [this](Widget&, const Json& data) {
            if (data.value("kind", std::string()) == "asset")
                instantiate_asset(data.at("id").get<std::string>());
        };
        auto& bottom = *ui.find("bottom_panel");
        bottom.dock_area = "bottom";
        auto& tabs = bottom.add(Kind::Row, "bottom-tabs");
        tabs.layout.height = 29;
        tabs.layout.gap = 0;
        for (const std::string id : {"assets", "console", "jobs", "conflicts"}) {
            auto& tab = tabs.add(Kind::Tab, "tab-" + id,
                                 id == "assets"    ? "Assets"
                                 : id == "console" ? "Console"
                                                   : "Jobs");
            tab.layout.width = 94;
            tab.dock_area = "bottom";
            tab.dock_panel = id;
            tab.on_click = [this, id](Widget&) { active_bottom = id; };
        }
        auto& assetbar = bottom.add(Kind::Row, "asset-toolbar");
        assetbar.layout.height = 32;
        assetbar.layout.padding = 3;
        assetbar.layout.gap = 5;
        label(assetbar, "asset-path", "Project files", 102);
        assetbar.find("asset-path")->enabled = false;
        auto& search = assetbar.add(Kind::TextField, "asset-search", "");
        search.layout.width = 220;
        search.placeholder = "Search project files";
        search.tooltip = "Filter files by project-relative path";
        search.on_preview = [this](Widget& w) {
            asset_filter = w.text;
            assets_dirty = true;
        };
        button(
            assetbar, "asset-import", "Import / Reimport",
            [this] {
                if (!source_file.empty())
                    call("faset_import", {{"path", source_file}});
            },
            146)
            .appearance = ui::Appearance::Quiet;
        button(assetbar, "asset-open", "Open Scene", [this] { open_source(); }, 104)
            .appearance = ui::Appearance::Quiet;
        button(
            assetbar, "asset-refresh", "Refresh",
            [this] {
                assets_dirty = true;
                view.clearCache();
                picks_key.clear();
            },
            75)
            .appearance = ui::Appearance::Quiet;
        auto& items = bottom.add(Kind::Column, "asset-items");
        items.layout.flex = 1;
        items.layout.scroll = true;
        items.layout.gap = 0;
        auto& console = bottom.add(Kind::Column, "console-items");
        console.layout.flex = 1;
        console.layout.scroll = true;
        console.layout.gap = 0;
        auto& jobs = bottom.add(Kind::Column, "job-items");
        jobs.layout.flex = 1;
        jobs.layout.scroll = true;
        jobs.layout.gap = 1;
        auto& conflicts = bottom.add(Kind::Column, "conflict-items");
        conflicts.layout.flex = 1;
        conflicts.layout.scroll = true;
        conflicts.layout.gap = 3;
        auto& statusrow = ui.find("statusbar")->add(Kind::Row, "status-row");
        auto& statuslabel = statusrow.add(Kind::Label, "status", "Ready");
        statuslabel.layout.flex = 1;
        auto& schema_status = statusrow.add(Kind::Label, "schema-status");
        schema_status.layout.width = 220;
        schema_status.visible = false;
        schema_status.enabled = false;
        schema_status.tooltip = "Choose Build to refresh gameplay fields in Inspector";
        label(statusrow, "renderer-status", "Vulkan", 225);
        statusrow.find("renderer-status")->enabled = false;
        build_overlays();
    }
    void build_overlays() {
        auto& pop = ui.root().add(Kind::Panel, "menu-popup");
        pop.layout.absolute = true;
        pop.layout.x = 72;
        pop.layout.y = 35;
        pop.layout.width = 350;
        pop.layout.height = 355;
        pop.layout.padding = 8;
        pop.layout.gap = 5;
        pop.visible = false;
        label(pop, "menu-title", "File");
        button(pop, "new-3d", "New 3D scene", [this] { new_scene(3); });
        button(pop, "new-2d", "New 2D scene", [this] { new_scene(2); });
        button(pop, "project-settings", "Project settings", [this] { open_project_settings(); });
        button(pop, "open-project", "Open / create another project", [this] {
            if (!project_switch_enabled)
                return;
            ui.clear_focus();
            if (ui.editing())
                return;
            bool warn = session.playing();
            for (const auto& doc : session.authoring().documents())
                warn = warn || doc.value("dirty", false);
            const auto jobs = call("faset_jobs");
            if (!jobs.is_null())
                for (const auto& job : jobs.at("jobs"))
                    warn = warn || job.value("state", std::string()) == "running" ||
                           job.value("state", std::string()) == "queued";
            project_switch_warning = warn;
            project_switch_requested = !warn;
            menu.clear();
        });
        pop.add(Kind::TextField, "open-path", "Scenes/Main.scene.json");
        button(pop, "open-path-button", "Open project-relative scene", [this] {
            auto result = call("faset_document_open", {{"path", ui.find("open-path")->text}});
            if (!result.is_null()) {
                choose_document(result.at("id"));
                menu.clear();
            }
        });
        pop.add(Kind::TextField, "save-path", "Scenes/Untitled.scene.json");
        button(pop, "save-as-button", "Save scene as", [this] {
            auto result = call("faset_document_save",
                               {{"document", document}, {"path", ui.find("save-path")->text}});
            if (!result.is_null()) {
                current = result;
                menu.clear();
                assets_dirty = true;
            }
        });
        button(pop, "menu-duplicate", "Duplicate selection", [this] {
            if (!selected.empty())
                transaction(Json::array({{{"op", "entity.duplicate"}, {"entity", selected}}}));
            menu.clear();
        });
        button(pop, "menu-delete", "Delete selection", [this] {
            delete_selected();
            menu.clear();
        });
        pop.add(Kind::TextField, "template-path", "Assets/Templates/Template.scene.json");
        button(pop, "save-template", "Save selection as template",
               [this] { save_selection_template(ui.find("template-path")->text); });
        button(pop, "instance-template", "Instance scene at this path", [this] {
            instance_scene(ui.find("template-path")->text);
            menu.clear();
        });
        label(pop, "help-one", "Orbit: right drag. Pan: middle drag. Wheel: zoom.");
        label(pop, "help-two", "W / E / R: gizmos. F: frame. Ctrl+P: commands.");
        label(pop, "help-three", "Ctrl+S save. Ctrl+Z / Shift+Z undo / redo.");
        auto& command = ui.root().add(Kind::Panel, "palette");
        command.layout.absolute = true;
        command.layout.width = 570;
        command.layout.height = 470;
        command.layout.padding = 12;
        command.layout.gap = 6;
        command.visible = false;
        label(command, "palette-title", "Editor commands · authoring only");
        auto& filter = command.add(Kind::TextField, "palette-filter", "");
        filter.on_preview = [](Widget&) {};
        auto& list = command.add(Kind::Column, "palette-list");
        list.layout.flex = 1;
        list.layout.scroll = true;
        list.layout.gap = 1;
        command.add(Kind::TextField, "palette-arguments", "{}");
        button(command, "palette-run", "Run selected command", [this] {
            try {
                const auto result =
                    call(command_name, Json::parse(ui.find("palette-arguments")->text));
                if (!result.is_null()) {
                    session.log(result.dump(2));
                    status = "Completed " + command_name;
                    assets_dirty = true;
                }
            } catch (const std::exception& e) {
                report(e.what());
            }
        });
        button(command, "palette-close", "Close", [this] { palette = false; });
        auto& settings = ui.root().add(Kind::Panel, "simulation-panel");
        settings.layout.absolute = true;
        settings.layout.width = 470;
        settings.layout.height = 350;
        settings.layout.padding = 12;
        settings.layout.gap = 5;
        settings.visible = false;
        label(settings, "simulation-title", "Scene simulation");
        label(settings, "simulation-scope", "Used by the next Player snapshot");
        for (const std::string field : {"tick-rate", "max_catch_up_ticks", "physics_substeps"}) {
            auto& row = settings.add(Kind::Row, "simulation-row-" + field);
            row.layout.height = 32;
            label(row, "simulation-label-" + field,
                  field == "tick-rate"            ? "Fixed tick rate (Hz)"
                  : field == "max_catch_up_ticks" ? "Maximum catch-up ticks"
                                                  : "Physics substeps",
                  235);
            auto& input = row.add(Kind::NumberField, "simulation-" + field);
            input.layout.flex = 1;
            input.precision = field == "tick-rate" ? 3 : 0;
            input.step = 1;
        }
        label(settings, "simulation-gravity-label", "Gravity (units / second squared)");
        auto& gravity = settings.add(Kind::Row, "simulation-gravity");
        gravity.layout.height = 32;
        for (int axis = 0; axis < 3; ++axis) {
            auto& input =
                gravity.add(Kind::NumberField, "simulation-gravity-" + std::to_string(axis));
            input.layout.flex = 1;
            input.precision = 3;
            input.step = .05;
        }
        button(settings, "simulation-close", "Close", [this] { simulation_open = false; });
        auto& recover = ui.root().add(Kind::Panel, "recovery-panel");
        recover.layout.absolute = true;
        recover.layout.width = 470;
        recover.layout.height = 260;
        recover.layout.padding = 12;
        recover.layout.gap = 5;
        recover.visible = false;
        auto& switching = ui.root().add(Kind::Panel, "project-switch-dialog");
        switching.layout.absolute = true;
        switching.layout.width = 500;
        switching.layout.height = 220;
        switching.layout.padding = 16;
        switching.layout.gap = 8;
        switching.visible = false;
        label(switching, "project-switch-title", "Switch project?");
        label(switching, "project-switch-dirty", "Unsaved changes remain in recovery journals.");
        label(switching, "project-switch-jobs", "Active jobs and the Player stop on switching.");
        label(switching, "project-switch-save", "Cancel to save your scenes first.");
        auto& actions = switching.add(Kind::Row, "project-switch-actions");
        actions.layout.height = 32;
        button(
            actions, "project-switch-continue", "Switch project",
            [this] {
                project_switch_warning = false;
                project_switch_requested = project_switch_enabled;
            },
            180);
        button(
            actions, "project-switch-cancel", "Cancel", [this] { project_switch_warning = false; },
            90);
        auto& project = ui.root().add(Kind::Panel, "project-settings-panel");
        project.layout.absolute = true;
        project.layout.width = 610;
        project.layout.height = 550;
        project.layout.padding = 16;
        project.layout.gap = 6;
        project.layout.scroll = true;
        project.visible = false;
        auto& project_title =
            project.add(Kind::Label, "project-settings-title", "Project settings");
        project_title.font_size = 20;
        project_title.layout.height = 36;
        label(project, "project-settings-name-label", "Project name");
        project.add(Kind::TextField, "project-settings-name");
        label(project, "project-settings-dimension-label", "Initial scene type");
        auto& dimensions = project.add(Kind::Row, "project-settings-dimensions");
        dimensions.layout.height = 30;
        for (const auto value : {2, 3}) {
            auto& choice =
                dimensions.add(Kind::Tab, "project-settings-" + std::to_string(value) + "d",
                               std::to_string(value) + "D");
            choice.layout.width = 110;
            choice.on_click = [this, value](Widget&) { project_settings_dimension = value; };
        }
        label(project, "project-settings-start-label", "Start scene (saved project-relative path)");
        project.add(Kind::TextField, "project-settings-start");
        auto& scenes = project.add(Kind::Column, "project-settings-scenes");
        scenes.layout.height = 104;
        scenes.layout.scroll = true;
        scenes.layout.gap = 1;
        label(project, "project-settings-note",
              "Applies on next project open. Scene Undo is unchanged.");
        auto& project_actions = project.add(Kind::Row, "project-settings-actions");
        project_actions.layout.height = 32;
        button(
            project_actions, "project-settings-save", "Save project",
            [this] { save_project_settings(); }, 145)
            .selected = true;
        button(
            project_actions, "project-settings-reload", "Reload saved",
            [this] { open_project_settings(); }, 130);
        button(
            project_actions, "project-settings-cancel", "Cancel",
            [this] {
                project_settings_open = false;
                ui.clear_focus(false);
            },
            85);
        label(project, "project-settings-error", "");
        label(project, "project-settings-reload-note", "Reload saved discards this form's edits.");
    }
    void new_scene(int dimension) {
        auto result =
            call("faset_document_create", {{"name", "Untitled"}, {"dimension", dimension}});
        if (!result.is_null())
            choose_document(result.at("id"));
        menu.clear();
    }
    void choose_document(std::string id) {
        if (document != id)
            last_document = document;
        instance_selection = Json::array();
        ui.clear_focus(false);
        document = id;
        selected.clear();
        current = session.authoring().query(id);
        shown_revision = std::numeric_limits<std::uint64_t>::max();
        preview_fields.clear();
        edit_revisions.clear();
        gizmo_axis = -1;
    }
    bool inherited(const Json& object) const {
        return object.contains("origin") &&
               !object.at("origin").value("path", Json::array()).empty();
    }
    bool owned_addition(const Json& object) const {
        return inherited(object) && object.at("origin").value("local", false) &&
               object.at("origin").at("path").size() == 1;
    }
    Json addition_record(const Json& object) const {
        for (const auto& instance : current.at("scene").value("instances", Json::array()))
            if (instance.at("id") == object.at("origin").at("path").front())
                for (const auto& item : instance.value("additions", Json::array()))
                    if (item.at("id") == object.at("origin").at("object"))
                        return item;
        throw std::runtime_error("Instance-local addition is unavailable");
    }
    void update_addition(const Json& object, Json addition,
                         std::uint64_t revision = std::numeric_limits<std::uint64_t>::max()) {
        transaction(Json::array({{{"op", "template.addition_set"},
                                  {"instance", object.at("origin").at("path").front()},
                                  {"value", std::move(addition)}}}),
                    revision);
    }
    void add_component(const std::string& type) {
        const auto* object = entity(resolved, selected);
        if (!object)
            return;
        if (owned_addition(*object)) {
            auto addition = addition_record(*object);
            addition["components"].push_back(
                {{"id", new_id()},
                 {"type", type},
                 {"version", session.authoring().schemas().schema(type).value("version", 1)},
                 {"fields", session.authoring().schemas().default_fields(type)}});
            update_addition(*object, std::move(addition));
            component_menu = false;
        } else if (!inherited(*object) &&
                   transaction(Json::array(
                       {{{"op", "component.add"}, {"entity", selected}, {"type", type}}})))
            component_menu = false;
    }
    void remove_component(const std::string& cid) {
        const auto* object = entity(resolved, selected);
        if (!object)
            return;
        if (owned_addition(*object)) {
            std::string source;
            for (const auto& c : object->at("components"))
                if (c.at("id") == cid)
                    source = c.at("source_id");
            auto addition = addition_record(*object);
            auto& components = addition["components"];
            components.erase(std::remove_if(components.begin(), components.end(),
                                            [&](const Json& c) { return c.at("id") == source; }),
                             components.end());
            update_addition(*object, std::move(addition));
        } else if (!inherited(*object))
            transaction(Json::array(
                {{{"op", "component.remove"}, {"entity", selected}, {"component", cid}}}));
    }
    void migrate_component(const std::string& cid) {
        const auto* object = entity(resolved, selected);
        if (!object)
            return;
        Json operation = {{"op", "component.migrate"}, {"entity", selected}, {"component", cid}};
        if (owned_addition(*object)) {
            operation["instance"] = object->at("origin").at("path").front();
            operation["entity"] = object->at("origin").at("object");
            for (const auto& c : object->at("components"))
                if (c.at("id") == cid)
                    operation["component"] = c.at("source_id");
        } else if (inherited(*object)) {
            open_template_source(object->at("origin").at("path"),
                                 object->at("origin").at("object"));
            return;
        }
        transaction(Json::array({operation}));
    }
    Json relative_address(const Json& object, const std::string& component_id = {},
                          const std::string& field = {}) const {
        const auto& origin = object.at("origin");
        auto path = origin.at("path");
        path.erase(path.begin());
        Json address = {{"path", path}, {"object", origin.at("object")}};
        if (!component_id.empty()) {
            for (const auto& c : object.at("components"))
                if (c.at("id") == component_id) {
                    address["component"] = c.value("source_id", component_id);
                    break;
                }
            address["field"] = field;
        }
        return address;
    }
    Json field_operation(const Json& object, const std::string& component_id,
                         const std::string& field, Json value) const {
        if (inherited(object))
            return {{"op", "template.override"},
                    {"instance", object.at("origin").at("path").front()},
                    {"address", relative_address(object, component_id, field)},
                    {"value", std::move(value)}};
        return {{"op", "component.set"},
                {"entity", object.at("id")},
                {"component", component_id},
                {"field", field},
                {"value", std::move(value)}};
    }
    bool overridden(const Json& object, const std::string& component_id,
                    const std::string& field) const {
        if (!inherited(object))
            return false;
        const auto address = relative_address(object, component_id, field);
        for (const auto& instance : current.at("scene").value("instances", Json::array()))
            if (instance.at("id") == object.at("origin").at("path").front())
                for (const auto& change : instance.value("overrides", Json::array()))
                    if (change.at("address") == address)
                        return true;
        return false;
    }
    std::string instance_source(const Json& path) const {
        auto source_scene = current.at("scene");
        std::string source;
        for (const auto& id : path) {
            const auto instances = source_scene.value("instances", Json::array());
            auto found = std::find_if(instances.begin(), instances.end(),
                                      [&](const Json& item) { return item.at("id") == id; });
            if (found == instances.end())
                return {};
            source = found->at("source");
            bool open = false;
            for (const auto& doc : session.authoring().documents())
                if (doc.value("path", std::string()) == source) {
                    source_scene = session.authoring().query(doc.at("id"))["scene"];
                    open = true;
                    break;
                }
            if (!open)
                try {
                    source_scene = read_json(
                        project_path(session.config().project_root, path_from_utf8(source)));
                } catch (...) {
                    return source;
                }
        }
        return source;
    }
    void open_template_source(const Json& path, const std::string& object = {}) {
        const auto source = instance_source(path);
        if (source.empty())
            return;
        const auto opened = call("faset_document_open", {{"path", source}});
        if (!opened.is_null()) {
            choose_document(opened.at("id"));
            selected = object;
        }
    }
    void select_instance(const Json& path) {
        ui.clear_focus();
        selected.clear();
        instance_selection = path;
        preview_fields.clear();
        edit_revisions.clear();
    }
    void delete_selected() {
        if (!instance_selection.empty()) {
            if (instance_selection.size() != 1) {
                report("Open the source scene to remove this nested instance");
                return;
            }
            if (transaction(Json::array(
                    {{{"op", "template.remove"}, {"instance", instance_selection.front()}}})))
                instance_selection = Json::array();
            return;
        }
        const auto* object = entity(resolved, selected);
        if (!object)
            return;
        const Json operation = inherited(*object)
                                   ? Json{{"op", "template.suppress"},
                                          {"instance", object->at("origin").at("path").front()},
                                          {"value", relative_address(*object)}}
                                   : Json{{"op", "entity.delete"}, {"entity", selected}};
        if (transaction(Json::array({operation})))
            selected.clear();
    }
    void reparent(const std::string& id, const std::string& parent) {
        const auto* object = entity(resolved, id);
        const auto* target = parent.empty() ? nullptr : entity(resolved, parent);
        if (!object)
            return;
        if (inherited(*object)) {
            const auto path = object->at("origin").at("path");
            if (target && (!inherited(*target) || target->at("origin").at("path") != path)) {
                report("Instance reparenting stays within the same instance path");
                return;
            }
            transaction(
                Json::array({{{"op", "template.reparent"},
                              {"instance", path.front()},
                              {"value",
                               {{"object", relative_address(*object)},
                                {"parent", target ? relative_address(*target) : Json(nullptr)},
                                {"keep_world", true}}}}}));
        } else {
            if (target && inherited(*target)) {
                report("Add a local child through the instance Inspector");
                return;
            }
            transaction(Json::array({{{"op", "entity.reparent"},
                                      {"entity", id},
                                      {"parent", parent.empty() ? Json(nullptr) : Json(parent)},
                                      {"keep_world", true}}}));
        }
    }
    void add_instance_child(const Json& path, const std::string& parent = {}) {
        if (path.size() != 1) {
            report("Open the nested source to add a child there");
            return;
        }
        auto object = authoring::make_entity(session.authoring().schemas(), "Local Object", parent);
        transaction(
            Json::array({{{"op", "template.add"}, {"instance", path.front()}, {"value", object}}}));
    }
    void instance_scene(const std::string& path) {
        try {
            auto data =
                read_json(project_path(session.config().project_root, path_from_utf8(path)));
            authoring::validate_scene(data, session.authoring().schemas());
            if (data.at("id") == document)
                throw std::runtime_error("A scene cannot instance itself");
            const auto id = new_id();
            if (transaction(Json::array({{{"op", "template.instance"},
                                          {"instance",
                                           {{"id", id},
                                            {"source", path},
                                            {"overrides", Json::array()},
                                            {"suppressed", Json::array()},
                                            {"additions", Json::array()},
                                            {"reparents", Json::array()}}}}})))
                select_instance(Json::array({id}));
        } catch (const std::exception& e) {
            report(e.what());
        }
    }
    void save_selection_template(const std::string& path) {
        const auto* object = entity(resolved, selected);
        if (!object) {
            report("Select an object subtree to create a template");
            return;
        }
        try {
            std::set<std::string> subtree{selected};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& item : resolved.at("entities"))
                    if (item.at("parent").is_string() &&
                        subtree.contains(item.at("parent").get<std::string>()))
                        changed =
                            subtree.insert(item.at("id").get<std::string>()).second || changed;
            }
            auto result = call("faset_document_create",
                               {{"name", path_to_utf8(path_from_utf8(path).stem())},
                                {"dimension", current.at("scene").value("dimension", 3)}});
            if (result.is_null())
                return;
            Json operations = Json::array();
            for (auto item : resolved.at("entities"))
                if (subtree.contains(item.at("id").get<std::string>())) {
                    item.erase("origin");
                    for (auto& c : item["components"])
                        c.erase("source_id");
                    if (item.at("id") == selected)
                        item["parent"] = nullptr;
                    operations.push_back({{"op", "entity.create"}, {"entity", item}});
                }
            const auto id = result.at("id");
            result = call("faset_scene_edit", {{"document", id},
                                               {"revision", result.at("revision")},
                                               {"operations", operations}});
            if (result.is_null())
                return;
            result = call("faset_document_save", {{"document", id}, {"path", path}});
            if (!result.is_null()) {
                status = "Saved template: " + path;
                source_file = path;
                assets_dirty = true;
                menu.clear();
            }
        } catch (const std::exception& e) {
            report(e.what());
        }
    }
    void select(const std::string& id) {
        if (selected == id && instance_selection.empty())
            return;
        ui.clear_focus();
        selected = id;
        instance_selection = Json::array();
        preview_fields.clear();
        edit_revisions.clear();
        component_menu = false;
    }
    void open_source() {
        if (source_file.empty())
            return;
        if (path_from_utf8(source_file).extension() == ".lua") {
            const auto result = call("faset_script_open", {{"path", source_file}});
            if (!result.is_null())
                status = "Opened in external editor: " + source_file;
            return;
        }
        auto result = call("faset_document_open", {{"path", source_file}});
        if (!result.is_null())
            choose_document(result.at("id"));
    }
    void instantiate_asset(const std::string& id) {
        Json manifest;
        for (const auto& entry : assets)
            if (entry.at("id") == id && entry.contains("manifest"))
                manifest = entry.at("manifest");
        const bool image = manifest.is_object() && manifest.value("kind", std::string()) == "image";
        const auto name =
            manifest.is_object()
                ? path_to_utf8(
                      path_from_utf8(manifest.value("source", std::string("Imported asset")))
                          .stem())
                : "Imported asset";
        auto object = authoring::make_entity(session.authoring().schemas(), name);
        const auto entity_id = object.at("id").get<std::string>();
        if (image) {
            auto fields = session.authoring().schemas().default_fields("faset.sprite");
            fields["texture"] = id;
            fields["color"] = {1, 1, 1, 1};
            if (manifest.contains("image")) {
                const auto& info = manifest.at("image");
                const auto ppu = info.value("pixels_per_unit", 100.0);
                fields["size"] = {info.at("width").get<double>() / ppu,
                                  info.at("height").get<double>() / ppu};
            }
            object["components"].push_back(
                {{"id", new_id()}, {"type", "faset.sprite"}, {"version", 1}, {"fields", fields}});
        } else
            object["components"].push_back(
                {{"id", new_id()},
                 {"type", "faset.mesh"},
                 {"version", 1},
                 {"fields", {{"asset", id}, {"primitive", "asset"}, {"color", {1, 1, 1, 1}}}}});
        if (transaction(Json::array({{{"op", "entity.create"}, {"entity", object}}})))
            select(entity_id);
    }
    void refresh() {
        if (document.empty())
            return;
        current = session.authoring().query(document);
        schemas = session.authoring().schemas().manifest();
        // Resolution also depends on schemas: a rebuilt component version may
        // invalidate an override or change how entity references are remapped.
        const auto document_stamp = Json::array({session.authoring().documents(), schemas}).dump();
        if (shown_revision != current.at("revision").get<std::uint64_t>() ||
            resolved_stamp != document_stamp) {
            resolved_stamp = document_stamp;
            auto result = call("faset_template_preview", {{"document", document}});
            resolved = result.is_null() ? current.at("scene") : result.at("scene");
            template_conflicts = result.is_null() ? Json::array() : result.at("conflicts");
            for (auto& object : resolved["entities"])
                if (!object.contains("origin"))
                    object["origin"] = {{"path", Json::array()}, {"object", object.at("id")}};
            if (!result.is_null() && !result.at("conflicts").empty())
                status = "Template conflicts: " + std::to_string(result.at("conflicts").size());
            shown_revision = current.at("revision");
        }
        if (!selected.empty() && !entity(resolved, selected))
            selected.clear();
        if (!instance_selection.empty() && instance_source(instance_selection).empty())
            instance_selection = Json::array();
        refresh_tree();
        refresh_inspector();
        refresh_assets();
        refresh_bottom();
        refresh_overlays();
        ui.find("undo")->enabled = current.value("can_undo", false);
        ui.find("redo")->enabled = current.value("can_redo", false);
        ui.find("pause")->enabled = session.playing();
        ui.find("stop")->enabled = session.playing() || session.play_pending();
        ui.find("step")->enabled = session.playing();
        ui.find("pause")->text = paused ? "Resume" : "Pause";
        for (const std::string mode : {"Move", "Rotate", "Scale"})
            ui.find("gizmo-" + mode)->selected = gizmo_mode == mode;
        for (const std::string name : {"Faset", "File", "Edit", "Scene", "View", "Help"})
            ui.find("menu-" + name)->selected = menu == name;
        const auto now = std::chrono::steady_clock::now();
        // Source fingerprints read complete script bytes; do not hash them every render frame.
        if (schema_state.is_null() || now - last_schema_poll >= std::chrono::seconds(1)) {
            schema_state = call("faset_schema_status");
            last_schema_poll = now;
        }
        if (!schema_state.is_null()) {
            const bool stale = schema_state.value("stale", false);
            auto& build = *ui.find("build");
            build.text = "Build";
            const auto schema_error = schema_state.value("error", std::string());
            build.tooltip = stale ? "Gameplay metadata is out of date. Build to refresh Inspector "
                                    "fields."
                                  : "Build gameplay code and refresh Inspector metadata";
            if (stale && !schema_error.empty())
                build.tooltip += " Last error: " + schema_error;
            ui.find("schema-status")->visible = stale;
            ui.find("schema-status")->text = "Gameplay metadata out of date";
        }

        const auto project_name = session.project().value("name", std::string("Project"));
        const auto scene_name = current.at("name").get<std::string>();
        const auto scene_path = current.value("path", std::string());
        const auto scene_label = scene_path.empty()
                                     ? scene_name
                                     : path_to_utf8(path_from_utf8(scene_path).filename());
        ui.find("project-title")->text = project_name + "  /  " + scene_label +
                                         (current.value("dirty", false) ? " *" : "");
        ui.find("project-title")->tooltip = project_name + " / " + scene_name;
        ui.find("status")->text = status;
        ui.find("renderer-status")->text =
            "Vulkan 1.3  |  " + std::to_string(resolved.at("entities").size()) + " objects";
    }
    void refresh_tree() {
        auto& tree = *ui.find("scene-tree");
        std::set<std::string> keep;
        std::map<std::string, std::size_t> order;
        std::size_t sequence = 0;
        auto touch = [&](Widget& row) {
            keep.insert(row.id);
            order[row.id] = sequence++;
        };
        const auto full_scene_name = current.at("scene").at("name").get<std::string>();
        auto scene_name = full_scene_name;
        if (ui.font().measure(scene_name, ui.theme().font_size) >
            ui.find("scene_panel")->layout.width - 18) {
            const auto path = current.value("path", std::string());
            if (!path.empty())
                scene_name = path_to_utf8(path_from_utf8(path).filename());
        }
        auto& root = tree.add(Kind::TreeRow, "scene-root", scene_name);
        root.tooltip = full_scene_name;
        root.selected = selected.empty() && instance_selection.empty();
        root.on_click = [this](Widget&) { select(""); };
        touch(root);
        std::vector<Json> groups;
        for (const auto& instance : current.at("scene").value("instances", Json::array()))
            groups.push_back(Json::array({instance.at("id")}));
        for (const auto& object : resolved.at("entities")) {
            const auto path = object.at("origin").at("path");
            Json prefix = Json::array();
            for (const auto& part : path) {
                prefix.push_back(part);
                if (std::find(groups.begin(), groups.end(), prefix) == groups.end())
                    groups.push_back(prefix);
            }
        }
        for (const auto& conflict : template_conflicts) {
            Json prefix = Json::array();
            for (const auto& part : conflict.at("instance_path")) {
                prefix.push_back(part);
                if (std::find(groups.begin(), groups.end(), prefix) == groups.end())
                    groups.push_back(prefix);
            }
        }
        std::function<void(const Json&, std::string, int)> objects = [&](const Json& path,
                                                                         std::string parent,
                                                                         int depth) {
            for (const auto& object : resolved.at("entities")) {
                if (object.at("origin").at("path") != path)
                    continue;
                const auto parent_id =
                    object.at("parent").is_string() ? object.at("parent").get<std::string>() : "";
                const auto* parent_object =
                    parent_id.empty() ? nullptr : entity(resolved, parent_id);
                const bool own_parent =
                    parent_object && parent_object->at("origin").at("path") == path;
                if ((own_parent ? parent_id : "") != parent)
                    continue;
                const auto id = object.at("id").get<std::string>();
                auto& row = tree.add(Kind::TreeRow, "entity-" + id, object.at("name"));
                row.indent = depth;
                row.selected = selected == id;
                row.drag_payload = {{"kind", "entity"}, {"id", id}, {"label", object.at("name")}};
                row.on_click = [this, id](Widget&) { select(id); };
                row.on_drop = [this, id](Widget&, const Json& payload) {
                    if (payload.value("kind", std::string()) == "entity")
                        reparent(payload.at("id"), id);
                };
                touch(row);
                objects(path, id, depth + 1);
            }
        };
        objects(Json::array(), "", 1);
        std::function<void(Json, int)> append_group = [&](Json path, int depth) {
            const auto source = instance_source(path);
            auto& row = tree.add(Kind::TreeRow, "instance-" + path.dump(),
                                 "[T] " + path_to_utf8(path_from_utf8(source).filename()));
            row.indent = depth;
            row.selected = instance_selection == path;
            row.on_click = [this, path](Widget&) { select_instance(path); };
            touch(row);
            objects(path, "", depth + 1);
            for (const auto& candidate : groups) {
                if (candidate.size() != path.size() + 1)
                    continue;
                auto prefix = candidate;
                prefix.erase(prefix.end() - 1);
                if (prefix == path)
                    append_group(candidate, depth + 1);
            }
        };
        for (const auto& group : groups)
            if (group.size() == 1)
                append_group(group, 1);
        trim_children(tree, keep);
        std::stable_sort(tree.children.begin(), tree.children.end(),
                         [&](const auto& a, const auto& b) { return order[a->id] < order[b->id]; });
    }
    void set_field(const std::string& component_id, const std::string& field, Json value,
                   const std::string& widget) {
        const auto found = edit_revisions.find(widget);
        const auto revision = found == edit_revisions.end()
                                  ? current.at("revision").get<std::uint64_t>()
                                  : found->second;
        if (const auto* object = entity(resolved, selected))
            transaction(
                Json::array({field_operation(*object, component_id, field, std::move(value))}),
                revision);
        edit_revisions.erase(widget);
        preview_fields.erase(component_id + "/" + field);
    }
    void bind_field(Widget& w, const std::string& cid, const std::string& field, Json original,
                    int index = -1) {
        const auto id = w.id;
        w.on_preview = [this, id, cid, field, original, index](Widget& widget) {
            edit_revisions.try_emplace(id, current.at("revision").get<std::uint64_t>());
            if (widget.kind == Kind::NumberField) {
                try {
                    auto value = std::stod(widget.text);
                    if (!std::isfinite(value))
                        return;
                    Json changed = original;
                    if (index >= 0)
                        changed[index] = value;
                    else if (original.is_number_integer())
                        changed = std::int64_t(std::llround(value));
                    else
                        changed = value;
                    preview_fields[cid + "/" + field] = changed;
                } catch (...) {
                }
            }
        };
        w.on_commit = [this, cid, field, original, index, id](Widget& widget) {
            Json changed = original;
            if (widget.kind == Kind::Checkbox)
                changed = widget.checked;
            else if (widget.kind == Kind::NumberField) {
                if (index >= 0)
                    changed[index] = widget.value;
                else if (original.is_number_integer())
                    changed = std::int64_t(std::llround(widget.value));
                else
                    changed = widget.value;
            } else if (original.is_string())
                changed = widget.text;
            else
                try {
                    changed = Json::parse(widget.text);
                } catch (const std::exception& e) {
                    report(e.what());
                    return;
                }
            set_field(cid, field, changed, id);
        };
        w.on_cancel = [this, cid, field, id](Widget&) {
            preview_fields.erase(cid + "/" + field);
            edit_revisions.erase(id);
        };
        w.on_drop = [this, cid, field, id](Widget&, const Json& payload) {
            if (payload.value("kind", std::string()) == "asset")
                set_field(cid, field, payload.at("id"), id);
        };
    }
    void refresh_instance_inspector(Widget& body, std::set<std::string>& keep) {
        const auto path = instance_selection;
        const auto source = instance_source(path);
        label(body, "instance-heading", "Scene instance");
        keep.insert("instance-heading");
        auto& field = body.add(Kind::TextField, "instance-source");
        ui.update_text(field.id, source);
        field.enabled = path.size() == 1;
        field.on_commit = [this, path](Widget& w) {
            transaction(Json::array(
                {{{"op", "template.source_set"}, {"instance", path.front()}, {"source", w.text}}}));
        };
        keep.insert(field.id);
        button(body, "instance-open-source", "Open source",
               [this, path] { open_template_source(path); });
        keep.insert("instance-open-source");
        auto& remove =
            button(body, "instance-remove", "Remove instance", [this] { delete_selected(); });
        remove.enabled = path.size() == 1;
        keep.insert(remove.id);
        auto& add = button(body, "instance-add-local", "Add local object",
                           [this, path] { add_instance_child(path); });
        add.enabled = path.size() == 1;
        keep.insert(add.id);
        if (path.size() != 1) {
            label(body, "instance-nested-note", "Open source for nested structural changes");
            keep.insert("instance-nested-note");
            return;
        }
        for (const auto& instance : current.at("scene").value("instances", Json::array()))
            if (instance.at("id") == path.front()) {
                std::size_t i = 0;
                for (const auto& address : instance.value("suppressed", Json::array())) {
                    const auto id = "instance-restore-" + std::to_string(i++);
                    button(body, id,
                           "Restore suppressed object " +
                               address.at("object").get<std::string>().substr(0, 8),
                           [this, path, address] {
                               transaction(Json::array({{{"op", "template.restore"},
                                                         {"instance", path.front()},
                                                         {"address", address}}}));
                           });
                    keep.insert(id);
                }
            }
    }
    void refresh_inspector() {
        auto& body = *ui.find("properties");
        std::set<std::string> keep;
        std::vector<std::string> ordered;
        const auto keep_in_order = [&](const std::string& id) {
            keep.insert(id);
            ordered.push_back(id);
        };
        const auto* e = entity(resolved, selected);
        if (!instance_selection.empty()) {
            refresh_instance_inspector(body, keep);
            trim_children(body, keep);
            return;
        }
        if (!e) {
            label(body, "inspector-empty", "Select an object in Scene to edit it");
            body.find("inspector-empty")->enabled = false;
            keep.insert("inspector-empty");
            trim_children(body, keep);
            return;
        }
        auto& identity = body.add(Kind::Row, "object-identity");
        keep_in_order(identity.id);
        identity.layout.height = 30;
        identity.layout.gap = 4;
        label(identity, "object-name-label", "Name", 90);
        identity.find("object-name-label")->enabled = false;
        auto& name = identity.add(Kind::TextField, "object-name");
        name.layout.flex = 1;
        ui.update_text(name.id, e->at("name"));
        const bool source_object = inherited(*e), local_addition = owned_addition(*e);
        name.enabled = !source_object || local_addition;
        if (local_addition) {
            label(body, "object-origin", "Local addition to this instance");
            keep_in_order("object-origin");
        } else if (source_object) {
            const auto path = e->at("origin").at("path");
            const auto object = e->at("origin").at("object").get<std::string>();
            label(body, "object-origin",
                  "Source: " + path_to_utf8(path_from_utf8(instance_source(path)).filename()));
            keep_in_order("object-origin");
            button(body, "object-open-source", "Open source",
                   [this, path, object] { open_template_source(path, object); });
            keep_in_order("object-open-source");
        }
        name.on_preview = [this](Widget&) {
            edit_revisions.try_emplace("object-name", current.at("revision").get<std::uint64_t>());
        };
        name.on_cancel = [this](Widget& w) { edit_revisions.erase(w.id); };
        name.on_commit = [this](Widget& w) {
            const auto revision = edit_revisions.contains(w.id)
                                      ? edit_revisions[w.id]
                                      : current.at("revision").get<std::uint64_t>();
            if (const auto* object = entity(resolved, selected)) {
                if (owned_addition(*object)) {
                    auto addition = addition_record(*object);
                    addition["name"] = w.text;
                    update_addition(*object, std::move(addition), revision);
                } else
                    transaction(
                        Json::array(
                            {{{"op", "entity.rename"}, {"entity", selected}, {"name", w.text}}}),
                        revision);
            }
            edit_revisions.erase(w.id);
        };
        for (const auto& c : e->at("components")) {
            const auto cid = c.at("id").get<std::string>(), type = c.at("type").get<std::string>();
            const bool has_schema = session.authoring().schemas().contains(type);
            const bool known =
                has_schema && c.value("version", 1) ==
                                  session.authoring().schemas().schema(type).value("version", 1);
            const auto metadata = known ? session.authoring().schemas().schema(type)
                                        : Json{{"name", type + (has_schema ? " (version mismatch)"
                                                                           : " (schema missing)")},
                                               {"fields", Json::object()}};
            auto& header = body.add(Kind::Row, "component-header-" + cid);
            keep_in_order(header.id);
            header.layout.height = 32;
            header.appearance = ui::Appearance::Section;
            auto& title =
                header.add(Kind::Label, "component-title-" + cid, metadata.value("name", type));
            title.layout.flex = 1;
            title.font_size = 14;
            button(
                header, "component-remove-" + cid, "x", [this, cid] { remove_component(cid); }, 25)
                .appearance = ui::Appearance::Quiet;
            header.find("component-remove-" + cid)->enabled = !source_object || local_addition;
            if (!known) {
                label(body, "opaque-note-" + cid, "Schema unavailable. Data is preserved.");
                keep_in_order("opaque-note-" + cid);
                if (has_schema &&
                    c.value("version", 1) <
                        session.authoring().schemas().schema(type).value("version", 1)) {
                    const auto version =
                        session.authoring().schemas().schema(type).value("version", 1);
                    label(body, "opaque-note-" + cid,
                          "Stored v" + std::to_string(c.value("version", 1)) + " / schema v" +
                              std::to_string(version) + ". Migration is explicit and undoable.");
                    const auto action_id = "component-migrate-" + cid;
                    auto& action = button(body, action_id,
                                          source_object && !local_addition
                                              ? "Open source to migrate"
                                              : "Migrate to v" + std::to_string(version),
                                          [this, cid] { migrate_component(cid); });
                    action.tooltip =
                        "Apply the schema's declared migration rules. Errors keep all stored data.";
                    keep_in_order(action_id);
                }
                auto& raw =
                    body.add(Kind::TextField, "opaque-fields-" + cid, c.at("fields").dump());
                raw.enabled = false;
                keep_in_order(raw.id);
                button(body, "opaque-copy-" + cid, "Copy raw fields",
                       [this, fields = c.at("fields")] { renderer.set_clipboard(fields.dump(2)); });
                keep_in_order("opaque-copy-" + cid);
                continue;
            }
            for (const auto& [fid, value] : c.at("fields").items()) {
                const auto descriptor = metadata.at("fields").value(fid, Json::object());
                const auto key = "field-" + cid + "-" + fid;
                auto& row = body.add(Kind::Row, key);
                keep_in_order(key);
                const bool radians = descriptor.value("unit", std::string()) == "radians";
                const auto& options = descriptor.value("enum", Json::array());
                const bool vector_value = value.is_array() && value.size() >= 2 &&
                                          value.size() <= 4 &&
                                          std::all_of(value.begin(), value.end(),
                                                      [](const Json& v) { return v.is_number(); });
                const float label_width = radians ? 108.f : 90.f;
                const float field_width = vector_value
                                              ? float(value.size()) * 44.f +
                                                    float(value.size() - 1) * 3.f
                                              : 72.f;
                const float available_width =
                    ui.find("inspector_panel")->layout.width - 2.f * body.layout.padding;
                const bool stacked =
                    available_width < label_width + 4.f + field_width +
                                          (source_object ? 61.f : 0.f);
                row.layout.stack_vertical = stacked;
                row.layout.height = stacked ? (source_object ? 72.f : 48.f) : 28.f;
                row.layout.gap = stacked ? 2.f : 4.f;
                label(row, key + "-label",
                      descriptor.value("name", fid) + (radians ? " (rad)" : ""),
                      stacked ? -1.f : label_width);
                auto& field_label = *row.find(key + "-label");
                field_label.layout.height = stacked ? 18.f : -1.f;
                field_label.enabled = false;
                field_label.tooltip = descriptor.value("name", fid);
                const auto input_id = key + (vector_value ? "-vector" : "-value");
                const auto input_kind = !options.empty()     ? Kind::Button
                                        : vector_value       ? Kind::Row
                                        : value.is_boolean() ? Kind::Checkbox
                                        : value.is_number()  ? Kind::NumberField
                                                             : Kind::TextField;
                if (auto* old = row.find(input_id); old && old->kind != input_kind) {
                    if (old->find(ui.focused_id()))
                        ui.clear_focus(false);
                    row.remove(input_id);
                }
                if (source_object) {
                    const auto address = relative_address(*e, cid, fid);
                    const auto instance = e->at("origin").at("path").front();
                    const bool local_override = overridden(*e, cid, fid);
                    row.find(key + "-label")->text += local_override ? " · Override" : " · Source";
                    auto& revert =
                        button(row, key + "-revert", "Revert", [this, instance, address] {
                            transaction(Json::array({{{"op", "template.revert"},
                                                      {"instance", instance},
                                                      {"address", address}}}));
                        });
                    revert.enabled = local_override;
                    revert.appearance = ui::Appearance::Quiet;
                    revert.layout.width = stacked ? -1.f : 57.f;
                    revert.layout.height = stacked ? 22.f : -1.f;
                }
                trim_children(row, source_object ? std::set<std::string>{key + "-label", input_id,
                                                                         key + "-revert"}
                                                 : std::set<std::string>{key + "-label", input_id});
                if (!options.empty()) {
                    auto& input =
                        button(row, key + "-value",
                               value.is_string() ? value.get<std::string>() : value.dump(),
                               [this, cid, fid, value, options, key] {
                                   auto found = std::find(options.begin(), options.end(), value);
                                   auto index = found == options.end()
                                                    ? 0
                                                    : (std::size_t(found - options.begin()) + 1) %
                                                          options.size();
                                   set_field(cid, fid, options[index], key + "-value");
                               });
                    input.layout.flex = 1;
                } else if (value.is_boolean()) {
                    auto& input = row.add(Kind::Checkbox, key + "-value", "Enabled");
                    input.layout.flex = 1;
                    input.checked = value;
                    bind_field(input, cid, fid, value);
                } else if (vector_value) {
                    auto& vectorrow = row.add(Kind::Row, key + "-vector");
                    vectorrow.layout.gap = 3;
                    vectorrow.layout.flex = 1;
                    std::set<std::string> axis_ids;
                    for (std::size_t axis = 0; axis < value.size(); ++axis) {
                        auto& input =
                            vectorrow.add(Kind::NumberField, key + "-" + std::to_string(axis));
                        input.layout.flex = 1;
                        input.layout.min_width = 35;
                        ui.update_number(input.id, value[axis].get<double>());
                        input.step = .02;
                        bind_field(input, cid, fid, value, int(axis));
                        axis_ids.insert(input.id);
                    }
                    trim_children(vectorrow, axis_ids);
                } else if (value.is_number()) {
                    auto& input = row.add(Kind::NumberField, key + "-value");
                    input.layout.flex = 1;
                    input.step = value.is_number_integer() ? 1 : .05;
                    input.precision = value.is_number_integer() ? 0 : 3;
                    ui.update_number(input.id, value.get<double>());
                    bind_field(input, cid, fid, value);
                } else {
                    auto& input = row.add(Kind::TextField, key + "-value");
                    input.layout.flex = 1;
                    ui.update_text(input.id,
                                   value.is_string() ? value.get<std::string>() : value.dump());
                    bind_field(input, cid, fid, value);
                }
            }
        }
        auto& add = button(body, "add-component", "+ Add Component",
                           [this] { component_menu = !component_menu; });
        add.enabled = !source_object || local_addition;
        keep_in_order(add.id);
        if (source_object) {
            const auto path = e->at("origin").at("path");
            const auto object = e->at("origin").at("object").get<std::string>();
            auto& child = button(body, "instance-add-child", "Add local child",
                                 [this, path, object] { add_instance_child(path, object); });
            child.enabled = path.size() == 1;
            keep_in_order(child.id);
        }
        if (component_menu && (!source_object || local_addition))
            for (const auto& schema : schemas.at("types")) {
                const auto type = schema.at("id").get<std::string>();
                auto& choice = button(body, "component-choice-" + type, schema.value("name", type),
                                      [this, type] { add_component(type); });
                keep_in_order(choice.id);
            }
        trim_children(body, keep);
        std::map<std::string, std::size_t> position;
        for (std::size_t i = 0; i < ordered.size(); ++i)
            position[ordered[i]] = i;
        std::stable_sort(body.children.begin(), body.children.end(), [&](const auto& a, const auto& b) {
            return position.at(a->id) < position.at(b->id);
        });
    }
    void refresh_assets() {
        const auto now = std::chrono::steady_clock::now();
        if (!assets_dirty && now - last_assets < std::chrono::seconds(1))
            return;
        assets_dirty = false;
        last_assets = now;
        files = Json::array();
        try {
            for (const std::string folder : {"Assets", "Scenes", "Scripts"}) {
                const auto directory = session.config().project_root / folder;
                if (!std::filesystem::exists(directory))
                    continue;
                std::size_t count = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         directory, std::filesystem::directory_options::skip_permission_denied)) {
                    if (++count > 4000)
                        break;
                    if (!entry.is_regular_file())
                        continue;
                    if (folder == "Scripts" && entry.path().extension() != ".lua")
                        continue;
                    auto relative = generic_path_to_utf8(
                        std::filesystem::relative(entry.path(), session.config().project_root));
                    if (relative.find(".faset-") != std::string::npos)
                        continue;
                    if (!asset_filter.empty() && relative.find(asset_filter) == std::string::npos)
                        continue;
                    files.push_back(relative);
                }
            }
        } catch (const std::exception& e) {
            report(std::string("Asset scan: ") + e.what());
        }
        auto result = call("faset_assets");
        if (!result.is_null()) {
            if (assets != result.at("assets")) {
                view.clearCache();
                picks_key.clear();
            }
            assets = result.at("assets");
        }
        auto& list = *ui.find("asset-items");
        const bool script = path_from_utf8(source_file).extension() == ".lua";
        ui.find("asset-open")->text = script ? "Open Script" : "Open Scene";
        ui.find("asset-open")->tooltip =
            script ? "Open Lua source in your external editor" : "Open a project scene";
        ui.find("asset-import")->enabled = !script;
        std::set<std::string> keep;
        for (const auto& file : files) {
            const auto path = file.get<std::string>();
            const auto file_path = path_from_utf8(path);
            auto name = path_to_utf8(file_path.filename());
            const auto stem = path_to_utf8(file_path.stem());
            if (stem.size() > 24 &&
                std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                           (c >= 'A' && c <= 'F');
                }))
                name = stem.substr(0, 10) + "…" + stem.substr(stem.size() - 6) +
                       path_to_utf8(file_path.extension());
            const auto folder = generic_path_to_utf8(file_path.parent_path());
            auto& row = list.add(Kind::TreeRow, "file-" + path, name + "   /   " + folder);
            row.layout.height = 25;
            row.indent = 1;
            row.tooltip = path;
            row.selected = source_file == path;
            row.on_click = [this, path](Widget&) {
                source_file = path;
                assets_dirty = true;
            };
            keep.insert(row.id);
        }
        for (const auto& asset : assets) {
            const auto id = asset.at("id").get<std::string>();
            const auto name =
                asset.contains("manifest")
                    ? path_to_utf8(path_from_utf8(asset["manifest"].value("source", id)).filename())
                    : id;
            const auto freshness = asset.value("freshness", Json::object());
            const auto state = freshness.value("state", std::string("unavailable"));
            const auto prefix = state == "current" ? "Imported"
                                : state == "stale" ? "Stale"
                                                   : "Unavailable";
            auto& row =
                list.add(Kind::TreeRow, "asset-" + id, std::string(prefix) + "   /   " + name);
            row.layout.height = 25;
            row.indent = 1;
            row.tooltip = "Asset " + id + " — " + state;
            row.drag_payload = {{"kind", "asset"}, {"id", id}, {"label", name}};
            row.on_click = [this, id, asset, freshness, state](Widget&) {
                renderer.set_clipboard(id);
                if (asset.contains("manifest")) {
                    source_file = generic_path_to_utf8(std::filesystem::relative(
                        path_from_utf8(asset.at("manifest").at("source").get<std::string>()),
                        session.config().project_root));
                    assets_dirty = true;
                }
                status = state == "current"
                             ? "Asset ID copied; drag to viewport or an asset field"
                             : "Reimport required; select Import to refresh the selected source";
                for (const auto& reason : freshness.value("reasons", Json::array()))
                    report(reason.value("message", "Input changed") + ": " +
                           reason.value("path", ""));
            };
            keep.insert(row.id);
        }
        if (keep.empty()) {
            label(list, "assets-empty",
                  "Import images/models from Assets, or open Lua sources from Scripts.");
            keep.insert("assets-empty");
        }
        trim_children(list, keep);
    }
    void refresh_bottom() {
        const auto jobs_response = call("faset_jobs");
        const auto all_jobs = jobs_response.is_null() ? Json::array() : jobs_response.at("jobs");
        for (const auto& panel : session.plugin_panels()) {
            const auto panel_id = panel.at("id").get<std::string>();
            const auto dock_id = "plugin-" + panel_id;
            auto order = dock.panels("bottom");
            if (std::find(order.begin(), order.end(), dock_id) == order.end())
                dock.move(dock_id, "bottom", order.size());
            auto& tab = ui.find("bottom-tabs")->add(Kind::Tab, "tab-" + dock_id, panel.at("title"));
            tab.layout.width = 150;
            tab.dock_area = "bottom";
            tab.dock_panel = dock_id;
            tab.selected = active_bottom == dock_id;
            tab.on_click = [this, dock_id](Widget&) { active_bottom = dock_id; };
            auto& body = ui.find("bottom_panel")->add(Kind::Column, "plugin-panel-" + panel_id);
            body.layout.flex = 1;
            body.layout.padding = 10;
            body.layout.gap = 6;
            body.visible = active_bottom == dock_id;
            label(body, "plugin-title-" + panel_id, panel.at("title"));
            button(body, "plugin-action-" + panel_id, panel.value("action", std::string("Run")),
                   [this, panel] {
                       Json arguments = panel.value("arguments", Json::object());
                       std::function<void(Json&)> resolve = [&](Json& value) {
                           if (value.is_string() && value == "$document")
                               value = document;
                           else if (value.is_structured())
                               for (auto& item : value)
                                   resolve(item);
                       };
                       resolve(arguments);
                       auto result = call(panel.at("command"), arguments);
                       if (!result.is_null()) {
                           status = "Completed " + panel.at("title").get<std::string>();
                           shown_revision = std::numeric_limits<std::uint64_t>::max();
                       }
                   });
        }
        const auto order = dock.panels("bottom");
        auto& tabs = *ui.find("bottom-tabs");
        std::stable_sort(tabs.children.begin(), tabs.children.end(),
                         [&](const auto& a, const auto& b) {
                             return std::find(order.begin(), order.end(), a->dock_panel) <
                                    std::find(order.begin(), order.end(), b->dock_panel);
                         });
        for (const std::string id : {"assets", "console", "jobs", "conflicts"})
            ui.find("tab-" + id)->selected = active_bottom == id;
        ui.find("asset-toolbar")->visible = active_bottom == "assets";
        ui.find("asset-items")->visible = active_bottom == "assets";
        ui.find("console-items")->visible = active_bottom == "console";
        ui.find("job-items")->visible = active_bottom == "jobs";
        ui.find("conflict-items")->visible = active_bottom == "conflicts";
        auto& conflict_list = *ui.find("conflict-items");
        std::set<std::string> conflict_keep;
        std::size_t conflict_index = 0;
        for (const auto& conflict : template_conflicts) {
            const auto id = "conflict-" + std::to_string(conflict_index++);
            auto& row = conflict_list.add(Kind::Row, id);
            row.layout.height = 28;
            const auto code = conflict.at("code").get<std::string>();
            const auto path = conflict.at("instance_path");
            auto& text = row.add(
                Kind::Label, id + "-text",
                code + " · " + path_to_utf8(path_from_utf8(instance_source(path)).filename()));
            text.layout.flex = 1;
            button(
                row, id + "-source", "Open source", [this, path] { open_template_source(path); },
                106);
            const auto record = conflict.at("record");
            const auto change = record.contains("change") ? record.at("change") : record;
            if (path.size() == 1 && code.starts_with("override.") && change.contains("address")) {
                const auto address = change.at("address");
                button(
                    row, id + "-discard", "Discard override",
                    [this, path, address] {
                        transaction(Json::array({{{"op", "template.revert"},
                                                  {"instance", path.front()},
                                                  {"address", address}}}));
                    },
                    140);
            } else if (path.size() == 1 && code == "suppression.object_missing") {
                button(
                    row, id + "-discard", "Discard suppression",
                    [this, path, record] {
                        transaction(Json::array({{{"op", "template.restore"},
                                                  {"instance", path.front()},
                                                  {"address", record}}}));
                    },
                    155);
            }
            conflict_keep.insert(id);
        }
        std::size_t import_conflicts = 0;
        for (const auto& job : all_jobs) {
            if (job.value("kind", std::string()) != "import" ||
                job.value("state", std::string()) != "conflict")
                continue;
            const auto job_id = job.at("id").get<std::string>();
            const auto& result = job.at("result");
            const auto request = job.value("request", Json::object());
            bool pending = false, superseded = false;
            std::string retry_error;
            if (const auto found = import_retries.find(job_id); found != import_retries.end())
                for (const auto& retry : all_jobs)
                    if (retry.at("id") == found->second) {
                        const auto state = retry.value("state", std::string());
                        pending = state == "queued" || state == "running";
                        superseded = state == "succeeded" || state == "conflict";
                        if (state == "failed" || state == "cancelled")
                            retry_error = retry.value("error", std::string("Retry cancelled"));
                    }
            for (const auto& asset : assets)
                if (asset.at("id") == result.value("asset_id", std::string()) &&
                    asset.contains("manifest") &&
                    asset.at("manifest").at("generation") ==
                        result.value("generation", std::string()))
                    superseded = true;
            if (superseded)
                continue;
            ++import_conflicts;
            const auto id = "import-conflict-" + job_id;
            auto& group = conflict_list.add(Kind::Column, id);
            group.layout.padding = 6;
            group.layout.gap = 3;
            conflict_keep.insert(id);
            label(group, id + "-title", "Import removal: " + request.value("path", job_id));
            label(group, id + "-note",
                  "The previous asset stays active. Update affected references before accepting.");
            label(group, id + "-generation",
                  "Reviewed generation: " + result.value("generation", std::string()));
            auto diagnostic = job.value("error", std::string());
            std::replace(diagnostic.begin(), diagnostic.end(), '\n', ' ');
            label(group, id + "-diagnostic", diagnostic);
            const auto removed = result.value("removed_output_ids", Json::array());
            auto& outputs = group.add(Kind::Column, id + "-outputs");
            outputs.layout.height = std::min(150.f, std::max(26.f, float(removed.size()) * 26));
            outputs.layout.scroll = true;
            outputs.layout.gap = 0;
            std::set<std::string> output_keep;
            for (const auto& output : removed) {
                const auto output_id = output.get<std::string>();
                std::string name;
                for (const auto& asset : assets)
                    if (asset.at("id") == result.value("asset_id", std::string()) &&
                        asset.contains("manifest"))
                        for (const auto* collection : {"nodes", "meshes", "materials", "textures"})
                            for (const auto& item :
                                 asset.at("manifest").value(collection, Json::array()))
                                if (item.at("id") == output_id)
                                    name = item.value("name", std::string());
                const auto output_widget = id + "-output-" + output_id;
                label(outputs, output_widget, output_id + (name.empty() ? "" : "  /  " + name));
                outputs.find(output_widget)->layout.height = 26;
                output_keep.insert(output_widget);
            }
            trim_children(outputs, output_keep);
            auto& actions = group.add(Kind::Row, id + "-actions");
            actions.layout.height = 30;
            const auto retry = [this, job_id, request, result](bool accept) {
                Json arguments = {{"path", request.at("path")}, {"allow_removed_outputs", accept}};
                if (request.contains("settings") && !request.at("settings").is_null())
                    arguments["settings"] = request.at("settings");
                if (accept) {
                    arguments["expected_generation"] = result.at("generation");
                    arguments["expected_active_generation"] = result.at("previous_generation");
                }
                auto submitted = call("faset_import", arguments);
                if (!submitted.is_null()) {
                    import_retries[job_id] = submitted.at("job");
                    status =
                        accept
                            ? "Publishing the reviewed import; source changes require a new review"
                            : "Reimporting source for a fresh review";
                }
            };
            button(
                actions, id + "-retry", "Reimport / review again", [retry] { retry(false); }, 185)
                .enabled = !pending && request.contains("path");
            button(
                actions, id + "-accept", "Accept reviewed removal", [retry] { retry(true); }, 190)
                .enabled = !pending && request.contains("path") && !removed.empty();
            button(
                actions, id + "-copy", "Copy removed IDs",
                [this, removed] { renderer.set_clipboard(removed.dump(2)); }, 145);
            auto detail = pending ? "Import in progress; the previous generation remains active."
                                  : "Accepting removes these outputs. Scene references are not "
                                    "remapped automatically.";
            label(group, id + "-detail", detail);
            if (!retry_error.empty()) {
                std::replace(retry_error.begin(), retry_error.end(), '\n', ' ');
                label(group, id + "-error", retry_error);
            } else
                group.remove(id + "-error");
        }
        ui.find("tab-conflicts")->text =
            "Conflicts (" + std::to_string(template_conflicts.size() + import_conflicts) + ")";
        if (conflict_keep.empty()) {
            label(conflict_list, "conflicts-empty",
                  "No template or import conflicts. Missing targets preserve their data here.");
            conflict_keep.insert("conflicts-empty");
        }
        trim_children(conflict_list, conflict_keep);
        auto& console = *ui.find("console-items");
        std::set<std::string> keep;
        std::size_t shown_diagnostics{};
        for (const auto& job : all_jobs) {
            const auto id = job.at("id").get<std::string>();
            const auto diagnostics = job.value("diagnostics", Json::array());
            if (!diagnostics.is_array())
                continue;
            for (std::size_t index = 0; index < diagnostics.size() && shown_diagnostics < 60;
                 ++index, ++shown_diagnostics) {
                const auto& diagnostic = diagnostics[index];
                const auto row_id = "diagnostic-" + id + "-" + std::to_string(index);
                auto& row = console.add(Kind::Row, row_id);
                row.layout.height = 27;
                row.layout.gap = 8;
                keep.insert(row_id);
                const auto severity = diagnostic.value("severity", std::string("error"));
                label(row, row_id + "-severity", severity, 68);
                auto message = diagnostic.value("message", std::string());
                auto& detail = row.add(Kind::Label, row_id + "-message", message);
                detail.layout.flex = 1;
                detail.tooltip = diagnostic.value("phase", std::string()) + ": " + message;
                const auto file = diagnostic.value("file", std::string());
                const auto line = diagnostic.value("line", 1);
                const auto column = diagnostic.value("column", 1);
                const auto location = file.empty()
                                          ? std::string("—")
                                          : file + ":" + std::to_string(line) + ":" +
                                                std::to_string(column);
                label(row, row_id + "-location", location, 235);
                auto& open = button(
                    row, row_id + "-open", "Open source",
                    [this, file, line, column] {
                        call("faset_source_open",
                             {{"path", file}, {"line", line}, {"column", column}});
                    },
                    115);
                open.enabled = !file.empty();
                open.appearance = ui::Appearance::Quiet;
            }
        }
        const auto& logs = session.logs();
        for (std::size_t i = logs.size() > 150 ? logs.size() - 150 : 0; i < logs.size(); ++i) {
            std::string line = logs[i];
            std::replace(line.begin(), line.end(), '\n', ' ');
            const auto id = "log-" + std::to_string(i);
            label(console, id, line);
            console.children.back()->layout.height = 24;
            keep.insert(id);
        }
        if (keep.empty()) {
            label(console, "log-empty", "No editor messages.");
            keep.insert("log-empty");
        }
        trim_children(console, keep);
        auto& jobs = *ui.find("job-items");
        keep.clear();
        for (const auto& job : all_jobs) {
            const auto id = job.at("id").get<std::string>();
            auto& row = jobs.add(Kind::Row, "job-" + id);
            row.layout.height = 28;
            keep.insert(row.id);
            const auto state = job.value("state", std::string());
            const auto result = job.value("result", Json::object());
            std::string reuse;
            if (result.is_object() && result.value("schema_cache_hit", false))
                reuse = " · verified reuse";
            if (result.is_object() && result.contains("phase_times_ms") &&
                result.at("phase_times_ms").is_object())
                reuse += " · " +
                         std::to_string(result.at("phase_times_ms").value("total", 0)) + " ms";
            auto& text = row.add(Kind::Label, "job-text-" + id,
                                 job.value("kind", std::string("Job")) + " · " + state + " · " +
                                     job.value("stage", std::string()) + " " +
                                     std::to_string(int(job.value("progress", 0.0) * 100)) + "%" +
                                     reuse);
            text.layout.flex = 1;
            auto& log_button = button(
                row, "job-log-toggle-" + id,
                expanded_job_log == id ? "Hide log" : "View log",
                [this, id] { expanded_job_log = expanded_job_log == id ? "" : id; }, 86);
            log_button.appearance = ui::Appearance::Quiet;
            auto& cancel = button(
                row, "job-cancel-" + id, "Cancel",
                [this, id] { call("faset_job_cancel", {{"id", id}}); }, 72);
            cancel.enabled = state == "queued" || state == "running";
            auto& review = button(
                row, "job-review-" + id, "Review removals", [this] { active_bottom = "conflicts"; },
                135);
            review.visible = job.value("kind", std::string()) == "import" && state == "conflict";
            if (expanded_job_log == id) {
                const auto panel_id = "job-log-panel-" + id;
                auto& panel = jobs.add(Kind::Column, panel_id);
                panel.layout.height = 170;
                panel.layout.scroll = true;
                panel.layout.gap = 1;
                keep.insert(panel_id);
                std::set<std::string> log_keep;
                const auto raw = job.value("log", std::string());
                auto& copy = button(panel, panel_id + "-copy", "Copy full log",
                                    [this, raw] { renderer.set_clipboard(raw); }, 110);
                copy.appearance = ui::Appearance::Quiet;
                log_keep.insert(copy.id);
                std::vector<std::string> lines;
                std::size_t begin{};
                while (begin < raw.size()) {
                    const auto end = raw.find('\n', begin);
                    lines.push_back(raw.substr(begin, end == std::string::npos
                                                         ? std::string::npos
                                                         : end - begin));
                    if (end == std::string::npos)
                        break;
                    begin = end + 1;
                }
                const auto first = lines.size() > 100 ? lines.size() - 100 : 0;
                for (std::size_t index = first; index < lines.size(); ++index) {
                    const auto line_id = panel_id + "-line-" + std::to_string(index);
                    label(panel, line_id, lines[index].substr(0, 600));
                    log_keep.insert(line_id);
                }
                trim_children(panel, log_keep);
            }
        }
        if (keep.empty()) {
            label(jobs, "jobs-empty", "No active import, build or export jobs.");
            keep.insert("jobs-empty");
        }
        trim_children(jobs, keep);
    }
    void refresh_overlays() {
        auto& popup = *ui.find("menu-popup");
        popup.visible = !menu.empty();
        ui.find("menu-title")->text = menu;
        refresh_recovery();
        refresh_simulation();
        refresh_project_settings();
        const bool modal = palette || !recovery.empty() || simulation_open ||
                           project_switch_warning || project_settings_open;
        for (const auto* id : {"menubar", "toolbar", "workspace", "bottom_panel"})
            ui.find(id)->enabled = !modal;
        const bool file = menu == "File" || menu == "Faset",
                   edit = menu == "Edit" || menu == "Scene",
                   help = menu == "Help" || menu == "View";
        for (const auto* id : {"new-3d", "new-2d", "project-settings", "open-project", "open-path",
                               "open-path-button", "save-path", "save-as-button"})
            ui.find(id)->visible = file;
        ui.find("open-project")->enabled = project_switch_enabled;
        ui.find("open-project")->tooltip =
            project_switch_enabled ? "Return to project launcher"
                                   : "Project switching is unavailable while MCP is connected";
        auto* switching = ui.find("project-switch-dialog");
        switching->visible = project_switch_warning;
        switching->layout.x = std::max(0.f, (logical_width() - 500) * .5f);
        switching->layout.y = std::max(0.f, (logical_height() - 220) * .5f);
        for (const auto* id : {"menu-duplicate", "menu-delete"})
            ui.find(id)->visible = edit;
        for (const auto* id : {"help-one", "help-two", "help-three"})
            ui.find(id)->visible = help;
        for (const auto* id : {"template-path", "save-template", "instance-template"})
            ui.find(id)->visible = menu == "Scene";
        popup.layout.height = file ? 335 : menu == "Scene" ? 220 : edit ? 112 : 150;
        popup.layout.x = menu == "File"    ? 70
                         : menu == "Edit"  ? 126
                         : menu == "Scene" ? 182
                         : menu == "View"  ? 238
                         : menu == "Help"  ? 294
                                           : 5;
        auto& command = *ui.find("palette");
        command.visible = palette;
        command.layout.x = std::max(0.f, (logical_width() - 570) / 2);
        command.layout.y = 90;
        auto& list = *ui.find("palette-list");
        std::set<std::string> keep;
        const auto filter = ui.find("palette-filter")->text;
        for (const auto& descriptor : session.commands().list()) {
            const auto name = descriptor.at("name").get<std::string>();
            if (!filter.empty() && name.find(filter) == std::string::npos)
                continue;
            auto& row = list.add(Kind::TreeRow, "command-" + name, name);
            row.selected = command_name == name;
            row.on_click = [this, name](Widget&) { command_name = name; };
            keep.insert(row.id);
        }
        trim_children(list, keep);
    }
    void open_project_settings() {
        ui.clear_focus(false);
        auto result = call("faset_project_settings_get");
        if (result.is_null())
            return;
        project_settings_state = result;
        const auto& settings = result.at("settings");
        project_settings_dimension = settings.value("dimension", 3);
        ui.update_text("project-settings-name", settings.value("name", std::string()), true);
        ui.update_text("project-settings-start", settings.value("start_scene", std::string()),
                       true);
        project_settings_error.clear();
        project_settings_open = true;
        menu.clear();
        auto& choices = *ui.find("project-settings-scenes");
        choices.children.clear();
        choices.scroll_y = 0;
        std::set<std::string> saved;
        for (const auto& doc : session.authoring().documents()) {
            const auto path = doc.value("path", std::string());
            if (!path.empty())
                saved.insert(path);
        }
        try {
            for (const auto* folder : {"Scenes", "Assets"}) {
                const auto directory = session.config().project_root / folder;
                if (!std::filesystem::is_directory(directory))
                    continue;
                std::size_t visited = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         directory, std::filesystem::directory_options::skip_permission_denied)) {
                    if (++visited > 4000)
                        break;
                    if (entry.is_regular_file() &&
                        path_to_utf8(entry.path().filename()).ends_with(".scene.json"))
                        saved.insert(generic_path_to_utf8(std::filesystem::relative(
                            entry.path(), session.config().project_root)));
                }
            }
        } catch (const std::exception& error) {
            project_settings_error = error.what();
        }
        std::size_t index = 0;
        for (const auto& path : saved) {
            try {
                const auto scene =
                    read_json(project_path(session.config().project_root, path_from_utf8(path)));
                if (scene.value("format", "") != "faset.scene" || scene.value("version", 0) != 1)
                    continue;
            } catch (...) {
                continue;
            }
            auto& choice =
                choices.add(Kind::TreeRow, "project-scene-choice-" + std::to_string(index++), path);
            choice.layout.height = 27;
            choice.on_click = [this, path](Widget&) {
                ui.update_text("project-settings-start", path, true);
                project_settings_error.clear();
            };
        }
        if (index == 0)
            choices.add(Kind::Label, "project-settings-no-scenes",
                        "Save a scene to choose it here.");
    }
    void save_project_settings() {
        ui.clear_focus();
        if (ui.editing())
            return;
        const auto name = ui.find("project-settings-name")->text;
        const auto start = ui.find("project-settings-start")->text;
        if (name.find_first_not_of(" \t\r\n") == std::string::npos) {
            project_settings_error = "Enter a project name.";
            return;
        }
        Json changes = {{"name", name}, {"dimension", project_settings_dimension}};
        if (!start.empty() ||
            !project_settings_state.at("settings").value("start_scene", std::string()).empty())
            changes["start_scene"] = start;
        const auto result =
            call("faset_project_settings_set",
                 {{"revision", project_settings_state.at("revision")}, {"settings", changes}});
        if (result.is_null()) {
            project_settings_error = status;
            return;
        }
        project_settings_state = result;
        project_settings_open = false;
        project_settings_error.clear();
        status = "Project settings saved for the next project open";
    }
    void refresh_project_settings() {
        auto* panel = ui.find("project-settings-panel");
        panel->visible = project_settings_open;
        panel->layout.width = std::min(610.f, std::max(340.f, logical_width() - 40));
        panel->layout.height = std::min(550.f, std::max(300.f, logical_height() - 40));
        panel->layout.x = std::max(0.f, (logical_width() - panel->layout.width) * .5f);
        panel->layout.y = std::max(0.f, (logical_height() - panel->layout.height) * .5f);
        ui.find("project-settings-2d")->selected = project_settings_dimension == 2;
        ui.find("project-settings-3d")->selected = project_settings_dimension == 3;
        ui.find("project-settings-error")->text = project_settings_error;
        for (auto& choice : ui.find("project-settings-scenes")->children)
            if (choice->kind == Kind::TreeRow)
                choice->selected = choice->text == ui.find("project-settings-start")->text;
    }
    void refresh_simulation() {
        auto& panel = *ui.find("simulation-panel");
        panel.visible = simulation_open;
        panel.layout.x = std::max(0.f, (logical_width() - 470) / 2);
        panel.layout.y = 90;
        if (!simulation_open)
            return;
        const auto response = call("faset_simulation_get", {{"document", document}});
        if (response.is_null())
            return;
        const auto settings = response.at("settings");
        auto bind = [&, this](Widget& input, const std::string& field, int axis = -1) {
            const auto id = input.id;
            input.on_preview = [this, id](Widget&) {
                edit_revisions.try_emplace(id, current.at("revision").get<std::uint64_t>());
            };
            input.on_cancel = [this, id](Widget&) { edit_revisions.erase(id); };
            input.on_commit = [this, id, field, axis, settings](Widget& w) {
                Json patch;
                if (axis >= 0) {
                    auto gravity = settings.at("gravity");
                    gravity[axis] = w.value;
                    patch = {{"gravity", gravity}};
                } else if (field == "tick-rate") {
                    if (w.value <= 0) {
                        report("Tick rate must be positive");
                        edit_revisions.erase(id);
                        return;
                    }
                    patch = {{"fixed_delta", 1.0 / w.value}};
                } else
                    patch = {{field, std::int64_t(std::llround(w.value))}};
                const auto revision = edit_revisions.contains(id)
                                          ? edit_revisions[id]
                                          : current.at("revision").get<std::uint64_t>();
                const auto result =
                    call("faset_simulation_set",
                         {{"document", document}, {"revision", revision}, {"settings", patch}});
                edit_revisions.erase(id);
                if (!result.is_null()) {
                    current = result;
                    status = "Scene simulation updated for the next Play";
                }
            };
        };
        for (const std::string field : {"tick-rate", "max_catch_up_ticks", "physics_substeps"}) {
            auto& input = *ui.find("simulation-" + field);
            ui.update_number(input.id, field == "tick-rate"
                                           ? 1.0 / settings.at("fixed_delta").get<double>()
                                           : settings.at(field).get<double>());
            bind(input, field);
        }
        for (int axis = 0; axis < 3; ++axis) {
            auto& input = *ui.find("simulation-gravity-" + std::to_string(axis));
            ui.update_number(input.id, settings.at("gravity")[axis].get<double>());
            bind(input, "gravity", axis);
        }
    }
    void refresh_recovery() {
        auto& panel = *ui.find("recovery-panel");
        panel.visible = !recovery.empty();
        panel.layout.x = std::max(0.f, (logical_width() - 470) / 2);
        panel.layout.y = 100;
        std::set<std::string> keep;
        label(panel, "recovery-title", "Unsaved authoring recovery");
        keep.insert("recovery-title");
        for (const auto& item : recovery) {
            const auto id = item.at("id").get<std::string>();
            auto& restore =
                button(panel, "recover-" + id, "Restore " + item.value("name", id), [this, id] {
                    Json args = {{"document", id}};
                    for (const auto& doc : session.authoring().documents())
                        if (doc.at("id") == id)
                            args["revision"] = doc.at("revision");
                    auto result = call("faset_recovery_restore", args);
                    if (!result.is_null()) {
                        choose_document(result.at("id"));
                        recovery.erase(
                            std::remove_if(recovery.begin(), recovery.end(),
                                           [&](const Json& entry) { return entry.at("id") == id; }),
                            recovery.end());
                    }
                });
            keep.insert(restore.id);
        }
        button(panel, "recovery-dismiss", "Continue without restoring",
               [this] { recovery.clear(); });
        keep.insert("recovery-dismiss");
        trim_children(panel, keep);
        panel.layout.height = 80 + float(recovery.size()) * 33;
    }
    player::CameraSettings camera() const {
        player::CameraSettings c;
        c.overrideSceneCamera = true;
        c.target = target;
        c.eye = add(target, {distance * std::cos(pitch) * std::sin(yaw), distance * std::sin(pitch),
                             distance * std::cos(pitch) * std::cos(yaw)});
        c.orthographicHeight = ortho;
        if (current.at("scene").value("dimension", 3) == 2)
            c.eye = add(target, {0, 0, distance});
        return c;
    }
    void frame_selection() {
        if (auto* e = entity(resolved, selected))
            target = point(world(resolved, *e), {});
        else
            target = {};
        distance = 8;
        ortho = 8;
    }
    bool project(Vec3 p, render::Vec2& output) const {
        const auto& m = rendered.view_projection;
        const auto w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (w <= .0001f)
            return false;
        const auto x = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w,
                   y = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w;
        output = {viewport.x + (x + 1) * viewport.width * .5f,
                  viewport.y + (y + 1) * viewport.height * .5f};
        return true;
    }
    void line(render::Vec2 a, render::Vec2 b, render::Color color, float thickness = 1) {
        const auto length = std::hypot(b[0] - a[0], b[1] - a[1]);
        const auto count = std::min(700, std::max(1, int(length / thickness)));
        for (int i = 0; i <= count; ++i) {
            const auto t = float(i) / count;
            ui::Rect q{a[0] + (b[0] - a[0]) * t - thickness * .5f,
                       a[1] + (b[1] - a[1]) * t - thickness * .5f, thickness + 1, thickness + 1};
            q = q.intersection(viewport);
            if (q.width > 0 && q.height > 0)
                rendered.ui_quads.push_back({q.x, q.y, q.width, q.height, color, {}, {0, 0, 1, 1}});
        }
    }
    void build_snapshot() {
        auto scene = resolved;
        for (auto& e : scene["entities"])
            for (auto& c : e["components"])
                for (auto& [field, value] : c["fields"].items()) {
                    const auto key = c.at("id").get<std::string>() + "/" + field;
                    if (preview_fields.contains(key))
                        value = preview_fields[key];
                }
        viewport = ui.find("viewport")->rect;
        rendered = view.build(scene, viewport.width / std::max(1.f, viewport.height), camera());
        for (const auto& diagnostic : view.diagnostics())
            if (seen_view_diagnostics.insert(diagnostic).second)
                session.log("Viewport: " + diagnostic);
        rendered.scene_rect = {viewport.x, viewport.y, viewport.width, viewport.height};
        rendered.clear_color = ui.theme().background;
        const auto key = scene.dump() + assets.dump();
        if (key != picks_key) {
            picks_key = key;
            picks.clear();
            // Picking uses the same cooked meshes and transforms as the visible
            // snapshot.
            for (const auto& e : scene.at("entities")) {
                if (!component(e, "faset.mesh") && !component(e, "faset.sprite"))
                    continue;
                const auto id = e.at("id").get<std::string>();
                auto isolated = scene;
                for (auto& other : isolated["entities"])
                    if (other.at("id") != id) {
                        auto& cs = other["components"];
                        cs.erase(std::remove_if(cs.begin(), cs.end(),
                                                [](const Json& c) {
                                                    return c.at("type") == "faset.mesh" ||
                                                           c.at("type") == "faset.sprite";
                                                }),
                                 cs.end());
                    }
                auto object = view.build(isolated, 1, camera());
                for (auto& item : object.draws)
                    picks.push_back({id, std::move(item)});
                for (const auto& sprite : object.sprites) {
                    auto mesh = std::make_shared<render::Mesh>();
                    mesh->vertices = {
                        {{-.5f, -.5f, 0}}, {{.5f, -.5f, 0}}, {{.5f, .5f, 0}}, {{-.5f, .5f, 0}}};
                    mesh->indices = {0, 1, 2, 0, 2, 3};
                    render::DrawItem item;
                    item.mesh = mesh;
                    item.model = render::transform(sprite.position, {0, 0, sprite.rotation},
                                                   {sprite.size[0], sprite.size[1], 1});
                    picks.push_back({id, item});
                }
            }
        }
        // Grid geometry participates in scene depth, so it never draws through
        // objects. It is presentation only and is absent from authoring, picking
        // and export.
        const bool is2d = scene.value("dimension", 3) == 2;
        for (int i = -10; i <= 10; ++i) {
            if (is2d) {
                rendered.sprites.push_back(
                    {{float(i), 0, -.02f}, {.012f, 20}, {.18f, .18f, .19f, 1}, 0, {}});
                rendered.sprites.push_back(
                    {{0, float(i), -.02f}, {20, .012f}, {.18f, .18f, .19f, 1}, 0, {}});
            } else {
                render::DrawItem grid;
                grid.mesh = render::cube_mesh();
                grid.cast_shadow = false;
                grid.color = {.035f, .035f, .04f, 1};
                grid.model = render::transform({float(i), 0, 0}, {}, {.012f, .002f, 20});
                rendered.draws.push_back(grid);
                grid.model = render::transform({0, 0, float(i)}, {}, {20, .002f, .012f});
                rendered.draws.push_back(grid);
            }
        }
        gizmo_valid = false;
        if (auto* e = entity(scene, selected);
            e && component(*e, "faset.transform") &&
            component(*e, "faset.transform")->value("version", 1) == 1) {
            gizmo_origin = point(world(scene, *e), {});
            gizmo_valid = project(gizmo_origin, gizmo_screen[0]);
            const auto length = is2d ? ortho * .12f : distance * .12f;
            for (int axis = 0; axis < 3; ++axis) {
                Vec3 p = gizmo_origin;
                Vec3 direction{};
                direction[axis] = 1;
                if (gizmo_mode != "Move")
                    direction = normalize(vector(world(scene, *e), direction));
                p = add(p, mul(direction, length));
                gizmo_valid = project(p, gizmo_screen[axis + 1]) && gizmo_valid;
            }
            if (gizmo_valid) {
                const render::Color colors[3] = {
                    {.88f, .35f, .34f, 1}, {.38f, .75f, .49f, 1}, {.4f, .58f, .92f, 1}};
                for (int axis = 0; axis < (is2d ? 2 : 3); ++axis) {
                    line(gizmo_screen[0], gizmo_screen[axis + 1], colors[axis], 2 * ui_scale);
                    const auto end = gizmo_screen[axis + 1];
                    if (viewport.contains(end[0], end[1]))
                        rendered.ui_quads.push_back({end[0] - 4 * ui_scale,
                                                     end[1] - 4 * ui_scale,
                                                     8 * ui_scale,
                                                     8 * ui_scale,
                                                     colors[axis],
                                                     {},
                                                     {0, 0, 1, 1}});
                }
            }
        }
        ui.draw(rendered);
    }
    void pick(float x, float y) {
        Mat4 inv;
        if (!inverse(rendered.view_projection, inv))
            return;
        const auto nx = 2 * (x - viewport.x) / viewport.width - 1,
                   ny = 2 * (y - viewport.y) / viewport.height - 1;
        const auto origin = homogeneous(inv, nx, ny, 0), end = homogeneous(inv, nx, ny, 1),
                   direction = normalize(sub(end, origin));
        float distance_hit = std::numeric_limits<float>::infinity();
        std::string hit;
        for (const auto& candidate : picks) {
            const auto& mesh = *candidate.item.mesh;
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const auto a = point(candidate.item.model,
                                     mesh.vertices.at(mesh.indices[i]).position),
                           b = point(candidate.item.model,
                                     mesh.vertices.at(mesh.indices[i + 1]).position),
                           c = point(candidate.item.model,
                                     mesh.vertices.at(mesh.indices[i + 2]).position);
                if (ray_triangle(origin, direction, a, b, c, distance_hit))
                    hit = candidate.id;
            }
        }
        select(hit);
    }
    bool start_gizmo(float x, float y) {
        if (!gizmo_valid)
            return false;
        const auto* e = entity(resolved, selected);
        if (!e)
            return false;
        const auto* c = component(*e, "faset.transform");
        if (!c || c->value("version", 1) != 1)
            return false;
        float best = 8 * ui_scale;
        int axis = -1;
        for (int i = 0; i < (current.at("scene").value("dimension", 3) == 2 ? 2 : 3); ++i) {
            auto a = gizmo_screen[0], b = gizmo_screen[i + 1];
            const auto dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
            if (l < 16 * ui_scale * ui_scale)
                continue;
            const auto t = std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / l, 0.f, 1.f);
            const auto d = std::hypot(x - a[0] - t * dx, y - a[1] - t * dy);
            if (d < best) {
                best = d;
                axis = i;
            }
        }
        if (axis < 0)
            return false;
        gizmo_axis =
            current.at("scene").value("dimension", 3) == 2 && gizmo_mode == "Rotate" ? 2 : axis;
        gizmo_drag_start = gizmo_screen[0];
        gizmo_drag_end = gizmo_screen[axis + 1];
        gizmo_world_length =
            (current.at("scene").value("dimension", 3) == 2 ? ortho : distance) * .12f;
        gizmo_down_x = x;
        gizmo_down_y = y;
        gizmo_original = c->at("fields");
        gizmo_component = c->at("id");
        gizmo_revision = current.at("revision");
        return true;
    }
    void update_gizmo(float x, float y, bool snap) {
        if (gizmo_axis < 0)
            return;
        const auto a = gizmo_drag_start, b = gizmo_drag_end;
        const auto dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
        const auto amount = l > 1 ? ((x - gizmo_down_x) * dx + (y - gizmo_down_y) * dy) / l : 0;
        if (gizmo_mode == "Move") {
            Vec3 delta{};
            delta[gizmo_axis] = amount * gizmo_world_length;
            if (snap)
                delta[gizmo_axis] = std::round(delta[gizmo_axis] * 4) / 4;
            const auto* e = entity(resolved, selected);
            if (e && e->at("parent").is_string())
                if (auto* parent = entity(resolved, e->at("parent").get<std::string>())) {
                    Mat4 inv;
                    if (inverse(world(resolved, *parent), inv))
                        delta = vector(inv, delta);
                }
            auto position = vec(gizmo_original.at("position"));
            position = add(position, delta);
            preview_fields[gizmo_component + "/position"] = position;
        } else if (gizmo_mode == "Rotate") {
            Vec3 delta{};
            delta[gizmo_axis] = snap ? std::round(amount * 12) * .261799f : amount * 3.141593f;
            const auto composed =
                render::multiply(render::transform({}, vec(gizmo_original.at("rotation"))),
                                 render::transform({}, delta));
            preview_fields[gizmo_component + "/rotation"] = euler_xyz(composed);
        } else {
            auto scale = vec(gizmo_original.at("scale"));
            scale[gizmo_axis] *= std::max(.01f, 1 + amount);
            if (snap)
                scale[gizmo_axis] = std::max(.05f, std::round(scale[gizmo_axis] * 4) / 4);
            preview_fields[gizmo_component + "/scale"] = scale;
        }
    }
    bool viewport_event(const render::Event& event) {
        using Type = render::Event::Type;
        if (event.type == Type::FocusLost) {
            camera_drag = 0;
            gizmo_axis = -1;
            preview_fields.clear();
            return false;
        }
        if (event.type == Type::MouseMove) {
            const auto dx = event.x - last_x, dy = event.y - last_y;
            last_x = event.x;
            last_y = event.y;
            mouse_x = event.x;
            mouse_y = event.y;
            if (gizmo_axis >= 0) {
                update_gizmo(event.x, event.y, event.control);
                return true;
            }
            if (camera_drag) {
                const bool is2d = current.at("scene").value("dimension", 3) == 2;
                if (camera_drag == 3 && !is2d) {
                    yaw -= dx / ui_scale * .008f;
                    pitch = std::clamp(pitch + dy / ui_scale * .008f, -1.5f, 1.5f);
                } else {
                    const auto right = Vec3{std::cos(yaw), 0, -std::sin(yaw)};
                    const auto up =
                        is2d ? Vec3{0, 1, 0} : normalize(cross(right, sub(camera().eye, target)));
                    const auto scale = (is2d ? ortho : distance) / std::max(1.f, viewport.height);
                    target = add(target, add(mul(is2d ? Vec3{1, 0, 0} : right, -dx * scale),
                                             mul(up, dy * scale)));
                }
                return true;
            }
        }
        if (event.type == Type::MouseDown && viewport.contains(event.x, event.y)) {
            last_x = event.x;
            last_y = event.y;
            if (event.button == 2 || event.button == 3) {
                camera_drag = event.button;
                return true;
            }
            if (event.button == 1) {
                if (!start_gizmo(event.x, event.y))
                    pick(event.x, event.y);
                return true;
            }
        }
        if (event.type == Type::MouseUp) {
            if (event.button == camera_drag) {
                camera_drag = 0;
                return true;
            }
            if (event.button == 1 && gizmo_axis >= 0) {
                const auto field = gizmo_mode == "Move"     ? "position"
                                   : gizmo_mode == "Rotate" ? "rotation"
                                                            : "scale";
                const auto key = gizmo_component + "/" + field;
                if (preview_fields.contains(key))
                    if (const auto* object = entity(resolved, selected))
                        transaction(Json::array({field_operation(*object, gizmo_component, field,
                                                                 preview_fields[key])}),
                                    gizmo_revision);
                preview_fields.erase(key);
                gizmo_axis = -1;
                return true;
            }
        }
        if (event.type == Type::Wheel && viewport.contains(mouse_x, mouse_y)) {
            distance = std::clamp(distance * std::exp(-event.y * .12f), .25f, 500.f);
            ortho = std::clamp(ortho * std::exp(-event.y * .12f), .1f, 1000.f);
            return true;
        }
        return false;
    }
    void events(const std::vector<render::Event>& input) {
        using Type = render::Event::Type;
        for (const auto& event : input) {
            if (event.type == Type::KeyDown && event.key == "Escape") {
                if (project_settings_open) {
                    project_settings_open = false;
                    ui.clear_focus(false);
                    continue;
                }
                if (project_switch_warning) {
                    project_switch_warning = false;
                    ui.clear_focus(false);
                    continue;
                }
                if (gizmo_axis >= 0) {
                    gizmo_axis = -1;
                    preview_fields.clear();
                    continue;
                }
                if (simulation_open) {
                    simulation_open = false;
                    ui.clear_focus(false);
                    continue;
                }
                if (palette || !menu.empty()) {
                    palette = false;
                    menu.clear();
                    ui.clear_focus(false);
                    continue;
                }
            }
            if (event.type == Type::MouseDown && !menu.empty() &&
                !ui.find("menu-popup")->rect.contains(event.x, event.y) && event.y > 36 * ui_scale)
                menu.clear();
            if (event.type == Type::MouseMove || event.type == Type::MouseDown) {
                mouse_x = event.x;
                mouse_y = event.y;
            }
            if ((palette || !recovery.empty() || simulation_open || project_switch_warning ||
                 project_settings_open) &&
                event.type == Type::MouseDown) {
                auto* modal = ui.find(project_settings_open    ? "project-settings-panel"
                                      : project_switch_warning ? "project-switch-dialog"
                                      : !recovery.empty()      ? "recovery-panel"
                                      : simulation_open        ? "simulation-panel"
                                                               : "palette");
                if (!modal->rect.contains(event.x, event.y))
                    continue;
            }
            if (camera_drag || gizmo_axis >= 0) {
                if (viewport_event(event))
                    continue;
            }
            if (ui.handle(event))
                continue;
            if ((project_switch_warning || !recovery.empty() || simulation_open ||
                 project_settings_open) &&
                event.type == Type::KeyDown)
                continue;
            if (event.type == Type::KeyDown) {
                if (event.control && (event.key == "S" || event.key == "s")) {
                    save();
                    continue;
                }
                if (event.control && (event.key == "Z" || event.key == "z")) {
                    history(event.shift);
                    continue;
                }
                if (event.control && (event.key == "P" || event.key == "p")) {
                    ui.clear_focus();
                    if (ui.editing())
                        continue;
                    palette = !palette;
                    continue;
                }
                if (!ui.editing()) {
                    if (event.key == "Delete")
                        delete_selected();
                    if (event.key == "F" || event.key == "f")
                        frame_selection();
                    if (event.key == "W" || event.key == "w")
                        gizmo_mode = "Move";
                    if (event.key == "E" || event.key == "e")
                        gizmo_mode = "Rotate";
                    if (event.key == "R" || event.key == "r")
                        gizmo_mode = "Scale";
                }
            }
            viewport_event(event);
        }
    }
    void frame(const std::vector<render::Event>& events_) {
        const auto next_scale = std::clamp(renderer.display_scale(), .5f, 4.f);
        const bool geometry_changed = next_scale != ui_scale || layout_width != renderer.width() ||
                                      layout_height != renderer.height();
        if (next_scale != ui_scale) {
            camera_drag = 0;
            if (gizmo_axis >= 0) {
                const auto field = gizmo_mode == "Move"     ? "position"
                                   : gizmo_mode == "Rotate" ? "rotation"
                                                            : "scale";
                preview_fields.erase(gizmo_component + "/" + field);
                gizmo_axis = -1;
            }
        }
        ui_scale = next_scale;
        layout_width = renderer.width();
        layout_height = renderer.height();
        session.poll();
        poll_presentation();
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        if (rendered.scene_rect[2] == 0 || geometry_changed)
            build_snapshot();
        events(events_);
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        build_snapshot();
    }
};
EditorUI::EditorUI(Session& s, render::Renderer& r, const std::filesystem::path& font,
                   const std::filesystem::path& styles)
    : impl_(std::make_unique<Impl>(s, r, font, styles)) {}
EditorUI::~EditorUI() = default;
void EditorUI::frame(const std::vector<render::Event>& input) {
    impl_->frame(input);
}
const render::Snapshot& EditorUI::snapshot() const {
    return impl_->rendered;
}
ui::Context& EditorUI::widgets() {
    return impl_->ui;
}
const std::string& EditorUI::current_document() const {
    return impl_->document;
}
void EditorUI::select_document(const std::string& id) {
    impl_->choose_document(id);
}
const std::string& EditorUI::selected_entity() const {
    return impl_->selected;
}
void EditorUI::select_entity(const std::string& id) {
    impl_->select(id);
}
bool EditorUI::project_switch_requested() const {
    return impl_->project_switch_requested;
}
void EditorUI::set_project_switch_enabled(bool enabled) {
    impl_->project_switch_enabled = enabled;
    if (!enabled) {
        impl_->project_switch_requested = false;
        impl_->project_switch_warning = false;
    }
}
} // namespace faset::editor
