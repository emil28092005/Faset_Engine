#include <faset/core/io.hpp>
#include <faset/editor/session.hpp>
#include <chrono>
#include <iostream>
#include <thread>

int test_main(int argc, char** argv) {
    using namespace faset;
    if (argc >= 3 && std::string_view(argv[1]) == "--editor-probe") {
        Json arguments = Json::array();
        for (int index = 3; index < argc; ++index)
            arguments.push_back(argv[index]);
        atomic_write_json(path_from_utf8(argv[2]), arguments);
        return 0;
    }
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
        const auto play = commands.call("faset_play", {{"document", document.at("id")}});
        require(play.value("play_pending", false) && session.play_pending() && !session.playing(),
                "test", "Play must expose its cancellable build phase before Player starts");
        commands.call("faset_stop", Json::object());
        require(!session.play_pending() && !session.playing(), "test",
                "Stop must cancel a pending Play build");
        const auto initial = commands.call("faset_project_settings_get", Json::object());
        require(initial.at("settings").at("editor").at("autosave") == true, "test",
                "Missing autosave setting must read as enabled");
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
        external["scripting"] = {{"lua", {{"scripts", Json::array()}}}};
        external["editor"] = {{"script_editor", {"zed", "{file}"}}};
        atomic_write_json(root / "project.faset.json", external);
        rejects({{"revision", changed.at("revision")}, {"settings", {{"name", "Race"}}}},
                "revision.conflict");
        auto reloaded = commands.call("faset_project_settings_get", Json::object());
        auto explicit_default = external;
        explicit_default["editor"]["autosave"] = true;
        atomic_write_json(root / "project.faset.json", explicit_default);
        rejects({{"revision", reloaded.at("revision")},
                 {"settings", {{"name", "Lost external write"}}}},
                "revision.conflict");
        reloaded = commands.call("faset_project_settings_get", Json::object());
        changed =
            commands.call("faset_project_settings_set", {{"revision", reloaded.at("revision")},
                                                         {"settings", {{"name", "Preserved"}}}});
        require(changed.at("settings").at("custom_tool").at("keep") == true, "test",
                "Saving settings erased unknown project metadata");
        require(changed.at("settings").at("scripting") == external.at("scripting") &&
                    changed.at("settings").at("editor").at("script_editor") ==
                        external.at("editor").at("script_editor"),
                "test", "Project settings erased Lua configuration or external editor command");
        const auto autosave_off = commands.call(
            "faset_project_settings_set",
            {{"revision", changed.at("revision")},
             {"settings", {{"editor", {{"autosave", false}}}}}});
        require(autosave_off.at("settings").at("editor").at("autosave") == false &&
                    session.project().at("editor").at("script_editor") ==
                        external.at("editor").at("script_editor"),
                "test", "Autosave setting must merge without erasing the editor command");
        session.poll();
        require(commands.call("faset_autosave_status", Json::object()).at("enabled") == false,
                "test", "Autosave disable must take effect in the current session");
        {
            editor::Session reopened({root, path_from_utf8(FASET_TEST_ENGINE), {}});
            reopened.poll();
            require(reopened.commands()
                            .call("faset_autosave_status", Json::object())
                            .at("enabled") == false,
                    "test", "Autosave setting must survive Editor reopen");
        }
        rejects({{"revision", changed.at("revision")},
                 {"settings", {{"editor", {{"autosave", true}}}}}},
                "revision.conflict");
        rejects({{"revision", autosave_off.at("revision")},
                 {"settings", {{"editor", {{"autosave", "sometimes"}}}}}},
                "project.autosave");
        rejects({{"revision", autosave_off.at("revision")},
                 {"settings", {{"editor", {{"script_editor", {"other"}}}}}}},
                "project.setting");
        changed = commands.call("faset_project_settings_set",
                                {{"revision", autosave_off.at("revision")},
                                 {"settings", {{"editor", {{"autosave", true}}}}}});
        session.poll();
        require(commands.call("faset_autosave_status", Json::object()).at("enabled") == true,
                "test", "Autosave enable must take effect in the current session");
        const auto setup = commands.call("faset_lua_setup", Json::object());
        require(setup.at("configuration_created") == true &&
                    setup.at("scripts_configuration_created") == true &&
                    std::filesystem::is_regular_file(root / ".faset/lua/faset.lua") &&
                    read_json(root / ".luarc.json").at("runtime.version") == "Lua 5.4",
                "test", "LuaLS setup did not install annotations and configuration");
        require(read_json(root / "Scripts/.luarc.json").at("workspace.library") ==
                    Json::array({"../.faset/lua"}),
                "test", "Single-file Lua workspace cannot find Faset annotations");
        const Json custom_luarc = {{"runtime.version", "Lua 5.4"},
                                   {"workspace.library", {"Custom/Lua"}},
                                   {"custom_setting", true}};
        const Json custom_scripts_luarc = {{"workspace.library", {"../Custom/Lua"}}};
        atomic_write_json(root / ".luarc.json", custom_luarc);
        atomic_write_json(root / "Scripts/.luarc.json", custom_scripts_luarc);
        const auto repeated_setup = commands.call("faset_lua_setup", Json::object());
        require(repeated_setup.at("configuration_created") == false &&
                    repeated_setup.at("scripts_configuration_created") == false &&
                    read_json(root / ".luarc.json") == custom_luarc &&
                    read_json(root / "Scripts/.luarc.json") == custom_scripts_luarc,
                "test", "LuaLS setup overwrote the user's configuration");
        auto reject_command = [&](const std::string& name, const Json& args,
                                  const std::string& code) {
            try {
                commands.call(name, args);
            } catch (const Error& error) {
                require(error.json().at("code") == code, "test",
                        "Unexpected error for invalid Lua editor command");
                return;
            }
            throw std::runtime_error("Invalid Lua editor command was accepted");
        };
        reject_command("faset_lua_refresh", Json::object(), "lua.disabled");
        reject_command("faset_lua_reload", Json::object(), "play.not_running");
        reject_command("faset_script_open", {{"path", "Scripts/Gameplay.cpp"}}, "lua.source");
        atomic_write(root / "Scripts/behavior.lua", "return faset.behavior { id = 'game.test' }\n");
        reject_command("faset_script_open",
                       {{"path", "Scripts/behavior.lua"}, {"editor", {"{file}"}}}, "lua.editor");
        reject_command("faset_script_open", {{"path", "Scripts/behavior.lua"}, {"editor", {""}}},
                       "lua.editor");
        const auto probe = root / "editor-argv.json";
        const auto opened = commands.call(
            "faset_source_open",
            {{"path", "Scripts/Gameplay.cpp"}, {"line", 17}, {"column", 4},
             {"editor", Json::array({path_to_utf8(std::filesystem::absolute(path_from_utf8(argv[0]))),
                                     "--editor-probe", path_to_utf8(probe),
                                     "{file}:{line}:{column}", "{project}"})}});
        for (int attempt = 0; attempt < 100 && !std::filesystem::exists(probe); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(opened.at("path") == "Scripts/Gameplay.cpp" && opened.at("line") == 17 &&
                    std::filesystem::is_regular_file(probe) &&
                    read_json(probe) ==
                        Json::array({path_to_utf8(root / "Scripts/Gameplay.cpp") + ":17:4",
                                     path_to_utf8(root)}),
                "test", "Source navigation passes a Unicode path and location as literal argv");
        reject_command("faset_source_open", {{"path", "../outside.cpp"}}, "source.path");
        reject_command("faset_source_open", {{"path", "Scripts/missing.cpp"}}, "source.path");
        reject_command("faset_source_open", {{"path", "Scripts"}}, "source.path");
        atomic_write(root / "Scripts/tool.exe", "not a source\n");
        reject_command("faset_source_open", {{"path", "Scripts/tool.exe"}}, "source.path");
        std::filesystem::create_directories(root / "Outside");
        std::error_code link_error;
        std::filesystem::create_symlink(root / "Outside/secret.cpp",
                                        root / "Scripts/escape.cpp", link_error);
        if (!link_error)
            reject_command("faset_source_open", {{"path", "Scripts/escape.cpp"}}, "source.path");
        auto bad_lua_project = session.project();
        bad_lua_project["scripting"] = {{"lua", {{"scripts", {"Scripts/missing.lua"}}}}};
        atomic_write_json(root / "project.faset.json", bad_lua_project);
        const auto invalid_lua_status = commands.call("faset_schema_status", Json::object());
        require(invalid_lua_status.at("stale") == true &&
                    !invalid_lua_status.at("error").get<std::string>().empty(),
                "test", "Invalid Lua manifest did not mark schema stale with diagnostics");
        const auto autosave_id = document.at("id").get<std::string>();
        const auto autosave_revision =
            session.authoring().query(autosave_id).at("revision").get<std::uint64_t>();
        session.authoring().transact(
            autosave_id, autosave_revision,
            Json::array({{{"op", "scene.rename"}, {"name", "Saved by MCP polling"}}}));
        reject_command("faset_document_save",
                       {{"document", autosave_id},
                        {"expected_revision", autosave_revision}},
                       "revision.conflict");
        session.poll();
        require(commands.call("faset_autosave_status", Json::object()).at("enabled") == true,
                "test", "Autosave status is available through Editor commands");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        session.poll();
        require(read_json(root / path_from_utf8("Scenes/Начало 世界.scene.json")).at("name") ==
                    "Saved by MCP polling",
                "test", "A long-lived headless Editor poll saves a named scene after idle");
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
        std::cout << "Project settings validation, revision conflicts, opaque metadata, LuaLS "
                     "configuration preservation and Lua editor commands passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run_utf8_main(argc, argv, test_main);
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
