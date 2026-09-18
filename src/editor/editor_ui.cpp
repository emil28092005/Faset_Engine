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
    if (auto* t = component(e, "faset.transform")) {
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
    std::string picks_key;
    std::string document, selected, source_file, asset_filter,
        active_bottom = "assets", menu, command_name = "faset_documents", status = "Ready",
        gizmo_mode = "Move";
    Json current, resolved, files = Json::array(), assets = Json::array(), schemas = Json::object(),
                            recovery = Json::array();
    std::uint64_t shown_revision = std::numeric_limits<std::uint64_t>::max();
    std::map<std::string, std::uint64_t> edit_revisions;
    std::map<std::string, Json> preview_fields;
    std::filesystem::path layout_path;
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
        ui.set_theme(ui::Theme::load(styles));
        ui.apply_layout(read_json(styles.parent_path() / "editor-layout.json"));
        layout_path = session.config().project_root / ".faset/editor-layout.json";
        dock.move("assets", "bottom", 0);
        dock.move("console", "bottom", 1);
        dock.move("jobs", "bottom", 2);
        if (std::filesystem::exists(layout_path))
            try {
                dock.load(layout_path);
            } catch (const std::exception& e) {
                session.log(std::string("Layout reset: ") + e.what());
            }
        ui.find("scene_panel")->layout.width = dock.size("scene", 224);
        ui.find("inspector_panel")->layout.width = dock.size("inspector", 300);
        ui.find("bottom_panel")->layout.height = dock.size("bottom", 184);
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
        refresh();
    }
    void persist_layout() {
        dock.set_size("scene", ui.find("scene_panel")->rect.width);
        dock.set_size("inspector", ui.find("inspector_panel")->rect.width);
        dock.set_size("bottom", ui.find("bottom_panel")->rect.height);
        try {
            dock.save(layout_path);
        } catch (const std::exception& e) {
            report(e.what());
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
            selected = id;
    }
    void build_static() {
        auto& menurow = ui.find("menubar")->add(Kind::Row, "menuitems");
        menurow.layout.gap = 2;
        for (const std::string name : {"Faset", "File", "Edit", "Scene", "View", "Help"})
            button(
                menurow, "menu-" + name, name, [this, name] { menu = menu == name ? "" : name; },
                name == "Faset" ? 72 : 54);
        auto& project = menurow.add(Kind::Label, "project-title",
                                    session.project().value("name", std::string("Project")));
        project.layout.flex = 1;
        auto& toolbar = ui.find("toolbar")->add(Kind::Row, "tools");
        toolbar.layout.gap = 5;
        button(toolbar, "save", "Save", [this] { save(); }, 60);
        button(toolbar, "undo", "Undo", [this] { history(false); }, 56);
        button(toolbar, "redo", "Redo", [this] { history(true); }, 56);
        button(
            toolbar, "play", "Play", [this] { call("faset_play", {{"document", document}}); }, 58);
        button(
            toolbar, "stop", "Stop",
            [this] {
                call("faset_stop");
                paused = false;
            },
            56);
        button(
            toolbar, "pause", "Pause",
            [this] {
                if (!call("faset_play_control", {{"command", paused ? "resume" : "pause"}})
                         .is_null())
                    paused = !paused;
            },
            65);
        button(
            toolbar, "step", "Step", [this] { call("faset_play_control", {{"command", "step"}}); },
            55);
        button(toolbar, "build", "Build C++", [this] { call("faset_build"); }, 94);
        button(
            toolbar, "export", "Export",
            [this] { call("faset_export", {{"document", document}, {"output", "Exports"}}); }, 64);
        auto& space = toolbar.add(Kind::Label, "toolbar-space", "");
        space.layout.flex = 1;
        button(toolbar, "command-palette", "Commands", [this] { palette = !palette; }, 104);
        auto& scene = *ui.find("scene_panel");
        scene.add(Kind::Tab, "scene-tab", "Scene").selected = true;
        auto& tools = scene.add(Kind::Row, "scene-tools");
        tools.layout.height = 28;
        tools.layout.padding = 3;
        tools.layout.gap = 3;
        button(tools, "add-object", "+ Object", [this] { create_object("Object"); }, 80);
        button(tools, "add-cube", "Cube", [this] { create_object("Cube"); }, 55);
        button(tools, "add-sprite", "Sprite", [this] { create_object("Sprite"); }, 58);
        auto& tree = scene.add(Kind::Column, "scene-tree");
        tree.layout.flex = 1;
        tree.layout.scroll = true;
        tree.layout.gap = 0;
        tree.on_drop = [this](Widget&, const Json& payload) {
            if (payload.value("kind", std::string()) == "entity")
                transaction(Json::array({{{"op", "entity.reparent"},
                                          {"entity", payload.at("id")},
                                          {"parent", nullptr},
                                          {"keep_world", true}}}));
        };
        auto& inspector = *ui.find("inspector_panel");
        inspector.add(Kind::Tab, "inspector-tab", "Inspector").selected = true;
        auto& body = inspector.add(Kind::Column, "properties");
        body.layout.flex = 1;
        body.layout.padding = 10;
        body.layout.gap = 7;
        body.layout.scroll = true;
        auto& vp = *ui.find("viewport");
        auto& vptools = vp.add(Kind::Row, "viewport-tools");
        vptools.layout.absolute = true;
        vptools.layout.x = 8;
        vptools.layout.y = 8;
        vptools.layout.height = 28;
        vptools.layout.width = 300;
        for (const std::string mode : {"Move", "Rotate", "Scale"})
            button(vptools, "gizmo-" + mode, mode, [this, mode] { gizmo_mode = mode; }, 64);
        button(vptools, "frame-selection", "Frame", [this] { frame_selection(); }, 64);
        vp.on_drop = [this](Widget&, const Json& data) {
            if (data.value("kind", std::string()) == "asset")
                instantiate_asset(data.at("id").get<std::string>());
        };
        auto& bottom = *ui.find("bottom_panel");
        bottom.dock_area = "bottom";
        auto& tabs = bottom.add(Kind::Row, "bottom-tabs");
        tabs.layout.height = 29;
        tabs.layout.gap = 0;
        for (const std::string id : {"assets", "console", "jobs"}) {
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
        assetbar.layout.height = 30;
        assetbar.layout.padding = 2;
        assetbar.layout.gap = 5;
        label(assetbar, "asset-path", "Project assets", 135);
        auto& search = assetbar.add(Kind::TextField, "asset-search", "");
        search.layout.width = 220;
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
            146);
        button(assetbar, "asset-open", "Open Scene", [this] { open_source(); }, 104);
        button(
            assetbar, "asset-refresh", "Refresh",
            [this] {
                assets_dirty = true;
                view.clearCache();
                picks_key.clear();
            },
            75);
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
        auto& statusrow = ui.find("statusbar")->add(Kind::Row, "status-row");
        auto& statuslabel = statusrow.add(Kind::Label, "status", "Ready");
        statuslabel.layout.flex = 1;
        label(statusrow, "renderer-status", "Vulkan", 225);
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
        auto& recover = ui.root().add(Kind::Panel, "recovery-panel");
        recover.layout.absolute = true;
        recover.layout.width = 470;
        recover.layout.height = 260;
        recover.layout.padding = 12;
        recover.layout.gap = 5;
        recover.visible = false;
    }
    void new_scene(int dimension) {
        auto result =
            call("faset_document_create", {{"name", "Untitled"}, {"dimension", dimension}});
        if (!result.is_null())
            choose_document(result.at("id"));
        menu.clear();
    }
    void choose_document(const std::string& id) {
        ui.clear_focus(false);
        document = id;
        selected.clear();
        current = session.authoring().query(id);
        shown_revision = std::numeric_limits<std::uint64_t>::max();
        preview_fields.clear();
        edit_revisions.clear();
        gizmo_axis = -1;
    }
    void delete_selected() {
        if (selected.empty())
            return;
        if (transaction(Json::array({{{"op", "entity.delete"}, {"entity", selected}}})))
            selected.clear();
    }
    void select(const std::string& id) {
        if (selected == id)
            return;
        ui.clear_focus();
        selected = id;
        preview_fields.clear();
        edit_revisions.clear();
        component_menu = false;
    }
    void open_source() {
        if (source_file.empty())
            return;
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
                ? std::filesystem::path(manifest.value("source", std::string("Imported asset")))
                      .stem()
                      .string()
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
            selected = entity_id;
    }
    void refresh() {
        if (document.empty())
            return;
        current = session.authoring().query(document);
        schemas = session.authoring().schemas().manifest();
        if (shown_revision != current.at("revision").get<std::uint64_t>()) {
            auto result = call("faset_template_preview", {{"document", document}});
            resolved = result.is_null() ? current.at("scene") : result.at("scene");
            if (!result.is_null() && !result.at("conflicts").empty())
                status = "Template conflicts: " + std::to_string(result.at("conflicts").size());
            shown_revision = current.at("revision");
        }
        if (!selected.empty() && !entity(current.at("scene"), selected))
            selected.clear();
        refresh_tree();
        refresh_inspector();
        refresh_assets();
        refresh_bottom();
        refresh_overlays();
        ui.find("undo")->enabled = current.value("can_undo", false);
        ui.find("redo")->enabled = current.value("can_redo", false);
        ui.find("pause")->enabled = session.playing();
        ui.find("step")->enabled = session.playing();
        ui.find("pause")->text = paused ? "Resume" : "Pause";
        ui.find("project-title")->text = session.project().value("name", std::string("Project")) +
                                         " / " + current.at("name").get<std::string>() +
                                         (current.value("dirty", false) ? " *" : "");
        ui.find("status")->text = status;
        ui.find("renderer-status")->text =
            "Vulkan 1.3  |  " + std::to_string(current.at("scene").at("entities").size()) +
            " objects";
    }
    void refresh_tree() {
        auto& tree = *ui.find("scene-tree");
        std::set<std::string> keep;
        const auto& scene = current.at("scene");
        auto& root = tree.add(Kind::TreeRow, "scene-root", scene.at("name"));
        root.selected = selected.empty();
        root.on_click = [this](Widget&) { select(""); };
        keep.insert(root.id);
        std::function<void(std::string, int)> append = [&](std::string parent, int depth) {
            for (const auto& e : scene.at("entities")) {
                const auto p = e.at("parent").is_string() ? e.at("parent").get<std::string>() : "";
                if (p != parent)
                    continue;
                const auto id = e.at("id").get<std::string>();
                auto& row = tree.add(Kind::TreeRow, "entity-" + id, e.at("name"));
                keep.insert(row.id);
                row.indent = depth;
                row.selected = selected == id;
                row.drag_payload = {{"kind", "entity"}, {"id", id}, {"label", e.at("name")}};
                row.on_click = [this, id](Widget&) { select(id); };
                row.on_drop = [this, id](Widget&, const Json& payload) {
                    if (payload.value("kind", std::string()) == "entity")
                        transaction(Json::array({{{"op", "entity.reparent"},
                                                  {"entity", payload.at("id")},
                                                  {"parent", id},
                                                  {"keep_world", true}}}));
                };
                append(id, depth + 1);
            }
        };
        append("", 1);
        trim_children(tree, keep);
        // Match retained children to hierarchy order after a reparent without
        // changing widget IDs.
        std::map<std::string, std::size_t> order;
        std::size_t n = 0;
        std::function<void(std::string)> visit = [&](std::string p) {
            for (const auto& e : scene.at("entities"))
                if ((e.at("parent").is_string() ? e.at("parent").get<std::string>() : "") == p) {
                    const auto id = e.at("id").get<std::string>();
                    order["entity-" + id] = ++n;
                    visit(id);
                }
        };
        order["scene-root"] = 0;
        visit("");
        std::stable_sort(tree.children.begin(), tree.children.end(),
                         [&](const auto& a, const auto& b) { return order[a->id] < order[b->id]; });
    }
    void set_field(const std::string& component_id, const std::string& field, Json value,
                   const std::string& widget) {
        const auto found = edit_revisions.find(widget);
        const auto revision = found == edit_revisions.end()
                                  ? current.at("revision").get<std::uint64_t>()
                                  : found->second;
        transaction(Json::array({{{"op", "component.set"},
                                  {"entity", selected},
                                  {"component", component_id},
                                  {"field", field},
                                  {"value", std::move(value)}}}),
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
    void refresh_inspector() {
        auto& body = *ui.find("properties");
        std::set<std::string> keep;
        const auto* e = entity(current.at("scene"), selected);
        if (!e) {
            label(body, "inspector-empty", "Select an object to edit its components");
            keep.insert("inspector-empty");
            trim_children(body, keep);
            return;
        }
        auto& name = body.add(Kind::TextField, "object-name");
        ui.update_text(name.id, e->at("name"));
        keep.insert(name.id);
        name.on_preview = [this](Widget&) {
            edit_revisions.try_emplace("object-name", current.at("revision").get<std::uint64_t>());
        };
        name.on_cancel = [this](Widget& w) { edit_revisions.erase(w.id); };
        name.on_commit = [this](Widget& w) {
            transaction(
                Json::array({{{"op", "entity.rename"}, {"entity", selected}, {"name", w.text}}}),
                edit_revisions.contains(w.id) ? edit_revisions[w.id]
                                              : current.at("revision").get<std::uint64_t>());
            edit_revisions.erase(w.id);
        };
        for (const auto& c : e->at("components")) {
            const auto cid = c.at("id").get<std::string>(), type = c.at("type").get<std::string>();
            const bool known = session.authoring().schemas().contains(type);
            const auto metadata =
                known ? session.authoring().schemas().schema(type)
                      : Json{{"name", type + " (schema missing)"}, {"fields", Json::object()}};
            auto& header = body.add(Kind::Row, "component-header-" + cid);
            keep.insert(header.id);
            header.layout.height = 28;
            auto& title =
                header.add(Kind::Label, "component-title-" + cid, metadata.value("name", type));
            title.layout.flex = 1;
            button(
                header, "component-remove-" + cid, "x",
                [this, cid] {
                    transaction(Json::array(
                        {{{"op", "component.remove"}, {"entity", selected}, {"component", cid}}}));
                },
                25);
            for (const auto& [fid, value] : c.at("fields").items()) {
                const auto descriptor = metadata.at("fields").value(fid, Json::object());
                const auto key = "field-" + cid + "-" + fid;
                auto& row = body.add(Kind::Column, key);
                keep.insert(key);
                row.layout.gap = 2;
                label(row, key + "-label",
                      descriptor.value("name", fid) +
                          (descriptor.value("unit", std::string()) == "radians" ? " (rad)" : ""));
                const auto& options = descriptor.value("enum", Json::array());
                const bool vector_value = value.is_array() && value.size() >= 2 &&
                                          value.size() <= 4 &&
                                          std::all_of(value.begin(), value.end(),
                                                      [](const Json& v) { return v.is_number(); });
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
                trim_children(row, {key + "-label", input_id});
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
                    input.layout.height = 27;
                } else if (value.is_boolean()) {
                    auto& input = row.add(Kind::Checkbox, key + "-value", "Enabled");
                    input.checked = value;
                    bind_field(input, cid, fid, value);
                } else if (vector_value) {
                    auto& vectorrow = row.add(Kind::Row, key + "-vector");
                    vectorrow.layout.gap = 3;
                    for (std::size_t axis = 0; axis < value.size(); ++axis) {
                        auto& input =
                            vectorrow.add(Kind::NumberField, key + "-" + std::to_string(axis));
                        input.layout.flex = 1;
                        input.layout.min_width = 35;
                        ui.update_number(input.id, value[axis].get<double>());
                        input.step = .02;
                        bind_field(input, cid, fid, value, int(axis));
                    }
                } else if (value.is_number()) {
                    auto& input = row.add(Kind::NumberField, key + "-value");
                    input.step = value.is_number_integer() ? 1 : .05;
                    input.precision = value.is_number_integer() ? 0 : 3;
                    ui.update_number(input.id, value.get<double>());
                    bind_field(input, cid, fid, value);
                } else {
                    auto& input = row.add(Kind::TextField, key + "-value");
                    ui.update_text(input.id,
                                   value.is_string() ? value.get<std::string>() : value.dump());
                    bind_field(input, cid, fid, value);
                }
            }
        }
        auto& add = button(body, "add-component", "+ Add Component",
                           [this] { component_menu = !component_menu; });
        keep.insert(add.id);
        if (component_menu)
            for (const auto& schema : schemas.at("types")) {
                const auto type = schema.at("id").get<std::string>();
                auto& choice = button(
                    body, "component-choice-" + type, schema.value("name", type), [this, type] {
                        if (transaction(Json::array(
                                {{{"op", "component.add"}, {"entity", selected}, {"type", type}}})))
                            component_menu = false;
                    });
                keep.insert(choice.id);
            }
        trim_children(body, keep);
    }
    void refresh_assets() {
        const auto now = std::chrono::steady_clock::now();
        if (!assets_dirty && now - last_assets < std::chrono::seconds(1))
            return;
        assets_dirty = false;
        last_assets = now;
        files = Json::array();
        try {
            for (const std::string folder : {"Assets", "Scenes"}) {
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
                    auto relative =
                        std::filesystem::relative(entry.path(), session.config().project_root)
                            .generic_string();
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
        std::set<std::string> keep;
        for (const auto& file : files) {
            const auto path = file.get<std::string>();
            auto& row = list.add(Kind::TreeRow, "file-" + path, path);
            row.layout.height = 25;
            row.indent = 1;
            row.selected = source_file == path;
            row.on_click = [this, path](Widget&) {
                source_file = path;
                assets_dirty = true;
            };
            keep.insert(row.id);
        }
        for (const auto& asset : assets) {
            const auto id = asset.at("id").get<std::string>();
            const auto name = asset.contains("manifest")
                                  ? std::filesystem::path(asset["manifest"].value("source", id))
                                        .filename()
                                        .string()
                                  : id;
            auto& row = list.add(Kind::TreeRow, "asset-" + id, "Imported  /  " + name);
            row.layout.height = 25;
            row.indent = 1;
            row.drag_payload = {{"kind", "asset"}, {"id", id}, {"label", name}};
            row.on_click = [this, id](Widget&) {
                renderer.set_clipboard(id);
                status = "Asset ID copied; drag to viewport or an asset field";
            };
            keep.insert(row.id);
        }
        if (keep.empty()) {
            label(list, "assets-empty", "Place GLB, glTF, PNG or JPEG in Assets, then Import.");
            keep.insert("assets-empty");
        }
        trim_children(list, keep);
    }
    void refresh_bottom() {
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
        for (const std::string id : {"assets", "console", "jobs"})
            ui.find("tab-" + id)->selected = active_bottom == id;
        ui.find("asset-toolbar")->visible = active_bottom == "assets";
        ui.find("asset-items")->visible = active_bottom == "assets";
        ui.find("console-items")->visible = active_bottom == "console";
        ui.find("job-items")->visible = active_bottom == "jobs";
        auto& console = *ui.find("console-items");
        std::set<std::string> keep;
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
        auto result = call("faset_jobs");
        auto& jobs = *ui.find("job-items");
        keep.clear();
        if (!result.is_null())
            for (const auto& job : result.at("jobs")) {
                const auto id = job.at("id").get<std::string>();
                auto& row = jobs.add(Kind::Row, "job-" + id);
                row.layout.height = 28;
                keep.insert(row.id);
                const auto state = job.value("state", std::string());
                auto& text =
                    row.add(Kind::Label, "job-text-" + id,
                            job.value("kind", std::string("Job")) + " · " + state + " · " +
                                job.value("stage", std::string()) + " " +
                                std::to_string(int(job.value("progress", 0.0) * 100)) + "%");
                text.layout.flex = 1;
                auto& cancel = button(
                    row, "job-cancel-" + id, "Cancel",
                    [this, id] { call("faset_job_cancel", {{"id", id}}); }, 72);
                cancel.enabled = state == "queued" || state == "running";
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
        const bool file = menu == "File" || menu == "Faset",
                   edit = menu == "Edit" || menu == "Scene",
                   help = menu == "Help" || menu == "View";
        for (const auto* id :
             {"new-3d", "new-2d", "open-path", "open-path-button", "save-path", "save-as-button"})
            ui.find(id)->visible = file;
        for (const auto* id : {"menu-duplicate", "menu-delete"})
            ui.find(id)->visible = edit;
        for (const auto* id : {"help-one", "help-two", "help-three"})
            ui.find(id)->visible = help;
        popup.layout.height = file ? 260 : edit ? 112 : 150;
        popup.layout.x = menu == "File"    ? 70
                         : menu == "Edit"  ? 126
                         : menu == "Scene" ? 182
                         : menu == "View"  ? 238
                         : menu == "Help"  ? 294
                                           : 5;
        auto& command = *ui.find("palette");
        command.visible = palette;
        command.layout.x = std::max(0.f, (float(renderer.width()) - 570) / 2);
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
    void refresh_recovery() {
        auto& panel = *ui.find("recovery-panel");
        panel.visible = !recovery.empty();
        panel.layout.x = std::max(0.f, (float(renderer.width()) - 470) / 2);
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
                grid.color = {.12f, .12f, .13f, 1};
                grid.model = render::transform({float(i), 0, 0}, {}, {.012f, .002f, 20});
                rendered.draws.push_back(grid);
                grid.model = render::transform({0, 0, float(i)}, {}, {20, .002f, .012f});
                rendered.draws.push_back(grid);
            }
        }
        gizmo_valid = false;
        if (auto* e = entity(scene, selected)) {
            gizmo_origin = point(world(scene, *e), {});
            gizmo_valid = project(gizmo_origin, gizmo_screen[0]);
            const auto length = is2d ? ortho * .12f : distance * .12f;
            for (int axis = 0; axis < 3; ++axis) {
                Vec3 p = gizmo_origin;
                p[axis] += length;
                gizmo_valid = project(p, gizmo_screen[axis + 1]) && gizmo_valid;
            }
            if (gizmo_valid) {
                const render::Color colors[3] = {
                    {.88f, .35f, .34f, 1}, {.38f, .75f, .49f, 1}, {.4f, .58f, .92f, 1}};
                for (int axis = 0; axis < (is2d ? 2 : 3); ++axis) {
                    line(gizmo_screen[0], gizmo_screen[axis + 1], colors[axis], 2);
                    const auto end = gizmo_screen[axis + 1];
                    if (viewport.contains(end[0], end[1]))
                        rendered.ui_quads.push_back(
                            {end[0] - 4, end[1] - 4, 8, 8, colors[axis], {}, {0, 0, 1, 1}});
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
        const auto* e = entity(current.at("scene"), selected);
        if (!e)
            return false;
        const auto* c = component(*e, "faset.transform");
        if (!c)
            return false;
        float best = 8;
        int axis = -1;
        for (int i = 0; i < (current.at("scene").value("dimension", 3) == 2 ? 2 : 3); ++i) {
            auto a = gizmo_screen[0], b = gizmo_screen[i + 1];
            const auto dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
            if (l < 16)
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
            const auto* e = entity(current.at("scene"), selected);
            if (e && e->at("parent").is_string())
                if (auto* parent =
                        entity(current.at("scene"), e->at("parent").get<std::string>())) {
                    Mat4 inv;
                    if (inverse(world(current.at("scene"), *parent), inv))
                        delta = vector(inv, delta);
                }
            auto position = vec(gizmo_original.at("position"));
            position = add(position, delta);
            preview_fields[gizmo_component + "/position"] = position;
        } else if (gizmo_mode == "Rotate") {
            auto rotation = vec(gizmo_original.at("rotation"));
            rotation[gizmo_axis] += snap ? std::round(amount * 12) * .261799f : amount * 3.141593f;
            preview_fields[gizmo_component + "/rotation"] = rotation;
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
                    yaw -= dx * .008f;
                    pitch = std::clamp(pitch + dy * .008f, -1.5f, 1.5f);
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
                    transaction(Json::array({{{"op", "component.set"},
                                              {"entity", selected},
                                              {"component", gizmo_component},
                                              {"field", field},
                                              {"value", preview_fields[key]}}}),
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
                if (gizmo_axis >= 0) {
                    gizmo_axis = -1;
                    preview_fields.clear();
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
                !ui.find("menu-popup")->rect.contains(event.x, event.y) && event.y > 36)
                menu.clear();
            if (event.type == Type::MouseMove || event.type == Type::MouseDown) {
                mouse_x = event.x;
                mouse_y = event.y;
            }
            if ((palette || !recovery.empty()) && event.type == Type::MouseDown) {
                auto* modal = ui.find(!recovery.empty() ? "recovery-panel" : "palette");
                if (!modal->rect.contains(event.x, event.y))
                    continue;
            }
            if (camera_drag || gizmo_axis >= 0) {
                if (viewport_event(event))
                    continue;
            }
            if (ui.handle(event))
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
        session.poll();
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()));
        if (rendered.scene_rect[2] == 0)
            build_snapshot();
        events(events_);
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()));
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
} // namespace faset::editor
