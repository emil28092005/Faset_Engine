#include <algorithm>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/runtime/Runtime.hpp>
#include <faset/runtime/schema.hpp>
#include <faset/scripting/LuaModule.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace faset::runtime;
using namespace faset::scripting;
using Json = nlohmann::json;

namespace {
void check(bool result, const char* message) {
    if (!result)
        throw std::runtime_error(message);
}
void near(float actual, float expected, const char* message) {
    check(std::abs(actual - expected) < 0.001f, message);
}
template <class F> void rejects(F&& fn, const char* message) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
LuaProject project(std::string source) {
    LuaProject result;
    result.scripts = {"Scripts/main.lua"};
    result.sources.emplace(result.scripts.front(), std::move(source));
    return result;
}
Json component(const std::string& type, Json fields = Json::object()) {
    return {{"id", type + "-component"}, {"type", type}, {"version", 1}, {"fields", fields}};
}
Json entity(const std::string& id, const std::string& type = "test.lua",
            Json fields = Json::object()) {
    return {{"id", id},
            {"name", id},
            {"parent", nullptr},
            {"components", Json::array({component("faset.transform"), component(type, fields)})}};
}
Json scene(Json entities) {
    return {{"format", "faset.scene"},
            {"version", 1},
            {"id", "lua-tests"},
            {"dimension", 2},
            {"entities", std::move(entities)},
            {"instances", Json::array()}};
}
bool contains(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const auto& line) { return line.find(text) != std::string::npos; });
}
void defaultsAndIsolation() {
    LuaModule lua(project(R"lua(
local B = faset.behavior {
    id = "test.lua", version = 1, name = "Lua test",
    fields = {
        speed = { type = "number", default = 2, min = 0, max = 10 },
        enabled = { type = "boolean", default = true },
        label = { type = "string", default = "default" }
    }
}
function B:on_start()
    assert(self.fields.enabled and self.fields.label == "default")
    self.state.count = 0
end
function B:update(dt)
    assert(dt > 0)
    self.state.count = self.state.count + 1
    local pose = self.entity:transform()
    pose.position.x = self.state.count * self.fields.speed
    self.entity:set_transform(pose)
end
return B
)lua"));
    const auto metadata = lua.schema();
    check(metadata.is_array() && metadata.size() == 1, "one Lua behavior exports one schema");
    check(metadata[0]["fields"]["speed"]["default"] == 2, "schema retains defaults");
    check(metadata[0]["id"] == "test.lua", "schema retains stable TypeId");
    Runtime world;
    lua.registerBehaviors(world);
    auto doc =
        scene(Json::array({entity("default"), entity("override", "test.lua", {{"speed", 5}})}));
    validate_scene_schemas(doc, metadata);
    lua.validateScene(doc);
    auto invalid = doc;
    invalid["entities"][0]["components"][1]["fields"]["speed"] = "not a number";
    rejects([&] { lua.validateScene(invalid); }, "CPU validation rejects invalid Lua field type");
    invalid["entities"][0]["components"][1]["fields"]["speed"] = -1;
    rejects([&] { lua.validateScene(invalid); }, "CPU validation enforces Lua field constraints");
    const auto original = doc;
    world.load(doc);
    world.advance(1.0 / 60);
    near(world.transform(world.find("default")).position[0], 2, "Lua uses schema default");
    near(world.transform(world.find("override")).position[0], 5, "Lua uses scene override");
    world.advance(1.0 / 60);
    near(world.transform(world.find("default")).position[0], 4, "state persists per instance");
    near(world.transform(world.find("override")).position[0], 10, "instances do not share state");
    check(world.fields(world.find("default"), "test.lua").empty(),
          "defaults do not mutate stored runtime configuration");
    check(doc == original, "Lua cannot mutate authoring scene");
    check(world.diagnostics().empty(), "valid Lua behavior produces no diagnostics");
}
void lifecycleAndHandles() {
    LuaModule lua(project(R"lua(
local previous
local B = faset.behavior { id = "test.lua", version = 1, fields = {} }
function B:on_start()
    if previous then assert(not previous:valid()) end
    assert(self.entity:valid())
    assert(faset.find("missing") == nil)
    assert(faset.find("actor") == self.entity)
    previous = self.entity
    faset.log("phase:start")
end
function B:fixed_update(dt)
    assert(dt > 0)
    local input = faset.input()
    assert(input.horizontal == 1 and input.vertical == -1)
    assert(input.jump_pressed and input.interact_pressed)
    faset.log("phase:fixed")
end
function B:update(dt) faset.log("phase:update") end
function B:late_update(dt)
    local pose = self.entity:presentation()
    pose.position.x = 17
    self.entity:set_presentation(pose)
    faset.log("phase:late")
end
function B:on_destroy()
    assert(self.entity:valid())
    faset.log("phase:destroy")
end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    const auto doc = scene(Json::array({entity("actor")}));
    world.load(doc);
    world.advance(1.0 / 60, {1, -1, true, true});
    near(world.presentation(world.find("actor")).position[0], 17,
         "late_update can write presentation transform");
    near(world.transform(world.find("actor")).position[0], 0,
         "presentation write does not mutate simulation transform");
    world.clear();
    const auto lines = lua.takeLogs();
    const std::vector<std::string> phases = {"phase:start", "phase:fixed", "phase:update",
                                             "phase:late", "phase:destroy"};
    check(lines.size() == phases.size(), "each lifecycle callback runs exactly once");
    for (std::size_t i = 0; i < phases.size(); ++i)
        check(lines[i].find(phases[i]) != std::string::npos, "Lua lifecycle follows runtime order");
    check(lua.takeLogs().empty(), "taking logs drains the queue");
    world.load(doc);
    check(world.diagnostics().empty(), "old Lua handles stay stale across world reload");
    world.clear();
}
void moduleLifetime() {
    Runtime world;
    {
        LuaModule temporary(project(R"lua(
local B = faset.behavior { id = "test.lua", fields = {} }
function B:update(dt)
    local pose = self.entity:transform()
    pose.position.x = 23
    self.entity:set_transform(pose)
end
return B
)lua"));
        temporary.registerBehaviors(world);
    }
    world.load(scene(Json::array({entity("actor")})));
    world.advance(0.01);
    near(world.transform(world.find("actor")).position[0], 23,
         "registered callbacks retain VM after LuaModule facade destruction");
    world.clear();
}
void modulesAndSandbox() {
    auto snapshot = project(R"lua(
assert(io == nil and os == nil and debug == nil)
assert(load == nil and loadfile == nil and dofile == nil)
assert(pcall == nil and xpcall == nil and coroutine == nil)
local util = require("util.math")
assert(require("util.math") == util)
local directory = require("directory")
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    local pose = self.entity:transform()
    pose.position.x = util.answer + directory.answer
    self.entity:set_transform(pose)
end
return B
)lua");
    snapshot.sources["Scripts/util/math.lua"] = "return { answer = 40 }";
    snapshot.sources["Scripts/directory/init.lua"] = "return { answer = 2 }";
    LuaModule lua(snapshot);
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("actor")})));
    near(world.transform(world.find("actor")).position[0], 42, "require loads captured modules");
    check(world.diagnostics().empty(), "sandboxed standard operations succeed");
    auto missing = project("return require('missing')");
    rejects([&] { LuaModule invalid(missing); }, "missing modules fail loading");
    auto cycle = project("return require('cycle')");
    cycle.sources["Scripts/cycle.lua"] = "return require('cycle')";
    rejects([&] { LuaModule invalid(cycle); }, "cyclic require fails rather than looping");
    for (const auto& name : {"../escape", "/absolute", "foo/bar", "foo..bar"}) {
        const auto escape = project(std::string("return require('") + name + "')");
        rejects([&] { LuaModule invalid(escape); }, "require rejects path-like module names");
    }
}
void structuralCommands() {
    LuaModule lua(project(R"lua(
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    self.state.ticks = 0
    self.entity:add_component {
        id = "extra-component", type = "test.data", version = 1, fields = { value = 7 }
    }
    faset.spawn {
        id = "spawned", name = "Spawned", parent = faset.null,
        components = {
            { id = "spawned-transform", type = "faset.transform", version = 1,
              fields = { position = { 3, 4, 0 } } }
        }
    }
    assert(faset.find("spawned") == nil)
end
function B:fixed_update(dt)
    self.state.ticks = self.state.ticks + 1
    if self.state.ticks == 1 then
        assert(self.entity:fields("test.data").value == 7)
        self.entity:remove_component("test.data")
        local spawned = faset.find("spawned")
        assert(spawned and spawned:valid())
        assert(spawned:transform().position.x == 3)
        spawned:destroy()
        assert(spawned:valid())
        self.state.spawned = spawned
    elseif self.state.ticks == 2 then
        assert(not self.state.spawned:valid())
        assert(faset.find("spawned") == nil)
        faset.log("structural:done")
    end
end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("actor")})));
    check(!world.find("spawned"), "Lua spawn waits for fixed-tick barrier");
    world.singleStep();
    check(bool(world.find("spawned")), "Lua spawn applies at first barrier");
    world.singleStep();
    check(!world.find("spawned"), "Lua destroy applies at next barrier");
    rejects([&] { world.fields(world.find("actor"), "test.data"); },
            "Lua remove_component applies at next barrier");
    check(contains(lua.takeLogs(), "structural:done"), "Lua observes deferred structural changes");
    check(world.diagnostics().empty(), "valid structural API calls produce no errors");
}
void errorsAreContained() {
    LuaModule lua(project(R"lua(
local B = faset.behavior {
    id = "test.lua", fields = { fail = { type = "boolean", default = false } }
}
function B:update(dt)
    if self.fields.fail then error("intentional-lua-error") end
    local pose = self.entity:transform()
    pose.position.x = pose.position.x + 1
    self.entity:set_transform(pose)
end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("bad", "test.lua", {{"fail", true}}), entity("good")})));
    world.advance(0.01);
    world.advance(0.01);
    near(world.transform(world.find("good")).position[0], 2,
         "a failed Lua instance does not disable healthy instances");
    check(world.diagnostics().size() == 1, "failed instance logs once and is disabled");
    check(contains(world.diagnostics(), "intentional-lua-error"),
          "Lua errors reach runtime diagnostics");
    check(contains(world.diagnostics(), "Scripts/main.lua"), "Lua error includes source path");
    check(contains(world.diagnostics(), "stack traceback"), "Lua error includes traceback");
    for (const auto& body :
         {"self.entity:set_presentation(self.entity:transform())", "self.entity:velocity()",
          "self.entity:set_transform({position = {x = 0/0, y = 0, z = 0}})"}) {
        LuaModule invalid(project(std::string("local B=faset.behavior{id='test.lua',fields={}}\n") +
                                  "function B:update(dt) " + body + " end\nreturn B"));
        Runtime separate;
        invalid.registerBehaviors(separate);
        separate.load(scene(Json::array({entity("actor")})));
        separate.advance(0.01);
        check(!separate.diagnostics().empty(), "invalid bound calls become contained Lua errors");
    }
}
void staleHandleAccess() {
    LuaModule lua(project(R"lua(
local previous
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    if previous then
        assert(not previous:valid())
        previous:transform()
    end
    previous = self.entity
end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    const auto document = scene(Json::array({entity("actor")}));
    world.load(document);
    check(world.diagnostics().empty(), "initial entity handle is valid");
    world.load(document);
    check(!world.diagnostics().empty(),
          "retained userdata rejects access after session replacement");

    LuaModule shared(project(R"lua(
local previous
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    if previous then
        assert(not previous:valid())
        previous:transform()
    end
    previous = self.entity
end
return B
)lua"));
    Runtime first;
    Runtime second;
    shared.registerBehaviors(first);
    shared.registerBehaviors(second);
    first.load(document);
    second.load(document);
    check(!second.diagnostics().empty(), "userdata from another world is rejected");
}
void componentInstanceLifetime() {
    LuaModule lua(project(R"lua(
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    assert(self.state.counter == nil)
    self.state.counter = 0
    faset.log("component:start")
end
function B:update(dt) self.state.counter = self.state.counter + 1 end
function B:on_destroy() faset.log("component:destroy") end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("actor")})));
    world.advance(0.01);
    auto handle = world.find("actor");
    world.removeComponent(handle, "test.lua");
    world.singleStep();
    check(world.valid(handle), "removing behavior does not remove its owner");
    world.addComponent(handle, component("test.lua"));
    world.singleStep();
    auto lines = lua.takeLogs();
    check(lines.size() == 3 && lines[0].find("component:start") != std::string::npos &&
              lines[1].find("component:destroy") != std::string::npos &&
              lines[2].find("component:start") != std::string::npos,
          "re-attaching a behavior creates fresh instance state");
    check(world.diagnostics().empty(), "component state is released on removal");
    world.clear();
}
void physicsAndCollision() {
    LuaModule lua(project(R"lua(
local B = faset.behavior { id = "test.lua", fields = {} }
function B:on_start()
    local velocity = self.entity:velocity()
    velocity.x = 1
    self.entity:set_velocity(velocity)
    self.entity:apply_impulse { x = 0, y = 0.1, z = 0 }
    assert(self.entity:velocity().y > 0)
    local pose = self.entity:transform()
    pose.position.y = 2
    self.entity:teleport(pose)
    assert(not self.entity:is_grounded())
end
function B:on_collision(event)
    assert(event.first:valid() and event.second:valid())
    if event.began then faset.log("contact:began") end
end
function B:fixed_update(dt)
    if self.entity:is_grounded() then faset.log("body:grounded") end
end
return B
)lua"));
    auto floor = entity("floor", "test.floor");
    floor["components"][0]["fields"] = {{"position", {0, -0.5, 0}}};
    floor["components"].push_back(
        component("faset.rigid_body_2d", {{"body_type", "static"}, {"half_extents", {10, 0.5}}}));
    auto falling = entity("actor");
    falling["components"].push_back(component("faset.rigid_body_2d"));
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({floor, falling})));
    for (int i = 0; i < 180; ++i)
        world.advance(1.0 / 60);
    const auto logs = lua.takeLogs();
    check(contains(logs, "contact:began"), "Lua receives native collision event");
    check(contains(logs, "body:grounded"), "Lua sees native grounded state");
    check(world.diagnostics().empty(), "valid physics bindings produce no diagnostics");
}
void invalidDefinitionsAndBudgets() {
    for (const auto& source :
         {"this is not lua", "return 42", "return faset.behavior { id = '', fields = {} }",
          "return faset.behavior { id = 'faset.transform', fields = {} }",
          "return faset.behavior { id = 'test.lua', version = 0, fields = {} }",
          "return faset.behavior { id = 'test.lua', fields = { speed = { type = 'number' } } }"}) {
        rejects([&] { LuaModule invalid(project(source)); },
                "invalid behavior declaration is rejected");
    }
    auto duplicate = project("return faset.behavior { id = 'test.lua', fields = {} }");
    duplicate.scripts.push_back("Scripts/second.lua");
    duplicate.sources["Scripts/second.lua"] = duplicate.sources.begin()->second;
    rejects([&] { LuaModule invalid(duplicate); }, "duplicate behavior TypeIds are rejected");
    LuaLimits limits;
    limits.instructions = 10'000;
    rejects([&] { LuaModule invalid(project("while true do end"), limits); },
            "instruction budget bounds module evaluation");
    LuaModule lua(project(R"lua(
local B = faset.behavior { id = "test.lua", fields = {} }
function B:update(dt) while true do end end
return B
)lua"),
                  limits);
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("actor")})));
    world.advance(0.01);
    check(!world.diagnostics().empty(), "instruction budget bounds callback evaluation");
    const auto failures = world.diagnostics().size();
    world.advance(0.01);
    check(world.diagnostics().size() == failures, "runaway instance stays disabled");
}
void memoryLimits() {
    LuaLimits limits;
    limits.memoryBytes = 256 * 1024;
    rejects(
        [&] {
            LuaModule invalid(project("local oversized = string.rep('x', 1048576)\n"
                                      "return faset.behavior{id='test.lua',fields={}}"),
                              limits);
        },
        "memory budget bounds module evaluation");
    LuaModule lua(project(R"lua(
local B = faset.behavior {
    id = "test.lua", fields = { fail = { type = "boolean", default = false } }
}
function B:update(dt)
    if self.fields.fail then
        self.state.oversized = string.rep("x", 1048576)
    else
        local pose = self.entity:transform()
        pose.position.x = pose.position.x + 1
        self.entity:set_transform(pose)
    end
end
return B
)lua"),
                  limits);
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(Json::array({entity("bad", "test.lua", {{"fail", true}}), entity("good")})));
    world.advance(0.01);
    check(!world.diagnostics().empty(), "memory budget bounds callback allocations");
    world.advance(0.01);
    near(world.transform(world.find("good")).position[0], 2,
         "VM survives a rejected allocation and runs other instances");
}
#ifdef FASET_SOURCE_DIR
void exampleSmokeTest() {
    const auto root = std::filesystem::path(FASET_SOURCE_DIR) / "examples/lua";
    const auto snapshot = loadLuaProject(root);
    LuaModule lua(snapshot);
    check(lua.schema().size() == 2, "shipped Lua example exports both behaviors");
    const auto document = faset::read_json(root / "Scenes/main.scene.json");
    validate_scene_schemas(document, lua.schema());
    Runtime world;
    lua.registerBehaviors(world);
    world.load(document);
    for (int i = 0; i < 120; ++i)
        world.advance(1.0 / 60);
    check(world.grounded(world.find("player")), "Lua example player lands on its floor");
    world.advance(1.0 / 60, {1, 0, true, false});
    check(world.velocity(world.find("player"))[0] > 4,
          "Lua example controller applies horizontal input");
    check(world.velocity(world.find("player"))[1] > 5,
          "Lua example controller jumps from native contact");
    world.advance(1.0 / 60, {0, 0, false, true});
    near(world.transform(world.find("player")).position[0], -2,
         "Lua example interaction resets player position");
    check(world.diagnostics().empty(), "shipped Lua scripts run without diagnostics");
}
#endif
} // namespace

int main() {
    try {
        defaultsAndIsolation();
        lifecycleAndHandles();
        moduleLifetime();
        modulesAndSandbox();
        structuralCommands();
        errorsAreContained();
        staleHandleAccess();
        componentInstanceLifetime();
        physicsAndCollision();
        invalidDefinitionsAndBudgets();
        memoryLimits();
#ifdef FASET_SOURCE_DIR
        exampleSmokeTest();
#endif
        std::cout << "Lua: schemas, state, lifecycle, safe handles, sandbox, modules, physics, "
                     "deferred commands, diagnostics, and execution limits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua test failed: " << error.what() << '\n';
        return 1;
    }
}
