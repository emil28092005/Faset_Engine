#include <faset/core/io.hpp>
#include <faset/editor/session.hpp>
#include <iostream>

int main() {
    using namespace faset;
    const auto root = std::filesystem::temp_directory_path() /
                      path_from_utf8("Faset Café 世界 session " + new_id());
    try {
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), {}});
        session.scaffold("Settings", 3);
        auto document = session.authoring().create("Start", 3);
        session.commands().call("faset_document_save", {{"document", document.at("id")},
                                                        {"path", "Scenes/Начало 世界.scene.json"}});
        require(session.authoring().query(document.at("id")).at("path") ==
                    "Scenes/Начало 世界.scene.json",
                "test", "Saved path must round-trip as UTF-8");
        require(std::filesystem::is_regular_file(root /
                                                 path_from_utf8("Scenes/Начало 世界.scene.json")),
                "test", "Unicode scene was saved under the wrong filename");
        session.commands().call("faset_document_open", {{"path", "Scenes/Начало 世界.scene.json"}});
        auto& commands = session.commands();
        const auto initial = commands.call("faset_project_settings_get", Json::object());
        auto changed = commands.call("faset_project_settings_set",
                                     {{"revision", initial.at("revision")},
                                      {"settings",
                                       {{"name", "Проект 世界"},
                                        {"dimension", 2},
                                        {"start_scene", "Scenes/Начало 世界.scene.json"}}}});
        require(changed.at("settings").at("name") == "Проект 世界" &&
                    session.project().at("dimension") == 2,
                "test", "Project settings were not saved");
        require(session.authoring().query(document.at("id")).at("scene").at("dimension") == 3,
                "test", "Project defaults changed the existing scene");
        auto rejects = [&](const Json& request, const std::string& code) {
            try {
                commands.call("faset_project_settings_set", request);
            } catch (const Error& error) {
                require(error.json().at("code") == code, "test", "Wrong project error");
                return;
            }
            throw std::runtime_error("Invalid project edit was accepted");
        };
        rejects({{"revision", initial.at("revision")}, {"settings", {{"name", "Stale"}}}},
                "revision.conflict");
        for (const auto& [field, value, code] :
             std::vector<std::tuple<std::string, Json, std::string>>{
                 {"name", "", "project.name"},
                 {"dimension", 4, "project.dimension"},
                 {"id", "replace-id", "project.setting"},
                 {"start_scene", "Scenes/missing.scene.json", "project.start_scene"}}) {
            rejects({{"revision", changed.at("revision")}, {"settings", {{field, value}}}}, code);
            require(commands.call("faset_project_settings_get", Json::object()) == changed, "test",
                    "Rejected project edit changed state");
        }
        auto external = session.project();
        external["custom_tool"] = {{"keep", true}};
        atomic_write_json(root / "project.faset.json", external);
        rejects({{"revision", changed.at("revision")}, {"settings", {{"name", "Race"}}}},
                "revision.conflict");
        const auto reloaded = commands.call("faset_project_settings_get", Json::object());
        changed =
            commands.call("faset_project_settings_set", {{"revision", reloaded.at("revision")},
                                                         {"settings", {{"name", "Preserved"}}}});
        require(changed.at("settings").at("custom_tool").at("keep") == true, "test",
                "Saving settings erased unknown project metadata");
        external["version"] = 999;
        atomic_write_json(root / "project.faset.json", external);
        bool rejected = false;
        try {
            session.project();
        } catch (const Error& error) {
            rejected = error.json().at("code") == "project.version";
        }
        require(rejected, "test", "Future project version was reinterpreted");
        std::filesystem::remove_all(root);
        std::cout << "Project settings validation, save, external revision conflicts and opaque "
                     "metadata passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
