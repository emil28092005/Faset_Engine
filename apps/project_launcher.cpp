#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <faset/core/io.hpp>
#include <faset/editor/project_launcher.hpp>
#include <fstream>
#include <set>
#include <thread>

namespace faset::editor {
namespace {
namespace fs = std::filesystem;
using ui::Kind;
using ui::Widget;
fs::path user_home() {
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"USERPROFILE"); value && *value)
        return fs::path(value);
#else
    if (const auto* value = std::getenv("HOME"))
        return fs::path(value);
#endif
    return fs::current_path();
}
fs::path recent_path() {
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"APPDATA"); value && *value)
        return fs::path(value) / "Faset/recent-projects.json";
#else
    if (const auto* value = std::getenv("XDG_CONFIG_HOME"); value && *value)
        return fs::path(value) / "faset/recent-projects.json";
#endif
    return user_home() / ".config/faset/recent-projects.json";
}
std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
fs::path normalize(std::string text) {
    text = trimmed(std::move(text));
    if (text.empty())
        throw std::runtime_error("Enter a project directory.");
    if (text.find_first_of("\r\n\t") != std::string::npos || text.find('\0') != std::string::npos)
        throw std::runtime_error("A directory must fit on one line.");
    fs::path path = path_from_utf8(text);
    if (text == "~")
        path = user_home();
    else if (text.starts_with("~/") || text.starts_with("~\\"))
        path = user_home() / path_from_utf8(text.substr(2));
    path = fs::weakly_canonical(fs::absolute(path));
    if (path.filename() == "project.faset.json")
        path = path.parent_path();
    return path;
}
ProjectSelection open_project(const fs::path& path) {
    if (!fs::is_directory(path))
        throw std::runtime_error("Project directory does not exist.");
    if (!fs::is_regular_file(path / "project.faset.json"))
        throw std::runtime_error("No project.faset.json in this directory.");
    Json data;
    try {
        data = read_json(path / "project.faset.json");
    } catch (...) {
        throw std::runtime_error("Cannot read project.faset.json.");
    }
    if (!data.is_object() || data.value("format", std::string()) != "faset.project" ||
        data.value("version", 0) != 1)
        throw std::runtime_error("Unsupported project format or version.");
    auto name = data.value("name", std::string());
    const auto dimension = data.value("dimension", 3);
    if (name.empty() || (dimension != 2 && dimension != 3))
        throw std::runtime_error("Project name or scene type is invalid.");
    return {path, std::move(name), dimension, false};
}
Json recent_records(const fs::path& file) {
    try {
        auto j = read_json(file);
        return j.is_array() ? j : Json::array();
    } catch (...) {
        return Json::array();
    }
}
void remember(const fs::path& project, const fs::path& store) {
    const auto selected = open_project(normalize(path_to_utf8(project)));
    auto records = recent_records(store);
    std::erase_if(records.get_ref<Json::array_t&>(), [&](const Json& record) {
        return !record.is_string() || record.get<std::string>() == path_to_utf8(selected.path);
    });
    records.insert(records.begin(), path_to_utf8(selected.path));
    while (records.size() > 12)
        records.erase(records.end() - 1);
    atomic_write_json(store, records);
}
Widget& button(Widget& parent, const std::string& id, const std::string& text,
               std::function<void()> action) {
    auto& w = parent.add(Kind::Button, id, text);
    w.on_click = [action = std::move(action)](Widget&) { action(); };
    return w;
}
} // namespace

void remember_project(const fs::path& project) {
    try {
        remember(project, recent_path());
    } catch (...) { /* Recent history is not required to open a project. */
    }
}
struct ProjectLauncher::Impl {
    render::Renderer& renderer;
    ui::Context ui;
    render::Snapshot frame_data;
    float ui_scale = 1;
    float logical_width() const {
        return float(renderer.width()) / ui_scale;
    }
    float logical_height() const {
        return float(renderer.height()) / ui_scale;
    }
    fs::path recents_file, browse_path;
    std::optional<fs::path> pending_browse;
    std::optional<ProjectSelection> selected;
    bool create = false, cancelled = false, browsing = false, browser_valid = false;
    int dimension = 3;
    std::string error;
    Impl(render::Renderer& r, const fs::path& engine, const fs::path& initial,
         const fs::path& recents)
        : renderer(r), ui(engine / "assets/fonts/NotoSans.ttf"),
          recents_file(recents.empty() ? recent_path() : recents) {
        auto theme = ui::Theme::load(engine / "assets/ui/dark.json");
        theme.font_size = 15;
        theme.row_height = 30;
        ui.set_theme(theme);
        ui.apply_layout(read_json(engine / "assets/ui/project-launcher-layout.json"));
        ui.set_clipboard([this] { return renderer.clipboard(); },
                         [this](const std::string& value) { renderer.set_clipboard(value); });
        ui.set_ime(
            [this](bool enabled) { renderer.set_text_input(enabled); },
            [this](ui::Rect r) { renderer.set_text_input_area(r.x, r.y, r.width, r.height); });
        const auto start = initial.empty() ? user_home() / "FasetProjects/MyGame" : initial;
        ui.update_text("launcher-path", path_to_utf8(start));
        create = initial.empty();
        ui.find("launcher-open")->on_click = [this](Widget&) { set_mode(false); };
        ui.find("launcher-create")->on_click = [this](Widget&) { set_mode(true); };
        ui.find("launcher-2d")->on_click = [this](Widget&) { dimension = 2; };
        ui.find("launcher-3d")->on_click = [this](Widget&) { dimension = 3; };
        ui.find("launcher-submit")->selected = true;
        ui.find("launcher-submit")->on_click = [this](Widget&) { submit(); };
        ui.find("launcher-cancel")->on_click = [this](Widget&) { cancelled = true; };
        ui.find("launcher-name")->on_preview = [this](Widget&) { error.clear(); };
        ui.find("launcher-path")->on_preview = [this](Widget&) { error.clear(); };
        ui.find("launcher-browse")->on_click = [this](Widget&) { show_browser(); };
        build_browser();
        load_recents();
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        ui.focus(create ? "launcher-name" : "launcher-path");
    }
    void set_mode(bool value) {
        ui.clear_focus();
        create = value;
        error.clear();
        refresh();
        ui.focus(create ? "launcher-name" : "launcher-path");
    }
    void load_recents() {
        auto& list = *ui.find("launcher-recents");
        std::size_t i = 0;
        std::set<fs::path> paths;
        for (const auto& record : recent_records(recents_file)) {
            if (!record.is_string())
                continue;
            try {
                const auto selection = open_project(normalize(record.get<std::string>()));
                if (!paths.insert(selection.path).second)
                    continue;
                const auto id = "launcher-recent-" + std::to_string(i++);
                auto& item = list.add(Kind::TreeRow, id, selection.name);
                item.layout.height = 40;
                item.tooltip = path_to_utf8(selection.path);
                item.on_click = [this, path = selection.path](Widget&) {
                    ui.update_text("launcher-path", path_to_utf8(path), true);
                    set_mode(false);
                };
            } catch (...) {
            }
        }
        if (i == 0)
            list.add(Kind::Label, "launcher-recents-empty", "No recent projects");
    }
    void submit() {
        ui.clear_focus();
        if (ui.editing())
            return;
        try {
            const auto path = normalize(ui.find("launcher-path")->text);
            if (create) {
                const auto name = trimmed(ui.find("launcher-name")->text);
                if (name.empty())
                    throw std::runtime_error("Enter a project name.");
                if (name.size() > 256 || name.find_first_of("\r\n\t") != std::string::npos)
                    throw std::runtime_error("Use a short project name on one line.");
                if (fs::exists(path) && (!fs::is_directory(path) || !fs::is_empty(path)))
                    throw std::runtime_error("Create requires a new or empty directory.");
                auto parent = path.parent_path();
                while (!parent.empty() && !fs::exists(parent))
                    parent = parent.parent_path();
                if (parent.empty() || !fs::is_directory(parent))
                    throw std::runtime_error("Project parent directory is unavailable.");
                selected = ProjectSelection{path, name, dimension, true};
            } else
                selected = open_project(path);
            error.clear();
        } catch (const fs::filesystem_error&) {
            error = "Cannot access this directory.";
        } catch (const Json::exception&) {
            error = "Invalid project.faset.json metadata.";
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    void build_browser() {
        auto& panel = ui.root().add(Kind::Panel, "launcher-browser");
        panel.layout.absolute = true;
        panel.layout.padding = 16;
        panel.layout.gap = 10;
        panel.visible = false;
        auto& title = panel.add(Kind::Label, "browser-heading", "Choose project directory");
        title.font_size = 20;
        title.layout.height = 35;
        auto& nav = panel.add(Kind::Row, "browser-navigation");
        nav.layout.height = 36;
        button(nav, "browser-up", "Up", [this] {
            browse_to(browse_path.parent_path());
        }).layout.width = 60;
        button(nav, "browser-home", "Home", [this] { browse_to(user_home()); }).layout.width = 76;
        auto& path = nav.add(Kind::TextField, "browser-path");
        path.layout.flex = 1;
        path.on_preview = [this](Widget&) { browser_valid = false; };
        path.on_commit = [this](Widget& w) {
            try {
                pending_browse = normalize(w.text);
            } catch (const std::exception& e) {
                error = e.what();
            }
        };
        auto& list = panel.add(Kind::Column, "browser-list");
        list.layout.flex = 1;
        list.layout.scroll = true;
        list.layout.gap = 2;
        auto& actions = panel.add(Kind::Row, "browser-actions");
        actions.layout.height = 36;
        button(actions, "browser-choose", "Choose directory", [this] {
            if (browser_valid) {
                ui.update_text("launcher-path", path_to_utf8(browse_path), true);
                close_browser();
            }
        }).layout.width = 180;
        button(actions, "browser-cancel", "Cancel", [this] { close_browser(); }).layout.width = 90;
        panel.add(Kind::Label, "browser-error");
    }
    void browse_to(const fs::path& path) {
        browser_valid = false;
        try {
            const auto normalized = fs::weakly_canonical(fs::absolute(path));
            if (!fs::is_directory(normalized))
                throw std::runtime_error("Directory does not exist.");
            std::vector<fs::path> children;
            for (const auto& entry : fs::directory_iterator(
                     normalized, fs::directory_options::skip_permission_denied)) {
                std::error_code status;
                if (entry.is_directory(status))
                    children.push_back(entry.path());
            }
            std::sort(children.begin(), children.end());
            browse_path = normalized;
            error.clear();
            browser_valid = true;
            auto& list = *ui.find("browser-list");
            list.children.clear();
            list.scroll_y = 0;
            ui.update_text("browser-path", path_to_utf8(browse_path), true);
            std::size_t index = 0;
            for (const auto& child : children) {
                auto& row = list.add(Kind::TreeRow, "browser-entry-" + std::to_string(index++),
                                     "[Folder]  " + path_to_utf8(child.filename()));
                row.layout.height = 34;
                row.on_click = [this, child](Widget&) {
                    ui.clear_focus();
                    browse_to(child);
                };
            }
            if (children.empty())
                list.add(Kind::Label, "browser-empty", "No subdirectories");
        } catch (const fs::filesystem_error&) {
            error = "Cannot access this directory.";
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    void show_browser() {
        ui.clear_focus();
        fs::path path = user_home();
        try {
            path = normalize(ui.find("launcher-path")->text);
            while (!path.empty() && !fs::exists(path))
                path = path.parent_path();
            if (!fs::is_directory(path))
                path = path.parent_path();
        } catch (...) {
        }
        browsing = true;
        refresh();
        browse_to(path);
        ui.focus("browser-path");
    }
    void close_browser() {
        ui.clear_focus(false);
        browsing = false;
        error.clear();
        refresh();
        ui.focus("launcher-path");
    }
    void navigate_pending() {
        if (pending_browse) {
            const auto path = *pending_browse;
            pending_browse.reset();
            browse_to(path);
        }
    }
    void refresh() {
        ui_scale = std::clamp(renderer.display_scale(), .5f, 4.f);
        ui.find("launcher-open")->selected = !create;
        ui.find("launcher-create")->selected = create;
        ui.find("launcher-2d")->selected = dimension == 2;
        ui.find("launcher-3d")->selected = dimension == 3;
        for (const auto* id : {"launcher-name-label", "launcher-name", "launcher-dimension-label",
                               "launcher-dimensions"})
            ui.find(id)->visible = create;
        ui.find("launcher-heading")->text = create ? "Create project" : "Open project";
        ui.find("launcher-submit")->text = create ? "Create project" : "Open project";
        ui.find("launcher-hint")->text = create ? "2D and 3D can share one project."
                                                : "Open a directory containing project.faset.json.";
        ui.find("launcher-error")->text = browsing ? "" : error;
        ui.find("browser-error")->text = error;
        ui.find("browser-choose")->enabled = browser_valid;
        ui.find("launcher-body")->enabled = !browsing;
        auto* dialog = ui.find("launcher-browser");
        dialog->visible = browsing;
        dialog->layout.width = std::max(320.f, std::min(760.f, logical_width() - 40));
        dialog->layout.height = std::max(300.f, std::min(540.f, logical_height() - 40));
        dialog->layout.x = (logical_width() - dialog->layout.width) * .5f;
        dialog->layout.y = (logical_height() - dialog->layout.height) * .5f;
        ui.find("launcher-sidebar")->layout.width =
            std::clamp(logical_width() * .24f, 180.f, 235.f);
        ui.find("launcher-main")->layout.padding = logical_width() < 850 ? 16 : 32;
    }
    void frame(const std::vector<render::Event>& events) {
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        for (const auto& event : events) {
            if (event.type == render::Event::Type::Quit)
                cancelled = true;
            if (event.type == render::Event::Type::KeyDown && event.key == "Escape") {
                if (browsing)
                    close_browser();
                else
                    cancelled = true;
                continue;
            }
            if (event.type == render::Event::Type::KeyDown && event.control &&
                event.key == "Return") {
                if (browsing) {
                    ui.clear_focus();
                    navigate_pending();
                    if (!ui.editing() && browser_valid) {
                        ui.update_text("launcher-path", path_to_utf8(browse_path), true);
                        close_browser();
                    }
                } else
                    submit();
                continue;
            }
            if (browsing && event.type == render::Event::Type::MouseDown &&
                !ui.find("launcher-browser")->rect.contains(event.x, event.y))
                continue;
            ui.handle(event);
            navigate_pending();
            refresh();
            ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        }
        refresh();
        ui.layout(float(renderer.width()), float(renderer.height()), ui_scale);
        frame_data = {};
        frame_data.clear_color = ui.theme().background;
        ui.draw(frame_data);
    }
};
ProjectLauncher::ProjectLauncher(render::Renderer& r, const fs::path& engine,
                                 const fs::path& initial, const fs::path& recents)
    : impl_(std::make_unique<Impl>(r, engine, initial, recents)) {}
ProjectLauncher::~ProjectLauncher() = default;
void ProjectLauncher::frame(const std::vector<render::Event>& events) {
    impl_->frame(events);
}
const render::Snapshot& ProjectLauncher::snapshot() const {
    return impl_->frame_data;
}
ui::Context& ProjectLauncher::widgets() {
    return impl_->ui;
}
const std::optional<ProjectSelection>& ProjectLauncher::selection() const {
    return impl_->selected;
}
bool ProjectLauncher::cancelled() const {
    return impl_->cancelled;
}
std::optional<ProjectSelection> run_project_launcher(const fs::path& engine,
                                                     const fs::path& initial,
                                                     std::uint64_t max_frames,
                                                     const fs::path& capture) {
    render::Renderer renderer({1100, 720, "Faset Engine — Projects", false, true});
    ProjectLauncher launcher(renderer, engine, initial);
    std::uint64_t frames = 0;
    while (!renderer.should_close() && !launcher.cancelled() && !launcher.selection() &&
           (max_frames == 0 || frames < max_frames)) {
        launcher.frame(renderer.poll_events());
        renderer.render(launcher.snapshot());
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!capture.empty())
        renderer.capture(capture);
    if (renderer.stats().validation_errors)
        throw std::runtime_error("Project launcher Vulkan validation failed");
    return launcher.selection();
}
} // namespace faset::editor
