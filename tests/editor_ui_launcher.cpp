#include <cstdlib>
#include <faset/core/io.hpp>
#include <faset/editor/project_launcher.hpp>
#include <iostream>
using namespace faset;
namespace {
class ScopedPathEnvironment {
  public:
#ifdef _WIN32
    ScopedPathEnvironment(const wchar_t* name, const std::filesystem::path& value) : name_(name) {
        if (const auto* previous = _wgetenv(name))
            old_ = previous;
        if (_wputenv_s(name, value.c_str()) != 0)
            throw std::runtime_error("Cannot set test environment");
    }
    ~ScopedPathEnvironment() {
        _wputenv_s(name_.c_str(), old_.c_str());
    }

  private:
    std::wstring name_, old_;
#else
    ScopedPathEnvironment(const char* name, const std::filesystem::path& value) : name_(name) {
        if (const auto* previous = std::getenv(name)) {
            old_ = previous;
            existed_ = true;
        }
        if (setenv(name, value.c_str(), 1) != 0)
            throw std::runtime_error("Cannot set test environment");
    }
    ~ScopedPathEnvironment() {
        if (existed_)
            setenv(name_.c_str(), old_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

  private:
    std::string name_, old_;
    bool existed_ = false;
#endif
};
void check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
render::Event key(std::string name, bool control = false) {
    render::Event e;
    e.type = render::Event::Type::KeyDown;
    e.key = std::move(name);
    e.control = control;
    return e;
}
void click(editor::ProjectLauncher& launcher, const std::string& id) {
    const auto* w = launcher.widgets().find(id);
    check(w, "Missing launcher widget: " + id);
    const auto rect = w->rect.intersection(w->clip);
    check(rect.width > 0 && rect.height > 0, "Hidden launcher widget: " + id);
    render::Event down;
    down.type = render::Event::Type::MouseDown;
    down.button = 1;
    down.x = rect.x + rect.width * .5f;
    down.y = rect.y + rect.height * .5f;
    auto up = down;
    up.type = render::Event::Type::MouseUp;
    launcher.frame({down, up});
}
void text(editor::ProjectLauncher& launcher, const std::string& id, const std::string& value,
          bool commit = true) {
    click(launcher, id);
    render::Event input;
    input.type = render::Event::Type::TextInput;
    input.text = value;
    std::vector<render::Event> events{key("A", true), input};
    if (commit)
        events.push_back(key("Return"));
    launcher.frame(events);
}
} // namespace
int main() {
    // Windows may spell TEMP using an 8.3 alias (RUNNER~1). The launcher returns
    // canonical paths, so the fixture must compare the same filesystem identity.
    const auto root = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
                      path_from_utf8("faset-проекты-" + new_id());
    try {
        const auto existing = root / path_from_utf8("Существующий проект");
        const auto new_project = root / path_from_utf8("Новая игра");
        atomic_write_json(existing / "project.faset.json", {{"format", "faset.project"},
                                                            {"version", 1},
                                                            {"name", "Existing project"},
                                                            {"dimension", 2}});
        const auto recents = root / "recent-projects.json";
        atomic_write_json(recents,
                          Json::array({path_to_utf8(existing), path_to_utf8(root / "Missing"),
                                       path_to_utf8(existing)}));
        render::Renderer renderer({1100, 720, "Project launcher acceptance", true, true});
        {
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE), {},
                                             recents);
            launcher.frame({});
            check(launcher.widgets().find("launcher-create")->selected,
                  "No initial path starts Create");
            check(launcher.widgets().find("launcher-recent-0") &&
                      !launcher.widgets().find("launcher-recent-1"),
                  "Recent list includes real valid unique projects only");
            text(launcher, "launcher-name", "Тестовый проект");
            text(launcher, "launcher-path", path_to_utf8(existing));
            click(launcher, "launcher-submit");
            check(!launcher.selection() && !launcher.widgets().find("launcher-error")->text.empty(),
                  "Create must reject nonempty existing project");
            text(launcher, "launcher-path", path_to_utf8(root / "." / new_project.filename()));
            click(launcher, "launcher-2d");
            check(launcher.widgets().find("launcher-2d")->selected, "2D project selection");
            click(launcher, "launcher-lua");
            check(launcher.widgets().find("launcher-lua")->selected,
                  "Lua project language selection");
            renderer.render(launcher.snapshot());
            renderer.capture(root / "launcher-create.ppm");
            launcher.frame({key("Return", true)});
            check(launcher.selection().has_value(),
                  "Create through keyboard rejected: " +
                      launcher.widgets().find("launcher-error")->text);
            check(launcher.selection()->create && launcher.selection()->dimension == 2 &&
                      launcher.selection()->language == "lua" &&
                      launcher.selection()->name == "Тестовый проект",
                  "Create through keyboard preserves typed project metadata");
            check(launcher.selection()->path == new_project,
                  "Create must normalize the typed project path: got " +
                      path_to_utf8(launcher.selection()->path) + ", expected " +
                      path_to_utf8(new_project));
            check(!std::filesystem::exists(new_project),
                  "Launcher does not create a partial project before Session scaffold");
        }
        {
            editor::ProjectSelection retry{new_project, "Retry starter", 3, true, "lua"};
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE), {},
                                             recents, retry, "Template source is unavailable");
            launcher.frame({});
            check(launcher.widgets().find("launcher-create")->selected &&
                      launcher.widgets().find("launcher-lua")->selected &&
                      launcher.widgets().find("launcher-path")->text == path_to_utf8(new_project) &&
                      launcher.widgets().find("launcher-name")->text == "Retry starter" &&
                      launcher.widgets().find("launcher-error")->text ==
                          "Template source is unavailable",
                  "Failed starter returns to Create with the chosen options and error");
        }
        {
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE),
                                             root / "Missing", recents);
            launcher.frame({});
            click(launcher, "launcher-submit");
            check(!launcher.selection() && !launcher.widgets().find("launcher-error")->text.empty(),
                  "Open validates missing directory");
            click(launcher, "launcher-recent-0");
            check(launcher.widgets().find("launcher-path")->text == path_to_utf8(existing),
                  "Recent selection fills actual path");
            click(launcher, "launcher-browse");
            check(launcher.widgets().find("launcher-browser")->visible,
                  "Native retained directory browser opens");
            text(launcher, "browser-path", path_to_utf8(root / "Absent folder"));
            launcher.frame({key("Return", true)});
            check(launcher.widgets().find("launcher-browser")->visible &&
                      !launcher.widgets().find("browser-choose")->enabled,
                  "Invalid typed directory cannot silently select prior directory");
            text(launcher, "browser-path", path_to_utf8(existing));
            click(launcher, "browser-up");
            check(launcher.widgets().find("browser-path")->text == path_to_utf8(root),
                  "Folder browser Up navigation");
            click(launcher, "browser-entry-0");
            check(launcher.widgets().find("browser-path")->text == path_to_utf8(existing),
                  "Directory list navigation");
            renderer.render(launcher.snapshot());
            renderer.capture(root / "launcher-browser.ppm");
            click(launcher, "browser-choose");
            check(!launcher.widgets().find("launcher-browser")->visible,
                  "Choose closes directory browser");
            launcher.frame({key("Return", true)});
            check(launcher.selection() && !launcher.selection()->create &&
                      launcher.selection()->path == existing &&
                      launcher.selection()->dimension == 2,
                  "Open reads project metadata");
        }
        {
            // Exercise the path returned by Create after the caller has created its
            // project, then reopen it through the launcher's UTF-8 text boundary.
            atomic_write_json(new_project / "project.faset.json", {{"format", "faset.project"},
                                                                   {"version", 1},
                                                                   {"name", "Тестовый проект"},
                                                                   {"dimension", 2}});
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE), {},
                                             recents);
            launcher.frame({});
            click(launcher, "launcher-open");
            text(launcher, "launcher-path", path_to_utf8(new_project / "project.faset.json"));
            launcher.frame({key("Return", true)});
            check(launcher.selection() && launcher.selection()->path == new_project &&
                      launcher.selection()->name == "Тестовый проект",
                  "Unicode project directory survives typed manifest path and reopen");
        }
        {
            const auto home = root / path_from_utf8("Дом пользователя");
            const auto config = root / path_from_utf8("Настройки пользователя");
            std::filesystem::create_directories(home);
#ifdef _WIN32
            ScopedPathEnvironment user_home(L"USERPROFILE", home), app_config(L"APPDATA", config);
            const auto recent_file = config / "Faset/recent-projects.json";
#else
            ScopedPathEnvironment user_home("HOME", home), app_config("XDG_CONFIG_HOME", config);
            const auto recent_file = config / "faset/recent-projects.json";
#endif
            editor::remember_project(existing);
            check(read_json(recent_file).at(0) == path_to_utf8(existing),
                  "Unicode config directory and recent project are serialized as UTF-8");
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE));
            launcher.frame({});
            check(launcher.widgets().find("launcher-path")->text ==
                      path_to_utf8(home / "FasetProjects/MyGame"),
                  "Unicode user home forms a native default project path");
            check(launcher.widgets().find("launcher-recent-0"),
                  "Default recents path reads the Unicode config directory");
            click(launcher, "launcher-browse");
            click(launcher, "browser-home");
            check(launcher.widgets().find("browser-path")->text == path_to_utf8(home),
                  "Home navigation preserves Unicode environment path");
            launcher.frame({key("Escape")});
        }
        {
            const auto corrupt = root / "Corrupt";
            atomic_write_json(corrupt / "project.faset.json",
                              {{"format", "faset.project"}, {"version", 9}, {"name", "Future"}});
            editor::ProjectLauncher launcher(renderer, path_from_utf8(FASET_TEST_ENGINE), corrupt,
                                             recents);
            launcher.frame({});
            click(launcher, "launcher-submit");
            check(!launcher.selection() && !launcher.widgets().find("launcher-error")->text.empty(),
                  "Unsupported project version is rejected");
            launcher.frame({key("Escape")});
            check(launcher.cancelled(), "Escape cancels launcher");
        }
        check(renderer.stats().validation_errors == 0, "Launcher Vulkan validation");
        std::cout
            << "Launcher Unicode/create/open/validation/recents/directory browser/keyboard passed. "
            << path_to_utf8(root) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nRetained: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
