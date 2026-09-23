#include <bit>
#include <algorithm>
#include <cmath>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/core/io.hpp>
#include <faset/player/SceneView.hpp>
#include <faset/runtime/Runtime.hpp>
#include <iostream>
#include <limits>
#include <numbers>
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
template <class F>
void rejectsContaining(F&& function, std::string_view entityId, std::string_view field) {
    try {
        function();
    } catch (const std::exception& error) {
        const std::string_view what(error.what());
        check(what.find(entityId) != std::string_view::npos &&
                  what.find(field) != std::string_view::npos,
              "Invalid light reports entity and field");
        return;
    }
    throw std::runtime_error("Invalid light was accepted");
}
Json component(std::string type, Json fields) {
    return {{"id", type}, {"type", type}, {"version", 1}, {"fields", fields}};
}
Json entity(std::string id, Json parent, Json components) {
    return {{"id", id}, {"name", id}, {"parent", parent}, {"components", components}};
}
void lightingExtraction(faset::player::SceneView& view) {
    auto scene = Json{{"format", "faset.scene"},
                      {"version", 1},
                      {"id", "lighting"},
                      {"name", "Lighting"},
                      {"dimension", 3},
                      {"instances", Json::array()},
                      {"entities", Json::array()}};
    const auto empty = view.build(scene, 16.f / 9.f);
    check(!empty.authored_lights_present && !empty.sun && empty.local_lights.empty(),
          "Scene with no lights leaves legacy sun fallback available");
    check(empty.camera_frustum && empty.camera_frustum->perspective &&
              empty.camera_frustum->near_plane > 0 &&
              empty.camera_frustum->far_plane > empty.camera_frustum->near_plane &&
              faset::render::multiply(empty.camera_frustum->projection,
                                      empty.camera_frustum->view) == empty.view_projection,
          "3D extraction retains an unjittered camera frustum");
    scene["entities"] = Json::array({
        entity("sun", nullptr,
               Json::array({component("faset.transform", {{"rotation", {0, .4, 0}}}),
                            component("faset.light", {{"kind", "directional"},
                                                      {"color", {1, .8, .6, 1}},
                                                      {"intensity", 2.5},
                                                      {"casts_shadow", false}})})),
        entity("point", nullptr,
               Json::array({component("faset.transform", {{"position", {2, 3, 4}}}),
                            component("faset.light", {{"kind", "point"},
                                                      {"color", {1, 0, 0, 1}},
                                                      {"intensity", 4},
                                                      {"range", 6},
                                                      {"shadow_priority", 3}})})),
        entity("spot", nullptr,
               Json::array({component("faset.transform", {{"position", {-2, 1, 0}}}),
                            component("faset.light", {{"kind", "spot"},
                                                      {"color", {0, 0, 1, 1}},
                                                      {"intensity", 3},
                                                      {"range", 8},
                                                      {"inner_angle", .2},
                                                      {"outer_angle", .6}})}))});
    const auto a = view.build(scene, 16.f / 9.f);
    check(a.authored_lights_present && a.sun && a.local_lights.size() == 2,
          "Directional, point, and spot lights survive extraction");
    check(a.sun->color[1] == .8f && a.sun->intensity == 2.5f && !a.sun->casts_shadow,
          "Authored sun properties survive extraction");
    check(a.local_lights[0].kind == faset::render::LocalLight::Kind::Point &&
              a.local_lights[0].position == faset::render::Vec3{2, 3, 4} &&
              a.local_lights[0].range == 6 && a.local_lights[0].shadow_priority == 3,
          "Point fields and transform survive extraction");
    check(a.local_lights[1].kind == faset::render::LocalLight::Kind::Spot &&
              a.local_lights[1].inner_angle == .2f && a.local_lights[1].outer_angle == .6f,
          "Spot cone survives extraction");
    std::reverse(scene["entities"].begin(), scene["entities"].end());
    const auto b = view.build(scene, 16.f / 9.f);
    check(a.sun->stable_id == b.sun->stable_id &&
              a.local_lights[0].stable_id == b.local_lights[0].stable_id &&
              a.local_lights[1].stable_id == b.local_lights[1].stable_id,
          "Light identity and ordering ignore entity array order");
    scene["entities"].erase(scene["entities"].begin() + 2);
    const auto localOnly = view.build(scene, 1);
    check(localOnly.authored_lights_present && !localOnly.sun &&
              localOnly.local_lights.size() == 2,
          "Local-only lighting does not synthesize a sun");
    scene["entities"] = Json::array({entity(
        "disabled-sun", nullptr,
        Json::array({component("faset.light", {{"kind", "directional"}, {"enabled", false}})}))});
    const auto disabled = view.build(scene, 1);
    check(disabled.authored_lights_present && !disabled.sun && disabled.local_lights.empty(),
          "Explicit disabled sun suppresses legacy fallback");
    scene["entities"][0]["components"][0]["version"] = 2;
    const auto future = view.build(scene, 1);
    check(future.authored_lights_present && !future.sun,
          "Opaque future-version light still suppresses legacy fallback");
    scene["entities"] = Json::array({
        entity("sun-z", nullptr, Json::array({component("faset.light", Json::object())})),
        entity("sun-a", nullptr, Json::array({component("faset.light", Json::object())}))});
    const auto twoSuns = view.build(scene, 1);
    check(twoSuns.sun && twoSuns.sun->stable_id.find("sun-a") != std::string::npos,
          "Multiple suns select lowest stable identity");
    check(!view.diagnostics().empty() &&
              view.diagnostics().front().find("directional") != std::string::npos,
          "Additional directionals produce an actionable diagnostic");
    scene["entities"] = Json::array({entity(
        "bad-light", nullptr,
        Json::array({component("faset.light", {{"kind", "point"}, {"range", 0}})}))});
    rejectsContaining([&] { view.build(scene, 1); }, "bad-light", "range");
    scene["entities"][0]["components"][0]["fields"] =
        {{"kind", "spot"}, {"range", 10}, {"inner_angle", .8}, {"outer_angle", .2}};
    rejectsContaining([&] { view.build(scene, 1); }, "bad-light", "inner_angle");
    scene["entities"][0]["components"][0]["fields"] = {{"kind", "area"}};
    rejectsContaining([&] { view.build(scene, 1); }, "bad-light", "kind");
    scene["entities"][0]["components"][0]["fields"] =
        {{"kind", "point"},
         {"color", Json::array({1, std::numeric_limits<double>::quiet_NaN(), 1, 1})}};
    rejectsContaining([&] { view.build(scene, 1); }, "bad-light", "color");
    scene["entities"][0]["components"].insert(
        scene["entities"][0]["components"].begin(),
        component("faset.transform", {{"position", {0, 0, 0}},
                                       {"scale", Json::array({1, std::numeric_limits<double>::quiet_NaN(), 1})}}));
    rejectsContaining([&] { view.build(scene, 1); }, "bad-light", "transform");
}
void physicsDebug(faset::player::SceneView& view) {
    for (int dimension : {2, 3}) {
        const std::string bodyName = dimension == 2 ? "rigid_body_2d" : "rigid_body_3d";
        Json body{{"body_type", "dynamic"},
                  {"half_extents", dimension == 2 ? Json{.75, .5} : Json{.75, .5, .25}},
                  {"linear_velocity", dimension == 2 ? Json{2, 0} : Json{2, 0, 0}}};
        const Json authoredPose{{"position", {1, 2, 3}},
                                {"rotation", {0, 0, std::numbers::pi_v<float> / 2}},
                                {"scale", {-2, 3, -4}}};
        Json scene{{"format", "faset.scene"},
                   {"version", 1},
                   {"id", "physics"},
                   {"name", "physics"},
                   {"dimension", dimension},
                   {"instances", Json::array()},
                   {"entities",
                    Json::array({entity("body", nullptr,
                                        Json::array({component("faset.transform", authoredPose),
                                                     component("faset." + bodyName, body)}))})}};
        faset::runtime::RuntimeConfig config;
        config.gravity = {0, 0, 0};
        faset::runtime::Runtime runtime(config);
        runtime.load(scene);
        runtime.advance(config.fixedDelta * 1.5);
        const auto handle = runtime.find("body");
        const auto physical = runtime.transform(handle);
        const auto displayed = runtime.presentation(handle);
        check(physical.position[0] > displayed.position[0] + .01f,
              "Fixture separates actual body pose from interpolated presentation");
        // This is the same explicit current-pose contract used by the Player;
        // snapshotJson's presentation pose is intentionally unsuitable here.
        Json physics{
            {"dimension", dimension},
            {"entities", Json::array({{{"parent", nullptr},
                                       {bodyName, runtime.fields(handle, "faset." + bodyName)},
                                       {"transform",
                                        {{"position", physical.position},
                                         {"rotation", physical.rotation},
                                         {"scale", physical.scale}}}}})}};
        faset::render::Snapshot debug;
        const auto originalCamera = debug.view_projection;
        view.appendPhysicsDebug(debug, physics, .02f);
        check(debug.draws.size() == (dimension == 2 ? 4 : 12), "Box outline edge count");
        check(debug.view_projection == originalCamera, "Debug overlay leaves camera unchanged");
        faset::render::Vec3 center{};
        for (const auto& edge : debug.draws) {
            check(!edge.cast_shadow && edge.mesh, "Debug edges are unshadowed meshes");
            for (const auto& vertex : edge.mesh->vertices)
                check(vertex.normal == faset::render::Vec3{0, 0, 0}, "Debug edge color is unlit");
            for (int axis = 0; axis < 3; ++axis)
                center[axis] += edge.model[12 + axis] / float(debug.draws.size());
        }
        for (int axis = 0; axis < 3; ++axis)
            check(std::abs(center[axis] - physical.position[axis]) < .0001f,
                  "Outlines center on current Box2D/Box3D pose");
        // Rz(pi/2) turns the first X edge into world Y. Abs(scale) gives a
        // three-unit X edge and 1.5-unit local Y half extent in both adapters.
        const auto& first = debug.draws.front().model;
        check(std::abs(first[0]) < .0001f && std::abs(first[1] - 3) < .0001f &&
                  std::abs(first[12] - physical.position[0] - 1.5f) < .0001f,
              "Negative nonuniform scale and box rotation match physics shape policy");
        if (dimension == 3)
            check(std::abs(first[14] - physical.position[2] + 1) < .0001f,
                  "Box3D Z half extent includes absolute Z scale");
        auto invalid = physics;
        invalid["entities"][0]["parent"] = "another-body";
        rejects([&] { view.appendPhysicsDebug(debug, invalid); },
                "Debug respects runtime root-only physical bodies");
        if (dimension == 2) {
            invalid = physics;
            invalid["entities"][0]["transform"]["rotation"][0] = .5;
            rejects([&] { view.appendPhysicsDebug(debug, invalid); },
                    "Box2D debug rejects rotation outside Z");
        }
        rejects([&] { view.appendPhysicsDebug(debug, physics, 0); },
                "Debug thickness must be positive");
        auto opaque = scene;
        opaque["entities"][0]["components"][1]["version"] = 2;
        opaque["entities"][0]["components"][1]["fields"] = "future representation";
        faset::render::Snapshot skipped;
        view.appendPhysicsDebug(skipped, opaque);
        check(skipped.draws.empty(), "Debug does not interpret unknown body schema versions");
    }
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
    lightingExtraction(view);
    physicsDebug(view);
    auto snapshot = view.build(scene, 16.f / 9.f);
    check(snapshot.draws.size() == 1, "SceneView builtin mesh");
    check(!snapshot.draws[0].instance_key.empty(),
          "Persistent scene entity gives its builtin mesh a stable render key");
    const auto builtin_key = snapshot.draws[0].instance_key;
    auto second_mesh = scene;
    second_mesh["entities"].push_back(
        entity("other-mesh", nullptr,
               Json::array({component("faset.mesh", {{"asset", "builtin:cube"}})})));
    const auto two_meshes = view.build(second_mesh, 1);
    check(two_meshes.draws.size() == 2 && two_meshes.draws[0].instance_key == builtin_key &&
              two_meshes.draws[1].instance_key != builtin_key,
          "A second instance of the same mesh has a distinct key without renumbering the first");
    std::swap(second_mesh["entities"][1], second_mesh["entities"][2]);
    const auto reordered_meshes = view.build(second_mesh, 1);
    check(reordered_meshes.draws[0].instance_key == two_meshes.draws[1].instance_key &&
              reordered_meshes.draws[1].instance_key == builtin_key,
          "Render instance identity follows entity identity, not draw order");
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
    auto opaque = scene;
    opaque["entities"][1]["components"][1]["version"] = 2;
    opaque["entities"][1]["components"][1]["fields"] = {{"asset", 42}, {"color", "future-format"}};
    const auto unchanged = opaque;
    check(view.build(opaque, 1).draws.empty(), "Future mesh fields are opaque in editor preview");
    check(!view.diagnostics().empty() &&
              view.diagnostics()[0].find("unsupported faset.mesh") != std::string::npos,
          "Opaque preview component has an actionable diagnostic");
    check(opaque == unchanged, "Preview preserves unknown component bytes");
    rejects([&] { world.load(opaque); }, "Player runtime rejects unknown component versions");
    opaque = scene;
    opaque["entities"][1]["components"][0]["version"] = 2;
    opaque["entities"][1]["components"][0]["fields"] = {{"position", "future-format"}};
    snapshot = view.build(opaque, 1);
    check(snapshot.draws[0].model[12] == 1 && snapshot.draws[0].model[13] == 2,
          "Future transform fields are not interpreted as v1 coordinates");
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
    for (const auto& diagnostic : view.diagnostics())
        std::cerr << "Cooked fixture: " << diagnostic << '\n';
    check(view.diagnostics().empty(), "valid cooked texture/material produces no error");
    check(snapshot.draws.size() == 1 && snapshot.draws[0].mesh->vertices.size() == 3,
          "cooked mesh reaches render snapshot");
    check(!snapshot.draws[0].instance_key.empty() &&
              snapshot.draws[0].instance_key != builtin_key &&
              view.build(importedScene, 1).draws[0].instance_key == snapshot.draws[0].instance_key,
          "Imported primitive identity is stable across scene extraction");
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
