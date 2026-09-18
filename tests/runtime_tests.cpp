#include "Gameplay.hpp"
#include <cmath>
#include <faset/runtime/Runtime.hpp>
#include <faset/runtime/schema.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace faset::runtime;
using Json = nlohmann::json;
namespace {
void check(bool result, const char* text) {
    if (!result)
        throw std::runtime_error(text);
}
void near(float actual, float expected, float tolerance, const char* text) {
    check(std::abs(actual - expected) < tolerance, text);
}
template <class F> void rejects(F&& fn, const char* message) {
    bool caught = false;
    try {
        fn();
    } catch (const std::exception&) {
        caught = true;
    }
    check(caught, message);
}
Json component(std::string type, Json fields = Json::object()) {
    return {{"id", type + "-id"}, {"type", type}, {"version", 1}, {"fields", fields}};
}
Json entity(std::string id, float y = 0) {
    return {{"id", id},
            {"name", id},
            {"parent", nullptr},
            {"components", Json::array({component("faset.transform", {{"position", {0, y, 0}}})})}};
}
Json scene(int dim = 2) {
    return {{"format", "faset.scene"},   {"version", 1},
            {"id", "test-scene"},        {"name", "Test"},
            {"dimension", dim},          {"entities", Json::array()},
            {"instances", Json::array()}};
}
void physics(int dimension) {
    Runtime world;
    auto doc = scene(dimension);
    auto floor = entity("ground", -0.5f);
    auto falling = entity("falling", 4);
    auto type = dimension == 2 ? "faset.rigid_body_2d" : "faset.rigid_body_3d";
    Json extents = dimension == 2 ? Json{10, 0.5} : Json{10, 0.5, 10};
    floor["components"].push_back(
        component(type, {{"body_type", "static"}, {"half_extents", extents}}));
    falling["components"].push_back(component(type));
    doc["entities"] = Json::array({floor, falling});
    world.load(doc);
    auto h = world.find("falling");
    bool contact = false;
    for (int i = 0; i < 240; ++i) {
        world.advance(1.0 / 60);
        for (const auto& event : world.collisions())
            contact = contact || event.began;
    }
    near(world.transform(h).position[1], 0.5f, 0.09f,
         "body must fall and settle on actual solver floor");
    check(contact, "native contact event must be delivered");
    check(world.grounded(h), "settled body must have a supporting native contact");
    auto pose = world.transform(h);
    pose.position[1] = 6;
    world.teleport(h, pose);
    near(world.presentation(h).position[1], 6, 0.0001f, "teleport resets interpolation");
    check(!world.grounded(h), "teleport off the floor removes grounded state");
    rejects([&] { world.setTransform(h, pose); },
            "physics transform cannot be casually overwritten");
    world.applyImpulse(h, {0, 2, 0});
    check(world.velocity(h)[1] > 0, "impulse changes solver velocity");
}
void lifecycle() {
    Runtime world;
    std::vector<std::string> events;
    bool spawned = false;
    float presented = -1;
    int pressedTicks = 0;
    Behavior behavior;
    behavior.onStart = [&](Runtime&, EntityHandle, double) { events.push_back("start"); };
    behavior.fixedUpdate = [&](Runtime& r, EntityHandle h, double) {
        events.push_back("fixed");
        if (r.input().jumpPressed)
            ++pressedTicks;
        auto t = r.transform(h);
        t.position[0] += 1;
        r.setTransform(h, t);
        if (!spawned) {
            r.spawn(entity("spawned"));
            spawned = true;
        }
    };
    behavior.update = [&](Runtime&, EntityHandle, double) { events.push_back("update"); };
    behavior.lateUpdate = [&](Runtime& r, EntityHandle h, double) {
        events.push_back("late");
        presented = r.presentation(h).position[0];
    };
    behavior.onDestroy = [&](Runtime& r, EntityHandle h, double) {
        check(r.valid(h), "OnDestroy still sees a valid handle");
        events.push_back("destroy");
    };
    world.registerBehavior("test.behavior", behavior);
    auto doc = scene();
    auto object = entity("main");
    object["components"].push_back(component("test.behavior"));
    doc["entities"].push_back(object);
    world.load(doc);
    check(events == std::vector<std::string>{"start"}, "load runs OnStart once");
    world.advance(1.0 / 120, {0, 0, true, false});
    check(!world.find("spawned"), "zero-tick frame applies no structural commands");
    world.advance(1.0 / 60);
    check(!world.find("spawned"), "FixedUpdate spawn must wait until next tick");
    near(presented, 0.5f, 0.001f, "LateUpdate receives interpolated transform");
    check(pressedTicks == 1, "input edge preserved across zero-tick frame");
    world.advance(3.0 / 60);
    check(bool(world.find("spawned")), "spawn appears next tick");
    check(pressedTicks == 1, "edge not repeated in catchup ticks");
    auto old = world.find("main");
    world.destroy(old);
    check(world.valid(old), "destroy deferred");
    world.singleStep();
    check(!world.valid(old), "handle invalid after removal");
    check(events.back() == "destroy", "destroy lifecycle runs exactly at barrier");
    world.spawn(entity("replacement"));
    world.singleStep();
    check(!world.valid(old), "reused slot never revives a stale handle");
    auto replacement = world.find("replacement");
    world.load(doc);
    check(!world.valid(replacement), "load creates a new session");
    // Ensure captured state remains alive while Runtime's destructor calls OnDestroy.
    world.clear();
}
void clockAndValidation() {
    Runtime world;
    auto doc = scene();
    doc["entities"].push_back(entity("object"));
    world.load(doc);
    auto stats = world.advance(1.0);
    check(stats.fixedTicks == 4, "catchup bounded to four ticks");
    check(stats.droppedTime > 0.9, "excess time reported");
    check(stats.interpolationAlpha >= 0 && stats.interpolationAlpha < 1,
          "interpolation fraction bounded");
    auto tick = stats.tick;
    world.setPaused(true);
    world.advance(100);
    check(world.snapshot().tick == tick, "pause does not accumulate");
    world.singleStep();
    check(world.snapshot().tick == tick + 1, "single-step advances exactly once");
    world.setPaused(false);
    check(world.advance(0).fixedTicks == 0, "resume does not catch up pause");
    auto old = world.find("object");
    auto invalid = doc;
    invalid["entities"][0]["parent"] = "object";
    rejects([&] { world.load(invalid); }, "reject hierarchy cycle");
    check(world.valid(old), "invalid load preserves old world");
    invalid = doc;
    invalid["entities"][0]["components"].push_back(component("faset.rigid_body_3d"));
    rejects([&] { world.load(invalid); }, "reject physics dimension mismatch");
    rejects([&] { world.advance(-1); }, "reject negative time");
    world.addComponent(old, component("faset.sprite"));
    check(!world.snapshot().entities[0].sprite, "component addition deferred");
    world.singleStep();
    check(world.snapshot().entities[0].sprite.has_value(), "component added at barrier");
    world.removeComponent(old, "faset.sprite");
    world.singleStep();
    check(!world.snapshot().entities[0].sprite, "component removed at barrier");
    Runtime other;
    other.load(doc);
    check(!other.valid(old), "handle cannot cross worlds");
    auto schema = faset::gameplay::schema();
    check(schema.size() == 2, "sample has explicit metadata without world");
}
void componentSchemaVersions() {
    auto doc = scene();
    auto object = entity("versioned");
    auto custom =
        component("test.versioned", {{"value", 42}, {"future_payload", {"kept", "opaque"}}});
    custom["version"] = 2;
    object["components"].push_back(custom);
    auto data = component("test.data", {{"description", "no behavior required"}});
    data["version"] = 3;
    object["components"].push_back(data);
    doc["entities"].push_back(object);
    const auto source_before = doc;
    const auto metadata =
        Json::array({{{"id", "test.versioned"}, {"version", 2}, {"fields", Json::object()}},
                     {{"id", "test.data"}, {"version", 3}, {"fields", Json::object()}}});
    validate_scene_schemas(doc, metadata);
    validate_scene_schemas(doc, Json{{"types", metadata}});
    check(doc == source_before, "schema validation must not migrate or mutate source");
    int starts = 0;
    Runtime world;
    Behavior versioned;
    versioned.onStart = [&](Runtime& runtime, EntityHandle handle, double) {
        ++starts;
        check(runtime.fields(handle, "test.versioned") == custom["fields"],
              "versioned behavior receives exact opaque fields");
    };
    world.registerBehavior("test.versioned", versioned);
    validate_scene_schemas(doc, metadata);
    check(starts == 0, "schema validation does not invoke gameplay");
    world.load(doc);
    const auto handle = world.find("versioned");
    check(starts == 1 && world.fields(handle, "test.data") == data["fields"],
          "Runtime accepts positive custom versions including data-only components");
    auto added = component("test.added", {{"value", "deferred"}});
    added["version"] = 4;
    world.addComponent(handle, added);
    world.singleStep();
    check(world.fields(handle, "test.added") == added["fields"],
          "deferred custom component accepts its positive version");
    for (const auto& invalid_version : Json::array({0, -1, 1.5, "2", true, nullptr})) {
        auto invalid = doc;
        invalid["entities"][0]["components"][1]["version"] = invalid_version;
        rejects([&] { world.load(invalid); }, "custom version must be a positive integer");
        rejects([&] { validate_scene_schemas(invalid, metadata); },
                "schema gate rejects malformed component version");
        check(world.valid(handle), "invalid version preserves running world");
    }
    for (const std::string builtin :
         {"faset.transform", "faset.sprite", "faset.mesh", "faset.camera", "faset.light",
          "faset.rigid_body_2d", "faset.rigid_body_3d"}) {
        auto invalid = scene();
        auto native = component(builtin);
        native["version"] = 2;
        invalid["entities"].push_back({{"id", "native"}, {"components", Json::array({native})}});
        rejects([&] { world.load(invalid); },
                "all builtin components retain strict native version 1");
        rejects([&] { validate_scene_schemas(invalid, metadata); },
                "schema gate rejects builtin v2");
        check(world.valid(handle), "invalid builtin load preserves running world");
    }
    auto bad_add = component("faset.camera");
    bad_add["version"] = 2;
    world.addComponent(handle, bad_add);
    world.singleStep();
    rejects([&] { world.fields(handle, "faset.camera"); },
            "invalid deferred builtin version cannot enter the world");
    rejects([&] { validate_scene_schemas(doc, Json::array()); },
            "Player schema gate rejects missing gameplay module metadata");
    auto mismatched = metadata;
    mismatched[0]["version"] = 1;
    rejects([&] { validate_scene_schemas(doc, mismatched); },
            "Player schema gate rejects gameplay version mismatch");
    auto unknown = doc;
    unknown["entities"][0]["components"][1]["type"] = "test.missing";
    unknown["entities"][0]["components"][1]["version"] = 1;
    rejects([&] { validate_scene_schemas(unknown, metadata); },
            "Unknown custom v1 must not silently run in Player");
    auto duplicate = metadata;
    duplicate.push_back(metadata[0]);
    rejects([&] { validate_scene_schemas(doc, duplicate); },
            "Ambiguous duplicate metadata TypeIds rejected");
    for (int version : {1, 2}) {
        auto overridden_native = metadata;
        overridden_native.push_back({{"id", "faset.transform"}, {"version", version}});
        rejects([&] { validate_scene_schemas(doc, overridden_native); },
                "Gameplay cannot redeclare native TypeIds, even at native version 1");
    }
    auto invalid_schema = metadata;
    invalid_schema[0]["version"] = "2";
    rejects([&] { validate_scene_schemas(doc, invalid_schema); },
            "Schema versions also require positive integers");
    rejects([&] { validate_scene_schemas(doc, Json::object()); },
            "Malformed schema manifest rejected");
    auto legacy = scene();
    legacy["entities"].push_back(entity("legacy"));
    legacy["entities"][0]["components"][0].erase("version");
    validate_scene_schemas(legacy, Json::array());
    world.load(legacy);
    check(bool(world.find("legacy")),
          "Omitted component version retains development scene v1 compatibility");
}
void structuralFailuresAndCallbacks() {
    Runtime world;
    auto doc = scene();
    doc["entities"].push_back(entity("object"));
    world.load(doc);
    auto h = world.find("object");
    world.addComponent(h, component("faset.sprite", {{"size", {-1, 2}}}));
    world.singleStep();
    check(!world.snapshot().entities[0].sprite,
          "invalid deferred component leaves entity unchanged");
    check(!world.diagnostics().empty(), "invalid deferred command reports diagnostic");
    auto zero = world.transform(h);
    zero.scale[0] = 0;
    world.setTransform(h, zero);
    world.addComponent(h, component("faset.rigid_body_2d"));
    world.singleStep();
    rejects([&] { world.fields(h, "faset.rigid_body_2d"); },
            "invalid runtime collider scale must not half-add component");
    zero.scale[0] = 1;
    world.setTransform(h, zero);
    world.addComponent(h, component("faset.rigid_body_2d"));
    world.singleStep();
    check(world.velocity(h)[1] < 0, "deferred body runs actual physics");
    world.removeComponent(h, "faset.rigid_body_2d");
    world.singleStep();
    rejects([&] { world.velocity(h); }, "removed physics adapter no longer accessible");
    world.destroy(h);
    world.destroy(h);
    world.singleStep();
    check(!world.valid(h), "repeated deferred destroy is safe");
    rejects([&] { world.advance(std::numeric_limits<double>::quiet_NaN()); },
            "nonfinite time rejected");
    rejects([&] { world.advance(0, {std::numeric_limits<float>::infinity(), 0, false, false}); },
            "nonfinite input rejected");
    Runtime callbacks;
    std::vector<std::string> order;
    Behavior b;
    b.onStart = [&](Runtime& r, EntityHandle, double) {
        order.push_back("start");
        rejects([&] { r.singleStep(); }, "OnStart cannot recursively advance");
    };
    b.fixedUpdate = [&](Runtime&, EntityHandle, double) {
        order.push_back("fixed");
        throw std::runtime_error("intentional callback failure");
    };
    b.update = [&](Runtime&, EntityHandle, double) { order.push_back("update"); };
    b.lateUpdate = [&](Runtime& r, EntityHandle h, double) {
        order.push_back("late");
        auto p = r.presentation(h);
        p.position[2] = 9;
        r.setPresentation(h, p);
    };
    callbacks.registerBehavior("test", b);
    auto object = entity("callbacks");
    object["components"].push_back(component("test"));
    doc["entities"] = Json::array({object});
    callbacks.load(doc);
    callbacks.singleStep();
    check(order == std::vector<std::string>{"start", "fixed", "update", "late"},
          "callback failure does not skip remaining phases");
    check(callbacks.diagnostics().size() == 1, "callback exception diagnostic");
    near(callbacks.snapshot().entities[0].transform.position[2], 9, 0.001f,
         "LateUpdate changes final presentation only");
    near(callbacks.transform(callbacks.find("callbacks")).position[2], 0, 0.001f,
         "presentation does not overwrite simulation");
    callbacks.clear();
}
void sampleGameplay() {
    Runtime world;
    faset::gameplay::registerGameplay(world);
    auto doc = scene(3);
    auto door = entity("door");
    door["components"].push_back(component("gameplay.door", {{"speed", 2.0}}));
    doc["entities"] = Json::array({door});
    world.load(doc);
    world.advance(1.0 / 60, {0, 0, false, true});
    for (int i = 0; i < 59; ++i)
        world.advance(1.0 / 60);
    near(world.transform(world.find("door")).rotation[1], 1.5707963f, 0.001f,
         "sample door opens through real static gameplay callback");
    world.advance(1.0 / 60, {0, 0, false, true});
    for (int i = 0; i < 59; ++i)
        world.advance(1.0 / 60);
    near(world.transform(world.find("door")).rotation[1], 0, 0.001f, "sample door toggles closed");
}
void groundedJump() {
    Runtime world;
    faset::gameplay::registerGameplay(world);
    auto doc = scene(2);
    auto ground = entity("ground", -0.5f);
    ground["components"].push_back(
        component("faset.rigid_body_2d", {{"body_type", "static"}, {"half_extents", {10, .5}}}));
    auto character = entity("character", 0.5f);
    character["components"].push_back(component("faset.rigid_body_2d"));
    character["components"].push_back(component("gameplay.character"));
    doc["entities"] = Json::array({ground, character});
    world.load(doc);
    auto h = world.find("character");
    for (int i = 0; i < 10; ++i)
        world.advance(1.0 / 60);
    check(world.grounded(h), "character starts supported");
    world.advance(1.0 / 60, {0, 0, true, false});
    check(world.velocity(h)[1] > 4, "grounded jump sets upward velocity");
    for (int i = 0; i < 90 && world.velocity(h)[1] > 0.1f; ++i)
        world.advance(1.0 / 60);
    check(!world.grounded(h), "apex is not grounded");
    auto before = world.velocity(h)[1];
    world.advance(1.0 / 60, {0, 0, true, false});
    check(world.velocity(h)[1] < before, "jump at apex must not create a second impulse");
    for (int i = 0; i < 180; ++i)
        world.advance(1.0 / 60);
    check(world.grounded(h), "character regains ground after landing");
    Runtime wall;
    auto wallScene = scene(2);
    auto obstacle = entity("wall");
    obstacle["components"].push_back(
        component("faset.rigid_body_2d", {{"body_type", "static"}, {"half_extents", {.5, 5}}}));
    auto body = entity("side");
    body["components"][0]["fields"]["position"] = {1, 0, 0};
    body["components"].push_back(component("faset.rigid_body_2d", {{"gravity_scale", 0}}));
    wallScene["entities"] = Json::array({obstacle, body});
    wall.load(wallScene);
    for (int i = 0; i < 5; ++i)
        wall.advance(1.0 / 60);
    check(!wall.grounded(wall.find("side")), "wall contact is not a supporting floor contact");
}
} // namespace
int main() {
    try {
        auto run = [](const char* name, auto fn) {
            try {
                fn();
                std::cout << name << " passed\n";
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(name) + ": " + error.what());
            }
        };
        run("Box2D", [] { physics(2); });
        run("Box3D", [] { physics(3); });
        run("Lifecycle", lifecycle);
        run("Clock/validation", clockAndValidation);
        run("Component schema versions", componentSchemaVersions);
        run("Structural failures", structuralFailuresAndCallbacks);
        run("Sample gameplay", sampleGameplay);
        run("Grounded jump", groundedJump);
        std::cout << "runtime contracts passed: actual Box2D/Box3D collisions, lifecycle, handles, "
                     "interpolation, deferred mutation, catchup, pause, validation, gameplay\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
