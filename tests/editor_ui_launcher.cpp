#include <faset/core/io.hpp>
#include <faset/editor/project_launcher.hpp>
#include <iostream>
using namespace faset;
namespace {
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
    const auto root = std::filesystem::temp_directory_path() / ("faset-launcher-ui-" + new_id());
    try {
        const auto existing = root / "Existing project";
        atomic_write_json(existing / "project.faset.json", {{"format", "faset.project"},
                                                            {"version", 1},
                                                            {"name", "Existing project"},
                                                            {"dimension", 2}});
        const auto recents = root / "recent-projects.json";
        atomic_write_json(recents, Json::array({existing.string(), (root / "Missing").string(),
                                                existing.string()}));
        render::Renderer renderer({1100, 720, "Project launcher acceptance", true, true});
        {
            editor::ProjectLauncher launcher(renderer, FASET_TEST_ENGINE, {}, recents);
            launcher.frame({});
            check(launcher.widgets().find("launcher-create")->selected,
                  "No initial path starts Create");
            check(launcher.widgets().find("launcher-recent-0") &&
                      !launcher.widgets().find("launcher-recent-1"),
                  "Recent list includes real valid unique projects only");
            text(launcher, "launcher-name", "Тестовый проект");
            text(launcher, "launcher-path", existing.string());
            click(launcher, "launcher-submit");
            check(!launcher.selection() && !launcher.widgets().find("launcher-error")->text.empty(),
                  "Create must reject nonempty existing project");
            text(launcher, "launcher-path", (root / "New Game").string());
            click(launcher, "launcher-2d");
            check(launcher.widgets().find("launcher-2d")->selected, "2D project selection");
            renderer.render(launcher.snapshot());
            renderer.capture(root / "launcher-create.ppm");
            launcher.frame({key("Return", true)});
            check(launcher.selection() && launcher.selection()->create &&
                      launcher.selection()->dimension == 2 &&
                      launcher.selection()->name == "Тестовый проект",
                  "Create through keyboard returns typed selection");
            check(!std::filesystem::exists(root / "New Game"),
                  "Launcher does not create a partial project before Session scaffold");
        }
        {
            editor::ProjectLauncher launcher(renderer, FASET_TEST_ENGINE, root / "Missing",
                                             recents);
            launcher.frame({});
            click(launcher, "launcher-submit");
            check(!launcher.selection() && !launcher.widgets().find("launcher-error")->text.empty(),
                  "Open validates missing directory");
            click(launcher, "launcher-recent-0");
            check(launcher.widgets().find("launcher-path")->text == existing.string(),
                  "Recent selection fills actual path");
            click(launcher, "launcher-browse");
            check(launcher.widgets().find("launcher-browser")->visible,
                  "Native retained directory browser opens");
            text(launcher, "browser-path", (root / "Absent folder").string());
            launcher.frame({key("Return", true)});
            check(launcher.widgets().find("launcher-browser")->visible &&
                      !launcher.widgets().find("browser-choose")->enabled,
                  "Invalid typed directory cannot silently select prior directory");
            text(launcher, "browser-path", existing.string());
            click(launcher, "browser-up");
            check(launcher.widgets().find("browser-path")->text == root.string(),
                  "Folder browser Up navigation");
            click(launcher, "browser-entry-0");
            check(launcher.widgets().find("browser-path")->text == existing.string(),
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
            const auto corrupt = root / "Corrupt";
            atomic_write_json(corrupt / "project.faset.json",
                              {{"format", "faset.project"}, {"version", 9}, {"name", "Future"}});
            editor::ProjectLauncher launcher(renderer, FASET_TEST_ENGINE, corrupt, recents);
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
            << root << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nRetained: " << root << '\n';
        return 1;
    }
}
