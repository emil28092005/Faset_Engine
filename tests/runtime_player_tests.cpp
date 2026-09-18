#include <bit>
#include <cmath>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/core/io.hpp>
#include <faset/player/SceneView.hpp>
#include <faset/runtime/Runtime.hpp>
#include <iostream>
#include <stdexcept>

using Json = nlohmann::json;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F&& function, const char* message) {
    bool caught = false;
    try {
        function();
    } catch (const std::exception&) {
        caught = true;
    }
    check(caught, message);
}
Json component(std::string type, Json fields) {
    return {{"id", type}, {"type", type}, {"version", 1}, {"fields", fields}};
}
Json entity(std::string id, Json parent, Json components) {
    return {{"id", id}, {"name", id}, {"parent", parent}, {"components", components}};
}
void run() {
    const auto folder =
        std::filesystem::temp_directory_path() / ("faset-player-test-" + faset::new_id());
    std::filesystem::create_directories(folder);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    Json scene{
        {"format", "faset.scene"},
        {"version", 1},
        {"id", "test"},
        {"name", "Test"},
        {"dimension", 3},
        {"instances", Json::array()},
        {"entities",
         Json::array(
             {entity("parent", nullptr,
                     Json::array({component("faset.transform", {{"position", {1, 2, 3}}})})),
              entity("child", "parent",
                     Json::array({component("faset.transform", {{"position", {2, 0, 0}}}),
                                  component("faset.mesh", {{"asset", "builtin:cube"}})}))})}};
    faset::atomic_write_json(folder / "scene.json", scene);
    check(faset::player::readScene(folder / "scene.json") == scene, "JSON scene roundtrip");
    const auto cbor = Json::to_cbor(scene);
    std::string cooked = "FASETSCN";
    for (int i = 0; i < 4; ++i)
        cooked.push_back(static_cast<char>(std::uint32_t{1} >> (8 * i)));
    for (int i = 0; i < 8; ++i)
        cooked.push_back(static_cast<char>(std::uint64_t(cbor.size()) >> (8 * i)));
    cooked.append(reinterpret_cast<const char*>(cbor.data()), cbor.size());
    faset::atomic_write(folder / "scene.fscene", cooked);
    check(faset::player::readScene(folder / "scene.fscene") == scene,
          "CBOR cooked scene roundtrip");
    auto truncated = cooked.substr(0, cooked.size() - 1);
    faset::atomic_write(folder / "truncated.fscene", truncated);
    rejects([&] { faset::player::readScene(folder / "truncated.fscene"); },
            "reject cooked size mismatch");
    auto version = cooked;
    version[8] = 2;
    faset::atomic_write(folder / "version.fscene", version);
    rejects([&] { faset::player::readScene(folder / "version.fscene"); }, "reject cooked version");
    faset::player::SceneView view(folder);
    auto snapshot = view.build(scene, 16.f / 9.f);
    check(snapshot.draws.size() == 1, "SceneView builtin mesh");
    check(snapshot.draws[0].model[12] == 3 && snapshot.draws[0].model[13] == 2 &&
              snapshot.draws[0].model[14] == 3,
          "hierarchy local transforms composed");
    faset::runtime::Runtime world;
    world.load(scene);
    auto pose = world.transform(world.find("child"));
    pose.position[0] = 5;
    world.setTransform(world.find("child"), pose);
    snapshot = view.build(world.snapshotJson(), 1);
    check(snapshot.draws[0].model[12] == 6,
          "runtime presentation overrides original authoring pose");
    auto bad = scene;
    bad["entities"][0]["parent"] = "child";
    rejects([&] { view.build(bad, 1); }, "view rejects hierarchy cycle");
    bad = scene;
    bad["entities"][1]["components"][1]["fields"]["asset"] = "missing-asset";
    view.build(bad, 1);
    check(!view.diagnostics().empty() && view.diagnostics()[0].starts_with("error:"),
          "missing asset is diagnostic, not silent success");
    auto camera = faset::player::CameraSettings{};
    camera.eye = camera.target;
    rejects([&] { view.build(scene, 1, camera); }, "reject degenerate camera");
    auto two = scene;
    two["dimension"] = 2;
    two["entities"] = Json::array(
        {entity("high", nullptr,
                Json::array({component("faset.sprite", {{"layer", 10}, {"color", {1, 0, 0, 1}}})})),
         entity(
             "low", nullptr,
             Json::array({component("faset.sprite", {{"layer", 0}, {"color", {0, 1, 0, 1}}})}))});
    snapshot = view.build(two, 1);
    check(snapshot.sprites.size() == 2 && snapshot.sprites[0].color[1] == 1,
          "2D sprites sorted by layer");

    // Import a real textured glTF fixture, then consume only its cooked generation.
    std::string geometry;
    auto word = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            geometry.push_back(static_cast<char>(v >> (8 * i)));
    };
    for (float value : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
        word(std::bit_cast<std::uint32_t>(value));
    for (char value : {0, 0, 1, 0, 2, 0})
        geometry.push_back(value);
    faset::atomic_write(folder / "geometry.bin", geometry);
    const std::vector<unsigned char> png{
        137, 80,  78, 71, 13, 10,  26,  10, 0,   0,  0,  13,  73,  72,  68,  82,  0, 0,
        0,   1,   0,  0,  0,  1,   8,   6,  0,   0,  0,  31,  21,  196, 137, 0,   0, 0,
        13,  73,  68, 65, 84, 120, 156, 99, 248, 16, 32, 242, 31,  0,   5,   220, 2, 84,
        184, 210, 98, 74, 0,  0,   0,   0,  73,  69, 78, 68,  174, 66,  96,  130};
    faset::atomic_write(folder / "pixel.png",
                        std::string_view(reinterpret_cast<const char*>(png.data()), png.size()));
    Json gltf = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
        {"nodes", Json::array({{{"mesh", 0}, {"translation", {2, 0, 0}}}})},
        {"buffers", Json::array({{{"uri", "geometry.bin"}, {"byteLength", 42}}})},
        {"bufferViews", Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}},
                                     {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 6}}})},
        {"accessors",
         Json::array(
             {{{"bufferView", 0},
               {"componentType", 5126},
               {"count", 3},
               {"type", "VEC3"},
               {"min", {0, 0, 0}},
               {"max", {1, 1, 0}}},
              {{"bufferView", 1}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}})},
        {"meshes", Json::array({{{"primitives", Json::array({{{"attributes", {{"POSITION", 0}}},
                                                              {"indices", 1},
                                                              {"material", 0}}})}}})},
        {"materials", Json::array({{{"pbrMetallicRoughness",
                                     {{"baseColorFactor", {1, 1, 1, 1}},
                                      {"baseColorTexture", {{"index", 0}}},
                                      {"metallicFactor", 0},
                                      {"roughnessFactor", 0.5}}}}})},
        {"images", Json::array({{{"uri", "pixel.png"}}})},
        {"textures", Json::array({{{"source", 0}}})}};
    faset::atomic_write_json(folder / "triangle.gltf", gltf);
    faset::assets::AssetPipeline pipeline(folder);
    auto imported = pipeline.import_asset({folder / "triangle.gltf"});
    check(imported.ok(), "real textured glTF fixture import");
    auto importedScene = scene;
    importedScene["entities"][1]["components"][1]["fields"]["asset"] = imported.asset_id;
    snapshot = view.build(importedScene, 1);
    check(view.diagnostics().empty(), "valid cooked texture/material produces no error");
    check(snapshot.draws.size() == 1 && snapshot.draws[0].mesh->vertices.size() == 3,
          "cooked mesh reaches render snapshot");
    check(snapshot.draws[0].model[12] == 5, "asset node transform composed with scene hierarchy");
    check(snapshot.draws[0].texture && snapshot.draws[0].texture->srgb &&
              snapshot.draws[0].texture->rgba == std::vector<std::uint8_t>({240, 80, 20, 255}),
          "PNG decoded into sRGB base-color texture");
}
} // namespace
int main() {
    try {
        run();
        std::cout << "Player JSON/CBOR, hierarchy, runtime snapshot, camera, sprite ordering and "
                     "asset error contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
