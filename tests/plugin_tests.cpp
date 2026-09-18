#include "../examples/extensions/beacon/Beacon.hpp"
#include <faset/core/io.hpp>
#include <faset/editor/plugins.hpp>
#include <iostream>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Check failed: " #x);                                         \
    } while (false)
int main() {
    using namespace faset;
    using namespace faset::editor;
    const auto root = std::filesystem::temp_directory_path() / ("faset-plugins-" + new_id());
    try {
        std::filesystem::create_directories(root);
        const auto folder = root / "Plugins";
        std::filesystem::copy(FASET_TEST_PLUGIN_DIRECTORY, folder,
                              std::filesystem::copy_options::recursive);
        authoring::AuthoringService authoring(root);
        authoring.register_schemas(Json::array({beacon::schema()}));
        Commands commands(authoring);
        const auto document = authoring.create("Extension test");
        const auto id = document.at("id");
        {
            PluginManager plugins(commands, [](auto) {});
            plugins.load(folder);
            CHECK(plugins.status().size() == 1);
            CHECK(plugins.status()[0]["state"] == "loaded");
            CHECK(plugins.panels().size() == 1);
            const auto edited = commands.call("plugin_example_beacon_create", {{"document", id}});
            CHECK(edited["revision"] == 1);
            CHECK(edited["scene"]["entities"][0]["components"][2]["type"] == "example.beacon");
            authoring.undo(id, 1);
            CHECK(authoring.query(id)["scene"]["entities"].empty());
            authoring.redo(id, 2);
            authoring.save(id, "Scenes/plugin.scene.json");
            // The extension's runtime component runs without loading its Editor DLL/SO.
            runtime::Runtime world;
            beacon::register_behavior(world);
            world.load(edited.at("scene"));
            world.advance(1.0 / 60);
            CHECK(world.transform(world.find(edited["scene"]["entities"][0]["id"])).rotation[1] >
                  0);
        }
        for (const auto& command : commands.list())
            CHECK(command["name"] != "plugin_example_beacon_create");
        authoring::AuthoringService absent(root);
        CHECK(absent.open("Scenes/plugin.scene.json")["scene"]["entities"][0]["components"][2]
                                                     ["fields"]["speed"] == 1.0);
        const auto file = folder / "beacon.faset-plugin.json";
        const auto original = read_json(file);
        auto wrong = original;
        wrong["build_fingerprint"] = "wrong-build";
        atomic_write_json(file, wrong);
        {
            PluginManager plugins(commands, [](auto) {});
            plugins.load(folder);
            CHECK(plugins.status()[0]["state"] == "failed");
            CHECK(plugins.panels().empty());
        }
        auto cycle = original;
        cycle["dependencies"] = Json::array({{{"id", "example.beacon"}, {"version", "1.0.0"}}});
        atomic_write_json(file, cycle);
        {
            PluginManager plugins(commands, [](auto) {});
            plugins.load(folder);
            CHECK(plugins.status()[0]["state"] == "failed");
            CHECK(plugins.panels().empty());
        }
        auto missing = original;
        missing["dependencies"] = Json::array({{{"id", "missing"}, {"version", "1.0.0"}}});
        atomic_write_json(file, missing);
        {
            PluginManager plugins(commands, [](auto) {});
            plugins.load(folder);
            CHECK(plugins.status()[0]["state"] == "failed");
        }
        std::filesystem::remove_all(root);
        std::cout << "Native plugin ABI, ownership, commands, runtime component, missing package "
                     "preservation and dependency validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
