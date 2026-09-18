#include <faset/runtime/Runtime.hpp>
#include <faset/scripting/LuaModule.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace faset::runtime;
using namespace faset::scripting;
using Json = nlohmann::json;

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Function> void rejects(Function function, const char* message) {
    bool rejected{};
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
LuaProject project(std::string source) {
    LuaProject result;
    result.scripts = {"Scripts/safety.lua"};
    result.sources.emplace(result.scripts.front(), std::move(source));
    return result;
}
Json scene(unsigned count = 1) {
    Json entities = Json::array();
    for (unsigned i = 0; i < count; ++i)
        entities.push_back({{"id", "actor" + std::to_string(i)},
                            {"name", "Safety actor"},
                            {"parent", nullptr},
                            {"components", Json::array({{{"id", "transform"},
                                                         {"type", "faset.transform"},
                                                         {"version", 1},
                                                         {"fields", Json::object()}},
                                                        {{"id", "behavior"},
                                                         {"type", "test.safety"},
                                                         {"version", 1},
                                                         {"fields", {{"fail", i == 0}}}}})}});
    return {{"format", "faset.scene"}, {"version", 1}, {"dimension", 2}, {"entities", entities}};
}
void depthAndMetatables() {
    LuaModule lua(project(R"lua(
assert(setmetatable == nil)
assert(getmetatable("") == "string")
local nested = 7
for i = 1, 24 do nested = {nested} end
local B = faset.behavior {
    id = "test.safety",
    fields = { nested = { type = "any", default = nested } }
}
function B:on_start()
    assert(getmetatable(self) == "faset.BehaviorInstance")
    assert(getmetatable(self.entity) == "faset.Entity")
    local value = self.fields.nested
    for i = 1, 24 do value = value[1] end
    assert(value == 7)
end
return B
)lua"));
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene());
    check(world.diagnostics().empty(), "deep supported JSON uses reserved Lua stack slots");
    rejects(
        [] {
            LuaModule invalid(project(R"lua(
local nested = 7
for i = 1, 40 do nested = {nested} end
return faset.behavior {id="test.safety",fields={value={type="any",default=nested}}}
)lua"));
        },
        "over-depth JSON rejects cleanly");
    rejects(
        [] {
            LuaModule invalid(project(R"lua(
local cyclic = {}
cyclic.self = cyclic
return faset.behavior {id="test.safety",fields={value={type="any",default=cyclic}}}
)lua"));
        },
        "cyclic JSON rejects cleanly");
}
void jsonFanout() {
    // Lua owns only one 1-MiB string. Copying aliases into native JSON must not
    // evade the host's aggregate byte limit and expand into an enormous tree.
    rejects(
        [] {
            LuaModule invalid(project(R"lua(
local text = string.rep("x", 1024 * 1024)
local aliases = {}
for i = 1, 64 do aliases[i] = text end
return faset.behavior {id="test.safety",fields={value={type="array",default=aliases}}}
)lua"));
        },
        "JSON string alias fanout is bounded before native copies");
    rejects(
        [] {
            LuaModule invalid(project(R"lua(
local key = string.rep("x", 1024 * 1024)
local aliases = {}
for i = 1, 64 do aliases[i] = {[key]=true} end
return faset.behavior {id="test.safety",fields={value={type="array",default=aliases}}}
)lua"));
        },
        "JSON key alias fanout is bounded before native copies");
}
void structuralBudgets() {
    for (const char* body : {"for i=1,2048 do self.entity:destroy() end",
                             "local text=string.rep('x',1024*1024)\n"
                             "for i=1,64 do self.entity:add_component {id='data'..i,type='data'..i,"
                             "version=1,fields={text=text}} end"}) {
        LuaModule lua(project(std::string("local B=faset.behavior{id='test.safety',fields={}}\n"
                                          "function B:on_start()\n") +
                              body + "\nend\nreturn B"));
        Runtime world;
        lua.registerBehaviors(world);
        world.load(scene());
        check(world.diagnostics().size() == 1,
              "native structural queue has count and byte budgets");
        world.advance(0.001);
        check(world.diagnostics().size() == 1,
              "structural-budget error disables only that instance");
    }
}
void repeatedMemoryFailures() {
    LuaLimits limits;
    limits.memoryBytes = 256 * 1024;
    for (unsigned repetition = 0; repetition < 32; ++repetition) {
        LuaModule lua(project(R"lua(
local B = faset.behavior {
    id = "test.safety", fields = { fail = { type = "boolean", default = false } }
}
function B:update(dt)
    if self.fields.fail then
        self.state.too_large = string.rep("x", 1024 * 1024)
    else
        local value = self.entity:transform()
        value.position.x = value.position.x + 1
        self.entity:set_transform(value)
    end
end
return B
)lua"),
                      limits);
        Runtime world;
        lua.registerBehaviors(world);
        world.load(scene(2));
        for (unsigned frame = 0; frame < 20; ++frame)
            world.advance(0.001);
        check(world.diagnostics().size() == 1, "OOM instance reports once across repeated frames");
        check(world.transform(world.find("actor1")).position[0] == 20,
              "OOM does not poison subsequent protected calls or Lua stack");
    }
    // VM bootstrap itself must report allocation failure, not invoke Lua panic.
    limits.memoryBytes = 1;
    rejects(
        [&] {
            LuaModule invalid(project("return faset.behavior{id='test.safety',fields={}}"), limits);
        },
        "tiny VM budget fails safely during construction");
}
void persistentStateMemoryFailure() {
    LuaLimits limits;
    limits.memoryBytes = 256 * 1024;
    LuaModule lua(project(R"lua(
local B = faset.behavior {
    id = "test.safety", fields = { fail = { type = "boolean", default = false } }
}
function B:update(dt)
    if self.fields.fail then
        self.state.allocations = {}
        while true do
            self.state.allocations[#self.state.allocations + 1] = string.rep("x", 1024)
        end
    else
        local value = self.entity:transform()
        value.position.x = value.position.x + 1
        self.entity:set_transform(value)
    end
end
return B
)lua"),
                  limits);
    Runtime world;
    lua.registerBehaviors(world);
    world.load(scene(2));
    for (unsigned frame = 0; frame < 20; ++frame)
        world.advance(0.001);
    check(world.diagnostics().size() == 1,
          "incremental state exhaustion only disables the failed instance");
    check(world.transform(world.find("actor1")).position[0] == 20,
          "failed state is released before healthy instances run");
}
} // namespace

int main() {
    try {
        depthAndMetatables();
        jsonFanout();
        structuralBudgets();
        repeatedMemoryFailures();
        persistentStateMemoryFailure();
        std::cout << "Lua safety tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua safety failure: " << error.what() << '\n';
        return 1;
    }
}
