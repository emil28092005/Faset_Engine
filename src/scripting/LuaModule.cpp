#include <faset/runtime/schema.hpp>
#include <faset/scripting/LuaModule.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace faset::scripting {
namespace {
using Json = nlohmann::json;
constexpr int hookInterval = 100;
constexpr std::size_t maxJsonNodes = 100'000;
constexpr std::size_t maxJsonBytes = 16 * 1024 * 1024;
constexpr std::size_t maxStructuralCommands = 1024;
char nullToken;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::invalid_argument(message);
}

std::string stringArgument(lua_State* state, int index) {
    require(lua_type(state, index) == LUA_TSTRING, "expected a string");
    std::size_t length{};
    const char* value = lua_tolstring(state, index, &length);
    return std::string(value, length);
}

// Lua strings/tables may be shared many times. JSON owns each occurrence, so
// the VM allocation quota alone cannot bound expansion into native memory.
struct JsonBudget {
    std::size_t nodes{}, bytes{};
    void charge(std::size_t amount) {
        require(amount <= maxJsonBytes - bytes, "Lua JSON byte budget exhausted (16 MiB)");
        bytes += amount;
    }
};

// Reads use raw Lua operations only: no metamethod can interrupt C++ object lifetimes.
Json readJson(lua_State* state, int index, std::set<const void*>& ancestors, JsonBudget& budget,
              unsigned depth = 0) {
    require(lua_checkstack(state, 4) != 0, "Lua stack budget exhausted");
    require(++budget.nodes <= maxJsonNodes && depth < 32,
            "Lua JSON value is too large or deeply nested");
    budget.charge(64); // Conservative scalar/container bookkeeping charge.
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return lua_toboolean(state, index) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
            return lua_tointeger(state, index);
        require(std::isfinite(lua_tonumber(state, index)), "JSON numbers must be finite");
        return lua_tonumber(state, index);
    case LUA_TSTRING: {
        std::size_t length{};
        lua_tolstring(state, index, &length);
        budget.charge(length);
        return stringArgument(state, index);
    }
    case LUA_TLIGHTUSERDATA:
        require(lua_touserdata(state, index) == &nullToken, "unsupported JSON userdata");
        return nullptr;
    case LUA_TTABLE: {
        const auto* identity = lua_topointer(state, index);
        require(ancestors.insert(identity).second, "cyclic table is not a JSON value");
        Json object = Json::object();
        std::map<lua_Integer, Json> entries;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) {
                std::size_t length{};
                lua_tolstring(state, -2, &length);
                budget.charge(length);
                const auto key = stringArgument(state, -2);
                object[key] = readJson(state, -1, ancestors, budget, depth + 1);
            } else {
                require(lua_isinteger(state, -2) && lua_tointeger(state, -2) > 0,
                        "JSON table keys must be strings or positive array indices");
                entries.emplace(lua_tointeger(state, -2),
                                readJson(state, -1, ancestors, budget, depth + 1));
            }
            lua_pop(state, 1);
        }
        ancestors.erase(identity);
        require(object.empty() || entries.empty(), "JSON table cannot mix string and array keys");
        if (entries.empty())
            return object;
        Json array = Json::array();
        for (auto& [key, value] : entries) {
            require(key == static_cast<lua_Integer>(array.size()) + 1,
                    "JSON array indices must be contiguous");
            array.push_back(std::move(value));
        }
        return array;
    }
    default:
        throw std::invalid_argument("functions, threads and entity handles are not JSON values");
    }
}

Json readJson(lua_State* state, int index, JsonBudget& budget) {
    require(lua_checkstack(state, 100) != 0, "Lua stack budget exhausted");
    std::set<const void*> ancestors;
    return readJson(state, index, ancestors, budget);
}

Json readJson(lua_State* state, int index) {
    JsonBudget budget;
    return readJson(state, index, budget);
}

void validateField(const Json& value, const Json& descriptor) {
    const auto kind = descriptor.value("type", std::string("any"));
    bool valid{};
    if (kind == "number" || kind == "float")
        valid = value.is_number() && std::isfinite(value.get<double>());
    else if (kind == "integer" || kind == "int")
        valid = value.is_number_integer();
    else if (kind == "boolean" || kind == "bool")
        valid = value.is_boolean();
    else if (kind == "string" || kind == "asset_ref" || kind == "entity_ref")
        valid = value.is_string();
    else if (kind == "vec2" || kind == "vec3" || kind == "vec4" || kind == "color") {
        const auto size = kind == "vec2" ? 2u : (kind == "vec3" ? 3u : 4u);
        valid = value.is_array() && value.size() == size;
        if (valid)
            for (const auto& entry : value)
                valid = valid && entry.is_number() && std::isfinite(entry.get<double>());
    } else if (kind == "array")
        valid = value.is_array();
    else if (kind == "object")
        valid = value.is_object();
    else {
        require(kind == "any", "unsupported schema field type: " + kind);
        valid = true;
    }
    require(valid, "invalid field " + descriptor.value("id", std::string("?")) + " (expected " +
                       kind + ")");
    if (value.is_number()) {
        if (descriptor.contains("min"))
            require(value.get<double>() >= descriptor.at("min").get<double>(),
                    "field " + descriptor.value("id", std::string("?")) + " is below its minimum");
        if (descriptor.contains("max"))
            require(value.get<double>() <= descriptor.at("max").get<double>(),
                    "field " + descriptor.value("id", std::string("?")) + " exceeds its maximum");
    }
    if (descriptor.contains("enum")) {
        bool found{};
        for (const auto& option : descriptor.at("enum"))
            found = found || option == value;
        require(found, "field " + descriptor.value("id", std::string("?")) +
                           " is not an allowed enum choice");
    }
}

void normalizeSchema(Json& value) {
    require(value.is_object() && value.contains("id") && value.at("id").is_string(),
            "Lua behavior requires a string id");
    const auto id = value.at("id").get<std::string>();
    require(!id.empty() && !runtime::is_builtin_component(id), "invalid or builtin behavior id");
    if (!value.contains("version"))
        value["version"] = 1;
    require(value.at("version").is_number_integer() && value.at("version") > 0 &&
                value.at("version") <= std::numeric_limits<int>::max(),
            "schema version must be a positive integer");
    if (!value.contains("name"))
        value["name"] = id;
    require(value.at("name").is_string(), "schema name must be a string");
    if (!value.contains("fields"))
        value["fields"] = Json::object();
    require(value.at("fields").is_object(), "schema fields must be an object");
    for (auto& [key, field] : value["fields"].items()) {
        require(!key.empty() && field.is_object() && field.contains("default"),
                "each field requires an id and typed default");
        require(field.value("id", key) == key, "field id must match its map key");
        field["id"] = key;
        if (field.value("type", std::string{}) == "array" && field["default"].is_object() &&
            field["default"].empty())
            field["default"] = Json::array();
        for (const auto* limit : {"min", "max"})
            if (field.contains(limit))
                require(field.at(limit).is_number() && std::isfinite(field.at(limit).get<double>()),
                        "field limits must be finite numbers");
        if (field.contains("min") && field.contains("max"))
            require(field.at("min") <= field.at("max"), "field minimum exceeds maximum");
        if (field.contains("enum")) {
            if (field["enum"].is_object() && field["enum"].empty())
                field["enum"] = Json::array();
            require(field["enum"].is_array(), "field enum must be an array");
        }
        validateField(field.at("default"), field);
    }
    if (value.contains("migrations")) {
        if (value["migrations"].is_object() && value["migrations"].empty())
            value["migrations"] = Json::array();
        require(value["migrations"].is_array(), "migrations must be an array");
        std::set<int> versions;
        for (const auto& step : value["migrations"]) {
            require(step.is_object() && step.contains("from_version") &&
                        step.at("from_version").is_number_integer() &&
                        step.at("from_version") > 0 && step.at("from_version") < value["version"] &&
                        step.contains("fields") && step.at("fields").is_object(),
                    "invalid migration step");
            require(versions.insert(step.at("from_version").get<int>()).second,
                    "duplicate migration version");
            for (const auto& [key, unused] : step.items())
                require(key == "from_version" || key == "fields", "unsupported migration property");
            for (const auto& [field, rules] : step.at("fields").items()) {
                require(!field.empty() && rules.is_object(), "invalid migration field rule");
                for (const auto& [operation, argument] : rules.items()) {
                    require(operation == "default" || operation == "scale" ||
                                operation == "require_manual",
                            "unsupported migration operation");
                    if (operation == "scale")
                        require(argument.is_number() && std::isfinite(argument.get<double>()),
                                "migration scale must be finite");
                    if (operation == "require_manual")
                        require(argument.is_boolean(), "require_manual must be boolean");
                }
            }
        }
    }
}

// An immutable, precomputed marshaling tree. Lua allocations never occur with
// owning C++ temporaries or iterators on the C stack (Lua errors use longjmp).
struct Value {
    enum class Kind { Null, Boolean, Integer, Number, String, Array, Object } kind{Kind::Null};
    bool boolean{};
    lua_Integer integer{};
    double number{};
    std::string string;
    std::vector<std::string> keys;
    std::vector<Value> children;
    Value() = default;
    explicit Value(const Json& json) {
        if (json.is_boolean()) {
            kind = Kind::Boolean;
            boolean = json.get<bool>();
        } else if (json.is_number_integer()) {
            if (json.is_number_unsigned())
                require(json.get<std::uint64_t>() <= static_cast<std::uint64_t>(LUA_MAXINTEGER),
                        "integer exceeds Lua's exact range");
            kind = Kind::Integer;
            integer = json.get<lua_Integer>();
        } else if (json.is_number()) {
            kind = Kind::Number;
            number = json.get<double>();
        } else if (json.is_string()) {
            kind = Kind::String;
            string = json.get<std::string>();
        } else if (json.is_array()) {
            kind = Kind::Array;
            for (const auto& child : json)
                children.emplace_back(child);
        } else if (json.is_object()) {
            kind = Kind::Object;
            for (const auto& [key, child] : json.items()) {
                keys.push_back(key);
                children.emplace_back(child);
            }
        }
    }
};

void pushValue(lua_State* state, const Value& value) {
    if (!lua_checkstack(state, 4))
        luaL_error(state, "Lua stack budget exhausted");
    switch (value.kind) {
    case Value::Kind::Null:
        lua_pushlightuserdata(state, &nullToken);
        break;
    case Value::Kind::Boolean:
        lua_pushboolean(state, value.boolean);
        break;
    case Value::Kind::Integer:
        lua_pushinteger(state, value.integer);
        break;
    case Value::Kind::Number:
        lua_pushnumber(state, value.number);
        break;
    case Value::Kind::String:
        lua_pushlstring(state, value.string.data(), value.string.size());
        break;
    case Value::Kind::Array:
    case Value::Kind::Object:
        lua_createtable(
            state, value.kind == Value::Kind::Array ? static_cast<int>(value.children.size()) : 0,
            value.kind == Value::Kind::Object ? static_cast<int>(value.children.size()) : 0);
        for (std::size_t i = 0; i < value.children.size(); ++i) {
            if (value.kind == Value::Kind::Object)
                lua_pushlstring(state, value.keys[i].data(), value.keys[i].size());
            pushValue(state, value.children[i]);
            if (value.kind == Value::Kind::Array)
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            else
                lua_rawset(state, -3);
        }
        break;
    }
}

void pushVector(lua_State* state, const runtime::Vec3& vector) {
    lua_createtable(state, 0, 3);
    lua_pushnumber(state, vector[0]);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, vector[1]);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, vector[2]);
    lua_setfield(state, -2, "z");
}
void pushTransform(lua_State* state, const runtime::Transform& transform) {
    lua_createtable(state, 0, 3);
    pushVector(state, transform.position);
    lua_setfield(state, -2, "position");
    pushVector(state, transform.rotation);
    lua_setfield(state, -2, "rotation");
    pushVector(state, transform.scale);
    lua_setfield(state, -2, "scale");
}
runtime::Vec3 readVector(lua_State* state, int index) {
    const auto value = readJson(state, index);
    require(value.is_object(), "vector must be a table with x, y, z");
    runtime::Vec3 result{};
    unsigned i{};
    for (const char* axis : {"x", "y", "z"}) {
        require(value.contains(axis) && value.at(axis).is_number(), "vector requires x, y, z");
        const auto number = value.at(axis).get<double>();
        require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(),
                "vector component must be finite and fit float");
        result[i++] = static_cast<float>(number);
    }
    return result;
}
runtime::Transform readTransform(lua_State* state, int index) {
    // This path makes no Lua allocations, even with C++ JSON temporaries alive.
    const auto value = readJson(state, index);
    require(value.is_object(), "transform must be an object");
    runtime::Transform result;
    auto read = [&](const char* key, runtime::Vec3& target) {
        require(value.contains(key) && value.at(key).is_object(),
                std::string("transform requires ") + key);
        unsigned i{};
        for (const char* axis : {"x", "y", "z"}) {
            const auto& vector = value.at(key);
            require(vector.contains(axis) && vector.at(axis).is_number(),
                    "vector requires x, y, z");
            const auto number = vector.at(axis).get<double>();
            require(std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max(),
                    "transform component must be finite and fit float");
            target[i++] = static_cast<float>(number);
        }
    };
    read("position", result.position);
    read("rotation", result.rotation);
    read("scale", result.scale);
    return result;
}
} // namespace

struct LuaModule::Impl {
    struct Memory {
        std::size_t used{}, limit{};
    } memory;
    struct alignas(std::max_align_t) Allocation {
        std::size_t bytes;
    };
    struct Definition {
        std::string path, id;
        Json schema;
        int reference{LUA_NOREF};
    };
    struct Instance {
        int reference{LUA_NOREF};
        bool disabled{};
        Value fields;
    };
    using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint64_t, std::size_t>;
    struct Invocation {
        Definition* definition{};
        Instance* instance{};
        runtime::EntityHandle entity;
        const char* callback{};
        double delta{};
        const runtime::CollisionEvent* collision{};
        bool destroy{};
    };
    LuaProject project;
    LuaLimits limits;
    lua_State* state{};
    std::vector<Definition> definitions;
    std::map<Key, Instance> instances;
    Json schemas = Json::array();
    std::vector<std::string> logs;
    runtime::Runtime* activeRuntime{};
    Invocation* activeInvocation{};
    const char* loadingPath{};
    std::map<std::string, int> modules;
    std::set<std::string> loadingModules;
    int entityMetatable{LUA_NOREF}, behaviorSet{LUA_NOREF};
    std::size_t instructionsLeft{};
    std::size_t structuralCommands{};
    JsonBudget metadataBudget, structuralBudget;
    bool budgetExceeded{};
    Value output;

    static void* allocate(void* user, void* pointer, std::size_t, std::size_t size) {
        auto& memory = *static_cast<Memory*>(user);
        auto* allocation = pointer ? static_cast<Allocation*>(pointer) - 1 : nullptr;
        const auto oldSize = allocation ? allocation->bytes : 0;
        if (!size) {
            std::free(allocation);
            memory.used -= oldSize;
            return nullptr;
        }
        if (size > std::numeric_limits<std::size_t>::max() - sizeof(Allocation))
            return nullptr;
        size += sizeof(Allocation);
        if (size > oldSize && size - oldSize > memory.limit - memory.used)
            return nullptr;
        auto* next = static_cast<Allocation*>(std::realloc(allocation, size));
        if (!next) {
            // Lua requires shrinking allocations to succeed. Keeping the larger
            // block is safe; its actual size remains charged to the VM budget.
            return size <= oldSize ? pointer : nullptr;
        }
        next->bytes = size;
        memory.used = memory.used - oldSize + size;
        return next + 1;
    }
    static Impl& get(lua_State* state) {
        return **static_cast<Impl**>(lua_getextraspace(state));
    }
    static void hook(lua_State* state, lua_Debug*) {
        auto& self = get(state);
        if (self.instructionsLeft <= hookInterval) {
            self.budgetExceeded = true;
            luaL_error(state, "Lua instruction budget exhausted");
        }
        self.instructionsLeft -= hookInterval;
    }
    // Exception boundaries never call lua_error until C++ catch objects have died.
    template <int (*Function)(lua_State*)> static int guarded(lua_State* state) {
        char error[2048]{};
        try {
            return Function(state);
        } catch (const std::exception& exception) {
            std::snprintf(error, sizeof(error), "%s", exception.what());
        } catch (...) {
            std::snprintf(error, sizeof(error), "unknown native Lua API error");
        }
        lua_pushstring(state, error);
        return lua_error(state);
    }
    static int traceback(lua_State* state) {
        const char* message =
            lua_type(state, 1) == LUA_TSTRING ? lua_tostring(state, 1) : "non-string Lua error";
        luaL_traceback(state, state, message, 1);
        return 1;
    }
    int protectedCall(lua_CFunction function) {
        lua_settop(state, 0);
        // Zero-upvalue C functions do not allocate. The following pcall protects
        // bootstrapping, argument marshaling and the Lua function itself.
        lua_pushcfunction(state, traceback);
        lua_pushcfunction(state, function);
        instructionsLeft = limits.instructions;
        structuralCommands = 0;
        structuralBudget = {};
        budgetExceeded = false;
        lua_sethook(state, hook, LUA_MASKCOUNT, hookInterval);
        const int status = lua_pcall(state, 0, 0, 1);
        lua_sethook(state, nullptr, 0, 0);
        return status;
    }
    std::string errorText(int status) const {
        if (status == LUA_ERRMEM)
            return "Lua memory budget exhausted";
        if (lua_type(state, -1) == LUA_TSTRING)
            return lua_tostring(state, -1);
        return "Lua execution failed";
    }
    explicit Impl(const LuaProject& value, LuaLimits configured)
        : project(value), limits(configured) {
        require(limits.memoryBytes > 0 && limits.instructions > 0, "Lua budgets must be positive");
        memory.limit = limits.memoryBytes;
        state = lua_newstate(allocate, &memory);
        if (!state)
            throw std::runtime_error("Lua memory budget exhausted during initialization");
        *static_cast<Impl**>(lua_getextraspace(state)) = this;
        try {
            int status = protectedCall(guarded<bootstrap>);
            if (status != LUA_OK)
                throw std::runtime_error(errorText(status));
            std::set<std::string> ids;
            definitions.reserve(project.scripts.size());
            for (const auto& path : project.scripts) {
                require(project.sources.contains(path), "missing Lua entry source: " + path);
                loadingPath = path.c_str();
                status = protectedCall(guarded<loadEntry>);
                loadingModules.clear();
                if (status != LUA_OK)
                    throw std::runtime_error(path + ": " + errorText(status));
                require(ids.insert(definitions.back().id).second,
                        "duplicate Lua behavior id: " + definitions.back().id);
                schemas.push_back(definitions.back().schema);
            }
            loadingPath = nullptr;
            lua_settop(state, 0);
        } catch (...) {
            lua_close(state);
            state = nullptr;
            throw;
        }
    }
    ~Impl() {
        if (state)
            lua_close(state);
    }

    static int bootstrap(lua_State* state) {
        auto& self = get(state);
        luaL_requiref(state, "_G", luaopen_base, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);
        for (const char* name :
             {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", "setmetatable"}) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        // Bytecode serialization has no useful role in an immutable text-only project.
        lua_getglobal(state, "string");
        lua_pushnil(state);
        lua_setfield(state, -2, "dump");
        lua_pop(state, 1);
        // Hide the string metatable as well: otherwise scripts could install
        // __close handlers on strings and execute code during error unwinding.
        lua_pushliteral(state, "");
        if (lua_getmetatable(state, -1)) {
            lua_pushliteral(state, "string");
            lua_setfield(state, -2, "__metatable");
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        lua_newtable(state);
        self.behaviorSet = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_newtable(state);
        static const luaL_Reg entityFunctions[] = {
            {"valid", guarded<entityValid>},
            {"transform", guarded<entityTransform>},
            {"presentation", guarded<entityPresentation>},
            {"fields", guarded<entityFields>},
            {"velocity", guarded<entityVelocity>},
            {"is_grounded", guarded<entityGrounded>},
            {"set_transform", guarded<entitySetTransform>},
            {"set_presentation", guarded<entitySetPresentation>},
            {"teleport", guarded<entityTeleport>},
            {"set_velocity", guarded<entitySetVelocity>},
            {"apply_impulse", guarded<entityImpulse>},
            {"destroy", guarded<entityDestroy>},
            {"add_component", guarded<entityAddComponent>},
            {"remove_component", guarded<entityRemoveComponent>},
            {"__eq", guarded<entityEqual>},
            {nullptr, nullptr}};
        luaL_setfuncs(state, entityFunctions, 0);
        lua_pushvalue(state, -1);
        lua_setfield(state, -2, "__index");
        lua_pushliteral(state, "faset.Entity");
        lua_setfield(state, -2, "__metatable");
        self.entityMetatable = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_newtable(state);
        static const luaL_Reg functions[] = {
            {"behavior", guarded<behavior>}, {"find", guarded<find>},   {"input", guarded<input>},
            {"log", guarded<log>},           {"spawn", guarded<spawn>}, {nullptr, nullptr}};
        luaL_setfuncs(state, functions, 0);
        lua_pushlightuserdata(state, &nullToken);
        lua_setfield(state, -2, "null");
        lua_setglobal(state, "faset");
        lua_pushcfunction(state, guarded<moduleRequire>);
        lua_setglobal(state, "require");
        lua_pushcfunction(state, guarded<log>);
        lua_setglobal(state, "print");
        return 0;
    }
    static int behavior(lua_State* state) {
        require(lua_istable(state, 1), "faset.behavior expects a descriptor table");
        lua_settop(state, 1);
        lua_rawgeti(state, LUA_REGISTRYINDEX, get(state).behaviorSet);
        lua_pushvalue(state, 1);
        lua_pushboolean(state, true);
        lua_rawset(state, -3);
        lua_pop(state, 1);
        return 1;
    }
    static int loadEntry(lua_State* state) {
        auto& self = get(state);
        // The map's referenced source survives every Lua allocation/error.
        const auto& source = self.project.sources.at(self.loadingPath);
        if (luaL_loadbufferx(state, source.data(), source.size(), self.loadingPath, "t") != LUA_OK)
            return lua_error(state);
        lua_call(state, 0, 1);
        require(lua_istable(state, -1), "Lua entry must return a faset.behavior table");
        lua_rawgeti(state, LUA_REGISTRYINDEX, self.behaviorSet);
        lua_pushvalue(state, -2);
        lua_rawget(state, -2);
        const bool declared = lua_toboolean(state, -1);
        lua_pop(state, 2);
        require(declared, "Lua entry must return a faset.behavior table");
        // Copy only metadata; callbacks are Lua functions and never JSON.
        // Field names are pushed BEFORE creating C++ values that own memory.
        self.definitions.emplace_back();
        self.definitions.back().path = self.loadingPath;
        self.definitions.back().schema = Json::object();
        for (const char* key : {"id", "version", "name", "fields", "migrations"}) {
            lua_pushstring(state, key);
            lua_rawget(state, -2);
            if (!lua_isnil(state, -1))
                self.definitions.back().schema[key] = readJson(state, -1, self.metadataBudget);
            lua_pop(state, 1);
        }
        normalizeSchema(self.definitions.back().schema);
        self.definitions.back().id = self.definitions.back().schema.at("id").get<std::string>();
        for (const char* name :
             {"on_start", "fixed_update", "update", "late_update", "on_destroy", "on_collision"}) {
            lua_pushstring(state, name);
            lua_rawget(state, -2);
            require(lua_isnil(state, -1) || lua_isfunction(state, -1),
                    "behavior callback must be a function");
            lua_pop(state, 1);
        }
        self.definitions.back().reference = luaL_ref(state, LUA_REGISTRYINDEX);
        return 0;
    }
    static int moduleRequire(lua_State* state) {
        auto& self = get(state);
        // All allocation-owning temporaries die before loading/calling Lua.
        const std::string* path{};
        const std::string* source{};
        int cached = LUA_NOREF;
        {
            auto name = stringArgument(state, 1);
            require(!name.empty() && name.size() <= 512 && name.front() != '.' &&
                        name.back() != '.',
                    "invalid Lua module name");
            bool dot{};
            for (char& c : name) {
                require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '.',
                        "invalid Lua module name");
                require(!(dot && c == '.'), "invalid Lua module name");
                dot = c == '.';
                if (dot)
                    c = '/';
            }
            auto found = self.project.sources.find("Scripts/" + name + ".lua");
            if (found == self.project.sources.end())
                found = self.project.sources.find("Scripts/" + name + "/init.lua");
            require(found != self.project.sources.end(),
                    "Lua module not in project snapshot: " + name);
            path = &found->first;
            source = &found->second;
            if (const auto loaded = self.modules.find(*path); loaded != self.modules.end())
                cached = loaded->second;
            else
                require(self.loadingModules.insert(*path).second, "cyclic Lua require: " + *path);
        }
        if (cached != LUA_NOREF) {
            lua_rawgeti(state, LUA_REGISTRYINDEX, cached);
            return 1;
        }
        if (luaL_loadbufferx(state, source->data(), source->size(), path->c_str(), "t") != LUA_OK)
            return lua_error(state);
        lua_call(state, 0, 1);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_pushboolean(state, true);
        }
        lua_pushvalue(state, -1);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        self.modules.emplace(*path, reference);
        self.loadingModules.erase(*path);
        return 1;
    }
    runtime::Runtime& world() {
        require(activeRuntime != nullptr,
                "runtime API is only available inside behavior callbacks");
        return *activeRuntime;
    }
    static runtime::EntityHandle entity(lua_State* state, int index = 1) {
        require(lua_type(state, index) == LUA_TUSERDATA &&
                    lua_rawlen(state, index) == sizeof(runtime::EntityHandle),
                "expected a faset.Entity");
        require(lua_getmetatable(state, index) != 0, "expected a faset.Entity");
        lua_rawgeti(state, LUA_REGISTRYINDEX, get(state).entityMetatable);
        const bool valid = lua_rawequal(state, -1, -2);
        lua_pop(state, 2);
        require(valid, "expected a faset.Entity");
        return *static_cast<runtime::EntityHandle*>(lua_touserdata(state, index));
    }
    static void pushEntity(lua_State* state, runtime::EntityHandle handle) {
        auto* value =
            static_cast<runtime::EntityHandle*>(lua_newuserdatauv(state, sizeof(handle), 0));
        *value = handle;
        lua_rawgeti(state, LUA_REGISTRYINDEX, get(state).entityMetatable);
        lua_setmetatable(state, -2);
    }
    static int find(lua_State* state) {
        runtime::EntityHandle handle;
        {
            const auto id = stringArgument(state, 1);
            handle = get(state).world().find(id);
        }
        if (handle)
            pushEntity(state, handle);
        else
            lua_pushnil(state);
        return 1;
    }
    static int input(lua_State* state) {
        const auto input = get(state).world().input();
        lua_createtable(state, 0, 4);
        lua_pushnumber(state, input.horizontal);
        lua_setfield(state, -2, "horizontal");
        lua_pushnumber(state, input.vertical);
        lua_setfield(state, -2, "vertical");
        lua_pushboolean(state, input.jumpPressed);
        lua_setfield(state, -2, "jump_pressed");
        lua_pushboolean(state, input.interactPressed);
        lua_setfield(state, -2, "interact_pressed");
        return 1;
    }
    static int log(lua_State* state) {
        auto& self = get(state);
        if (self.logs.size() >= 256)
            return 0;
        std::string message;
        for (int i = 1; i <= lua_gettop(state) && message.size() < 4096; ++i) {
            if (i > 1)
                message += '\t';
            switch (lua_type(state, i)) {
            case LUA_TSTRING: {
                std::size_t length{};
                const char* text = lua_tolstring(state, i, &length);
                message.append(text, std::min(length, 4096 - message.size()));
                break;
            }
            case LUA_TNUMBER: {
                char buffer[64];
                if (lua_isinteger(state, i))
                    std::snprintf(buffer, sizeof(buffer), "%lld",
                                  static_cast<long long>(lua_tointeger(state, i)));
                else
                    std::snprintf(buffer, sizeof(buffer), "%.14g", lua_tonumber(state, i));
                message += buffer;
                break;
            }
            case LUA_TBOOLEAN:
                message += lua_toboolean(state, i) ? "true" : "false";
                break;
            case LUA_TNIL:
                message += "nil";
                break;
            default:
                message += lua_typename(state, lua_type(state, i));
                break;
            }
        }
        if (message.size() > 4096)
            message.resize(4096);
        self.logs.push_back(std::move(message));
        return 0;
    }
    void structuralCommand() {
        require(++structuralCommands <= maxStructuralCommands,
                "Lua structural command budget exhausted (1024 per callback)");
        structuralBudget.charge(64);
    }
    static int spawn(lua_State* state) {
        auto& self = get(state);
        auto& world = self.world();
        self.structuralCommand();
        world.spawn(readJson(state, 1, self.structuralBudget));
        return 0;
    }
    static int entityValid(lua_State* state) {
        lua_pushboolean(state, get(state).world().valid(entity(state)));
        return 1;
    }
    static int entityEqual(lua_State* state) {
        lua_pushboolean(state, entity(state, 1) == entity(state, 2));
        return 1;
    }
    static int entityTransform(lua_State* state) {
        const auto value = get(state).world().transform(entity(state));
        pushTransform(state, value);
        return 1;
    }
    static int entityPresentation(lua_State* state) {
        const auto value = get(state).world().presentation(entity(state));
        pushTransform(state, value);
        return 1;
    }
    static int entityFields(lua_State* state) {
        auto& self = get(state);
        {
            const auto type = stringArgument(state, 2);
            self.output = Value(self.world().fields(entity(state), type));
        }
        pushValue(state, self.output);
        return 1;
    }
    static int entityVelocity(lua_State* state) {
        const auto value = get(state).world().velocity(entity(state));
        pushVector(state, value);
        return 1;
    }
    static int entityGrounded(lua_State* state) {
        lua_pushboolean(state, get(state).world().grounded(entity(state)));
        return 1;
    }
    static int entitySetTransform(lua_State* state) {
        get(state).world().setTransform(entity(state), readTransform(state, 2));
        return 0;
    }
    static int entitySetPresentation(lua_State* state) {
        get(state).world().setPresentation(entity(state), readTransform(state, 2));
        return 0;
    }
    static int entityTeleport(lua_State* state) {
        get(state).world().teleport(entity(state), readTransform(state, 2));
        return 0;
    }
    static int entitySetVelocity(lua_State* state) {
        get(state).world().setVelocity(entity(state), readVector(state, 2));
        return 0;
    }
    static int entityImpulse(lua_State* state) {
        get(state).world().applyImpulse(entity(state), readVector(state, 2));
        return 0;
    }
    static int entityDestroy(lua_State* state) {
        auto& self = get(state);
        auto& world = self.world();
        self.structuralCommand();
        world.destroy(entity(state));
        return 0;
    }
    static int entityAddComponent(lua_State* state) {
        auto& self = get(state);
        auto& world = self.world();
        self.structuralCommand();
        world.addComponent(entity(state), readJson(state, 2, self.structuralBudget));
        return 0;
    }
    static int entityRemoveComponent(lua_State* state) {
        auto& self = get(state);
        auto& world = self.world();
        self.structuralCommand();
        require(lua_type(state, 2) == LUA_TSTRING, "expected a string");
        std::size_t length{};
        lua_tolstring(state, 2, &length);
        self.structuralBudget.charge(length);
        world.removeComponent(entity(state), stringArgument(state, 2));
        return 0;
    }
    static int dispatch(lua_State* state) {
        auto& self = get(state);
        auto& invocation = *self.activeInvocation;
        auto& instance = *invocation.instance;
        if (instance.reference == LUA_NOREF) {
            lua_createtable(state, 0, 3);
            pushEntity(state, invocation.entity);
            lua_setfield(state, -2, "entity");
            pushValue(state, instance.fields);
            lua_setfield(state, -2, "fields");
            lua_newtable(state);
            lua_setfield(state, -2, "state");
            // Instance method lookup delegates to its definition; self data is isolated.
            lua_newtable(state);
            lua_rawgeti(state, LUA_REGISTRYINDEX, invocation.definition->reference);
            lua_setfield(state, -2, "__index");
            lua_pushliteral(state, "faset.BehaviorInstance");
            lua_setfield(state, -2, "__metatable");
            lua_setmetatable(state, -2);
            instance.reference = luaL_ref(state, LUA_REGISTRYINDEX);
        }
        lua_rawgeti(state, LUA_REGISTRYINDEX, invocation.definition->reference);
        lua_pushstring(state, invocation.callback);
        lua_rawget(state, -2);
        if (lua_isnil(state, -1))
            return 0;
        require(lua_isfunction(state, -1), "behavior callback was replaced with a non-function");
        lua_rawgeti(state, LUA_REGISTRYINDEX, instance.reference);
        int arguments = 1;
        if (invocation.collision) {
            lua_createtable(state, 0, 4);
            lua_pushboolean(state, invocation.collision->began);
            lua_setfield(state, -2, "began");
            pushEntity(state, invocation.collision->first);
            lua_setfield(state, -2, "first");
            pushEntity(state, invocation.collision->second);
            lua_setfield(state, -2, "second");
            pushEntity(state, invocation.collision->first == invocation.entity
                                  ? invocation.collision->second
                                  : invocation.collision->first);
            lua_setfield(state, -2, "other");
            ++arguments;
        } else if (std::strcmp(invocation.callback, "on_start") != 0 && !invocation.destroy) {
            lua_pushnumber(state, invocation.delta);
            ++arguments;
        }
        lua_call(state, arguments, 0);
        return 0;
    }
    static int releaseInstance(lua_State* state) {
        auto& instance = *get(state).activeInvocation->instance;
        if (instance.reference != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, instance.reference);
            instance.reference = LUA_NOREF;
        }
        return 0;
    }
    static int releaseFailedInstance(lua_State* state) {
        releaseInstance(state);
        // A failed instance must not keep its entire self.state reachable and
        // exhaust the shared VM for healthy instances. User finalizers cannot
        // be installed in this sandbox; collection remains protected anyway.
        lua_gc(state, LUA_GCCOLLECT);
        return 0;
    }
    void invoke(std::size_t definitionIndex, runtime::Runtime& world, runtime::EntityHandle handle,
                const char* callback, double delta,
                const runtime::CollisionEvent* collision = nullptr) {
        auto& definition = definitions.at(definitionIndex);
        const Key key{handle.session, handle.slot, handle.generation, definitionIndex};
        const bool destroying = std::strcmp(callback, "on_destroy") == 0;
        auto found = instances.find(key);
        if (found == instances.end()) {
            if (destroying)
                return;
            found = instances.emplace(key, Instance{}).first;
            try {
                auto fields = world.fields(handle, definition.id);
                for (const auto& [name, descriptor] : definition.schema.at("fields").items()) {
                    if (!fields.contains(name))
                        fields[name] = descriptor.at("default");
                    validateField(fields.at(name), descriptor);
                }
                found->second.fields = Value(fields);
            } catch (const std::exception& error) {
                found->second.disabled = true;
                throw std::runtime_error(definition.path + " [" + definition.id +
                                         "]: " + error.what());
            }
        }
        auto& instance = found->second;
        Invocation invocation{&definition, &instance, handle,    callback,
                              delta,       collision, destroying};
        activeRuntime = &world;
        activeInvocation = &invocation;
        struct ActiveCall {
            Impl& host;
            ~ActiveCall() {
                host.activeRuntime = nullptr;
                host.activeInvocation = nullptr;
                host.loadingModules.clear();
                lua_settop(host.state, 0);
            }
        } activeCall{*this};
        int status = LUA_OK;
        std::string error;
        if (!instance.disabled) {
            status = protectedCall(guarded<dispatch>);
            // Marshaling data is needed only until the Lua instance is created.
            // Do not retain duplicate defaults, including after a rejected OOM.
            instance.fields = Value{};
            if (status != LUA_OK) {
                instance.disabled = true;
                error = definition.path + " [" + definition.id + "." + callback +
                        "]: " + errorText(status);
                protectedCall(guarded<releaseFailedInstance>);
            }
        }
        loadingModules.clear();
        if (destroying) {
            // Releasing registry references is itself protected, including after OOM.
            protectedCall(guarded<releaseInstance>);
            instances.erase(found);
        }
        if (status != LUA_OK)
            throw std::runtime_error(error);
    }
};

LuaModule::LuaModule(const LuaProject& project, LuaLimits limits)
    : impl_(std::make_shared<Impl>(project, limits)) {}
LuaModule::~LuaModule() = default;
Json LuaModule::schema() const {
    return impl_->schemas;
}
void LuaModule::validateScene(const Json& scene) const {
    const Json empty = Json::object();
    for (const auto& entity : scene.at("entities")) {
        if (!entity.contains("components"))
            continue;
        for (const auto& component : entity.at("components")) {
            const auto type = component.at("type").get<std::string>();
            const auto definition =
                std::find_if(impl_->definitions.begin(), impl_->definitions.end(),
                             [&](const auto& entry) { return entry.id == type; });
            if (definition == impl_->definitions.end())
                continue;
            try {
                const auto& fields = component.contains("fields") ? component.at("fields") : empty;
                require(fields.is_object(), "component fields must be an object");
                for (const auto& [name, descriptor] : definition->schema.at("fields").items())
                    validateField(fields.contains(name) ? fields.at(name)
                                                        : descriptor.at("default"),
                                  descriptor);
            } catch (const std::exception& error) {
                throw std::runtime_error(definition->path + " [" + type + ", entity " +
                                         entity.value("id", std::string("?")) +
                                         "]: " + error.what());
            }
        }
    }
}
std::vector<std::string> LuaModule::takeLogs() {
    std::vector<std::string> result;
    result.swap(impl_->logs);
    return result;
}
void LuaModule::registerBehaviors(runtime::Runtime& runtime) {
    for (std::size_t i = 0; i < impl_->definitions.size(); ++i) {
        runtime::Behavior behavior;
        auto callback = [host = impl_, i](const char* name) {
            return [host, i, name](runtime::Runtime& world, runtime::EntityHandle entity,
                                   double dt) { host->invoke(i, world, entity, name, dt); };
        };
        behavior.onStart = callback("on_start");
        behavior.fixedUpdate = callback("fixed_update");
        behavior.update = callback("update");
        behavior.lateUpdate = callback("late_update");
        behavior.onDestroy = callback("on_destroy");
        behavior.onCollision = [host = impl_, i](runtime::Runtime& world,
                                                 runtime::EntityHandle entity,
                                                 const runtime::CollisionEvent& event) {
            host->invoke(i, world, entity, "on_collision", 0, &event);
        };
        runtime.registerBehavior(impl_->definitions[i].id, std::move(behavior));
    }
}
} // namespace faset::scripting
