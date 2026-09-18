#include "Physics.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <entt/entt.hpp>
#include <faset/runtime/Runtime.hpp>
#include <faset/runtime/schema.hpp>
#include <numbers>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace faset::runtime {
bool is_builtin_component(std::string_view type) noexcept {
    constexpr std::array<std::string_view, 7> types{
        "faset.transform", "faset.sprite",        "faset.mesh",         "faset.camera",
        "faset.light",     "faset.rigid_body_2d", "faset.rigid_body_3d"};
    return std::find(types.begin(), types.end(), type) != types.end();
}
namespace {
using Json = nlohmann::json;
std::atomic<std::uint64_t> nextSession{1};
constexpr const char* body2 = "faset.rigid_body_2d";
constexpr const char* body3 = "faset.rigid_body_3d";
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::invalid_argument(message);
}
std::uint64_t schemaVersion(const Json& record, const std::string& context) {
    if (!record.contains("version"))
        return 1;
    const auto& value = record.at("version");
    if (value.is_number_unsigned()) {
        const auto version = value.get<std::uint64_t>();
        require(version > 0, context + ": version must be a positive integer");
        return version;
    }
    require(value.is_number_integer(), context + ": version must be a positive integer");
    const auto version = value.get<std::int64_t>();
    require(version > 0, context + ": version must be a positive integer");
    return static_cast<std::uint64_t>(version);
}
template <std::size_t N>
std::array<float, N> vectorValue(const Json& object, const char* key,
                                 std::array<float, N> fallback) {
    if (!object.contains(key))
        return fallback;
    const auto& value = object.at(key);
    require(value.is_array() && value.size() == N, std::string(key) + ": wrong vector size");
    for (std::size_t i = 0; i < N; ++i) {
        require(value[i].is_number(), std::string(key) + ": expected number");
        fallback[i] = value[i].get<float>();
        require(std::isfinite(fallback[i]), std::string(key) + ": nonfinite value");
    }
    return fallback;
}
float number(const Json& fields, const char* key, float fallback) {
    if (!fields.contains(key))
        return fallback;
    require(fields.at(key).is_number(), std::string(key) + ": expected number");
    float v = fields.at(key).get<float>();
    require(std::isfinite(v), std::string(key) + ": nonfinite value");
    return v;
}
Transform readTransform(const Json& fields) {
    return {vectorValue<3>(fields, "position", {0, 0, 0}),
            vectorValue<3>(fields, "rotation", {0, 0, 0}),
            vectorValue<3>(fields, "scale", {1, 1, 1})};
}
void validateTransform(const Transform& t) {
    for (const auto& values : {t.position, t.rotation, t.scale})
        for (float value : values)
            require(std::isfinite(value), "nonfinite transform");
}
Json transformJson(const Transform& t) {
    return {{"position", t.position}, {"rotation", t.rotation}, {"scale", t.scale}};
}
detail::BodySettings settings(const Json& fields, int dimension) {
    detail::BodySettings b;
    b.type = fields.value("body_type", std::string("dynamic"));
    require(b.type == "dynamic" || b.type == "static" || b.type == "kinematic",
            "invalid body_type");
    if (dimension == 2) {
        auto half = vectorValue<2>(fields, "half_extents", {0.5f, 0.5f});
        b.halfExtents = {half[0], half[1], 0.5f};
        auto vel = vectorValue<2>(fields, "linear_velocity", {0, 0});
        b.linearVelocity = {vel[0], vel[1], 0};
    } else {
        b.halfExtents = vectorValue<3>(fields, "half_extents", {0.5f, 0.5f, 0.5f});
        b.linearVelocity = vectorValue<3>(fields, "linear_velocity", {0, 0, 0});
    }
    for (float extent : b.halfExtents)
        require(extent > 0 && extent < 100000, "half_extents must be positive and finite");
    b.density = number(fields, "density", 1);
    b.friction = number(fields, "friction", 0.3f);
    b.restitution = number(fields, "restitution", 0);
    b.gravityScale = number(fields, "gravity_scale", 1);
    require(b.density > 0 && b.friction >= 0 && b.restitution >= 0 && b.restitution <= 1,
            "invalid physics material");
    auto bits = [&](const char* name, std::uint64_t fallback) {
        if (!fields.contains(name))
            return fallback;
        const auto& value = fields.at(name);
        require(value.is_number_unsigned() ||
                    (value.is_number_integer() && value.get<std::int64_t>() >= 0),
                std::string(name) + ": expected nonnegative bits");
        return value.get<std::uint64_t>();
    };
    b.categoryBits = bits("category_bits", 1);
    b.maskBits = bits("mask_bits", ~std::uint64_t{0});
    return b;
}
void validateEntity(const Json& entity, int dimension) {
    require(entity.is_object(), "entity must be an object");
    require(entity.contains("id") && entity["id"].is_string() &&
                !entity["id"].get<std::string>().empty(),
            "entity requires id");
    require(!entity.contains("name") || entity["name"].is_string(), "entity name must be a string");
    require(entity.contains("components") && entity["components"].is_array(),
            "entity requires components array");
    if (entity.contains("parent"))
        require(entity["parent"].is_null() || entity["parent"].is_string(),
                "parent must be an id or null");
    std::set<std::string> types, ids;
    Transform transform{};
    bool physical = false;
    for (const auto& component : entity["components"]) {
        require(component.is_object() && component.contains("id") && component["id"].is_string() &&
                    !component["id"].get<std::string>().empty(),
                "component requires id");
        require(component.contains("type") && component["type"].is_string() &&
                    !component["type"].get<std::string>().empty(),
                "component requires type");
        const auto type = component["type"].get<std::string>();
        const auto version = schemaVersion(component, "component " + type);
        require(!is_builtin_component(type) || version == 1,
                "unsupported builtin component version: " + type);
        require(component.contains("fields") && component["fields"].is_object(),
                "component requires fields");
        require(ids.insert(component["id"].get<std::string>()).second, "duplicate component id");
        require(types.insert(type).second, "duplicate component type");
        const auto& f = component["fields"];
        if (type == "faset.transform")
            transform = readTransform(f);
        if (type == body2 || type == body3) {
            require(type == (dimension == 2 ? body2 : body3),
                    "physics dimension does not match scene");
            settings(f, dimension);
            physical = true;
        }
        if (type == "faset.sprite") {
            vectorValue<4>(f, "color", {1, 1, 1, 1});
            auto size = vectorValue<2>(f, "size", {1, 1});
            require(size[0] > 0 && size[1] > 0, "sprite size must be positive");
            require(!f.contains("texture") || f["texture"].is_string(),
                    "sprite texture must be a string");
            require(!f.contains("layer") || f["layer"].is_number_integer(),
                    "sprite layer must be an integer");
        }
        if (type == "faset.mesh") {
            vectorValue<4>(f, "color", {1, 1, 1, 1});
            require(!f.contains("asset") || f["asset"].is_string(), "mesh asset must be a string");
            require(!f.contains("primitive") || f["primitive"].is_string(),
                    "mesh primitive must be a string");
        }
    }
    if (physical) {
        require(!entity.contains("parent") || entity["parent"].is_null(),
                "physics bodies must be root entities in the initial runtime");
        for (int i = 0; i < dimension; ++i)
            require(std::abs(transform.scale[i]) > 0.00001f, "physics scale must be nonzero");
        if (dimension == 2)
            require(transform.rotation[0] == 0 && transform.rotation[1] == 0,
                    "2D physics rotates only around Z");
    }
}
Transform interpolate(const Transform& a, const Transform& b, float alpha) {
    Transform out;
    for (int i = 0; i < 3; ++i) {
        out.position[i] = std::lerp(a.position[i], b.position[i], alpha);
        out.scale[i] = std::lerp(a.scale[i], b.scale[i], alpha);
    }
    auto quaternion = [](Vec3 r) {
        const float cx = std::cos(r[0] * 0.5f), sx = std::sin(r[0] * 0.5f),
                    cy = std::cos(r[1] * 0.5f), sy = std::sin(r[1] * 0.5f),
                    cz = std::cos(r[2] * 0.5f), sz = std::sin(r[2] * 0.5f);
        return Vec4{sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                    cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
    };
    auto qa = quaternion(a.rotation), qb = quaternion(b.rotation);
    float dot = 0;
    for (int i = 0; i < 4; ++i)
        dot += qa[i] * qb[i];
    if (dot < 0) {
        for (auto& q : qb)
            q = -q;
        dot = -dot;
    }
    float wa = 1 - alpha, wb = alpha;
    if (dot < 0.9995f) {
        const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f)), denom = std::sin(angle);
        wa = std::sin((1 - alpha) * angle) / denom;
        wb = std::sin(alpha * angle) / denom;
    }
    Vec4 q{};
    float length = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = wa * qa[i] + wb * qb[i];
        length += q[i] * q[i];
    }
    for (auto& v : q)
        v /= std::sqrt(length);
    const auto [x, y, z, w] = q;
    out.rotation = {std::atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)),
                    std::asin(std::clamp(2 * (w * y - z * x), -1.0f, 1.0f)),
                    std::atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))};
    return out;
}
} // namespace

void validate_scene_schemas(const Json& scene, const Json& gameplay_schema) {
    require(gameplay_schema.is_array() ||
                (gameplay_schema.is_object() && gameplay_schema.contains("types") &&
                 gameplay_schema.at("types").is_array()),
            "gameplay schema must be an array or a types manifest");
    const auto& schemas =
        gameplay_schema.is_array() ? gameplay_schema : gameplay_schema.at("types");
    std::unordered_map<std::string, std::uint64_t> available;
    for (const auto& schema : schemas) {
        require(schema.is_object() && schema.contains("id") && schema.at("id").is_string() &&
                    !schema.at("id").get<std::string>().empty(),
                "gameplay schema requires a nonempty TypeId");
        const auto id = schema.at("id").get<std::string>();
        const auto version = schemaVersion(schema, "schema " + id);
        require(!is_builtin_component(id),
                "gameplay schema duplicates a builtin component TypeId: " + id);
        require(available.emplace(id, version).second, "duplicate gameplay schema TypeId: " + id);
    }
    require(scene.is_object() && scene.contains("entities") && scene.at("entities").is_array(),
            "scene entities must be an array");
    for (const auto& entity : scene.at("entities")) {
        require(entity.is_object() && entity.contains("components") &&
                    entity.at("components").is_array(),
                "entity components must be an array");
        for (const auto& component : entity.at("components")) {
            require(component.is_object() && component.contains("type") &&
                        component.at("type").is_string() &&
                        !component.at("type").get<std::string>().empty(),
                    "component requires a nonempty TypeId");
            const auto type = component.at("type").get<std::string>();
            const auto version = schemaVersion(component, "component " + type);
            if (is_builtin_component(type)) {
                require(version == 1, "unsupported builtin component version: " + type);
                continue;
            }
            const auto found = available.find(type);
            require(found != available.end(),
                    "component schema is absent from linked gameplay module: " + type);
            require(found->second == version, "component schema version mismatch: " + type +
                                                  " (scene " + std::to_string(version) +
                                                  ", linked gameplay " +
                                                  std::to_string(found->second) + ")");
        }
    }
}

struct Runtime::Impl {
    struct Data {
        Json document;
        std::uint64_t generation;
    };
    struct Pose {
        Transform previous, current, presented;
        bool changedInUpdate{};
    };
    enum class Phase { Idle, Initialize, Fixed, Update, Late, Destroy };
    enum class Kind { Spawn, Destroy, Add, Remove };
    struct Command {
        Kind kind;
        EntityHandle handle;
        Json payload;
        std::string type;
    };
    Runtime* owner;
    RuntimeConfig config;
    entt::registry registry;
    std::unordered_map<std::string, entt::entity> ids;
    std::unordered_map<std::string, Behavior> behaviors;
    std::vector<entt::entity> order;
    std::deque<Command> pending;
    std::unique_ptr<detail::Physics> physics;
    std::vector<CollisionEvent> contacts;
    std::vector<std::string> diagnostics;
    std::uint64_t session{nextSession.fetch_add(1)};
    std::uint64_t generation{}, tick{};
    int dimension{3};
    double accumulator{}, alpha{};
    bool paused{}, busy{};
    InputState currentInput{}, queuedInput{};
    Phase phase{Phase::Idle};
    Impl(Runtime* runtime, RuntimeConfig cfg) : owner(runtime), config(cfg) {}
    EntityHandle handle(entt::entity e) const {
        return {session, entt::to_integral(e), registry.get<Data>(e).generation};
    }
    bool valid(EntityHandle h) const noexcept {
        auto e = static_cast<entt::entity>(h.slot);
        return h.session == session && registry.valid(e) && registry.all_of<Data>(e) &&
               registry.get<Data>(e).generation == h.generation;
    }
    entt::entity entity(EntityHandle h) const {
        if (!valid(h))
            throw std::invalid_argument("stale or foreign runtime handle");
        return static_cast<entt::entity>(h.slot);
    }
    const Json* component(entt::entity e, const std::string& type) const {
        for (const auto& c : registry.get<Data>(e).document["components"])
            if (c["type"] == type)
                return &c;
        return nullptr;
    }
    void callback(const Behavior::Callback& fn, entt::entity e, double dt) {
        if (!fn)
            return;
        try {
            fn(*owner, handle(e), dt);
        } catch (const std::exception& ex) {
            diagnostics.push_back("gameplay " +
                                  registry.get<Data>(e).document["id"].get<std::string>() + ": " +
                                  ex.what());
        } catch (...) {
            diagnostics.push_back("unknown gameplay exception");
        }
    }
    void lifecycle(entt::entity e, Behavior::Callback Behavior::* member, double dt) {
        const auto components = registry.get<Data>(e).document["components"];
        for (const auto& c : components) {
            auto it = behaviors.find(c["type"].get<std::string>());
            if (it != behaviors.end())
                callback(it->second.*member, e, dt);
        }
    }
    void all(Behavior::Callback Behavior::* member, double dt) {
        for (auto e : order)
            if (registry.valid(e))
                lifecycle(e, member, dt);
    }
    void syncVisual(entt::entity e) {
        if (auto c = component(e, "faset.sprite")) {
            const auto& f = (*c)["fields"];
            registry.emplace_or_replace<Sprite>(
                e, vectorValue<4>(f, "color", {1, 1, 1, 1}), vectorValue<2>(f, "size", {1, 1}),
                f.value("texture", std::string{}), f.value("layer", 0));
        } else
            registry.remove<Sprite>(e);
        if (auto c = component(e, "faset.mesh")) {
            const auto& f = (*c)["fields"];
            registry.emplace_or_replace<Mesh>(e, f.value("asset", std::string{}),
                                              vectorValue<4>(f, "color", {1, 1, 1, 1}),
                                              f.value("primitive", std::string("cube")));
        } else
            registry.remove<Mesh>(e);
    }
    void addPhysics(entt::entity e) {
        require(bool(physics), "load a scene before creating physics");
        auto c = component(e, dimension == 2 ? body2 : body3);
        if (c)
            physics->add(entt::to_integral(e), registry.get<Pose>(e).current,
                         settings((*c)["fields"], dimension));
    }
    entt::entity create(Json document) {
        const auto id = document["id"].get<std::string>();
        require(!ids.contains(id), "duplicate entity id: " + id);
        auto e = registry.create();
        Transform t{};
        for (const auto& c : document["components"])
            if (c["type"] == "faset.transform")
                t = readTransform(c["fields"]);
        registry.emplace<Data>(e, std::move(document), ++generation);
        registry.emplace<Pose>(e, t, t, t, false);
        ids.emplace(id, e);
        order.push_back(e);
        addPhysics(e);
        syncVisual(e);
        return e;
    }
    void erase(entt::entity e) {
        // Authoring hierarchy destruction has the same subtree semantics in runtime.
        auto id = registry.get<Data>(e).document["id"].get<std::string>();
        std::vector<entt::entity> children;
        for (auto child : order)
            if (registry.valid(child) &&
                registry.get<Data>(child).document.value("parent", Json{}) == id)
                children.push_back(child);
        for (auto child : children)
            erase(child);
        phase = Phase::Destroy;
        lifecycle(e, &Behavior::onDestroy, 0);
        physics->remove(entt::to_integral(e));
        ids.erase(id);
        registry.destroy(e);
        std::erase(order, e);
    }
    void commands() {
        auto commands = std::move(pending);
        pending.clear();
        for (auto& command : commands)
            try {
                if (command.kind == Kind::Spawn) {
                    validateEntity(command.payload, dimension);
                    auto parent = command.payload.value("parent", Json{});
                    require(parent.is_null() || ids.contains(parent.get<std::string>()),
                            "spawn parent is absent");
                    auto e = create(std::move(command.payload));
                    phase = Phase::Initialize;
                    lifecycle(e, &Behavior::onStart, 0);
                    continue;
                }
                if (!valid(command.handle)) {
                    diagnostics.push_back("ignored structural command for stale handle");
                    continue;
                }
                auto e = entity(command.handle);
                if (command.kind == Kind::Destroy) {
                    erase(e);
                    continue;
                }
                auto candidate = registry.get<Data>(e).document;
                auto& components = candidate["components"];
                if (command.kind == Kind::Add) {
                    components.push_back(command.payload);
                    validateEntity(candidate, dimension);
                } else {
                    auto it =
                        std::find_if(components.begin(), components.end(),
                                     [&](const Json& c) { return c["type"] == command.type; });
                    if (it == components.end())
                        continue;
                    if (command.type == "faset.transform" && physics->contains(command.handle.slot))
                        throw std::invalid_argument("remove physics before removing transform");
                    auto behavior = behaviors.find(command.type);
                    if (behavior != behaviors.end()) {
                        phase = Phase::Destroy;
                        callback(behavior->second.onDestroy, e, 0);
                    }
                    components.erase(it);
                }
                const std::string changed = command.kind == Kind::Add
                                                ? command.payload["type"].get<std::string>()
                                                : command.type;
                if ((changed == body2 || changed == body3) && command.kind == Kind::Add) {
                    const auto& pose = registry.get<Pose>(e).current;
                    for (int i = 0; i < dimension; ++i)
                        require(std::abs(pose.scale[i]) > 0.00001f,
                                "runtime physics scale must be nonzero");
                    if (dimension == 2)
                        require(pose.rotation[0] == 0 && pose.rotation[1] == 0,
                                "2D physics rotates only around Z");
                }
                registry.get<Data>(e).document = std::move(candidate);
                syncVisual(e);
                if (changed == body2 || changed == body3) {
                    if (command.kind == Kind::Add)
                        addPhysics(e);
                    else
                        physics->remove(command.handle.slot);
                }
                if (changed == "faset.transform") {
                    auto& d = registry.get<Pose>(e);
                    d.current = command.kind == Kind::Add ? readTransform(command.payload["fields"])
                                                          : Transform{};
                    d.previous = d.presented = d.current;
                }
                if (command.kind == Kind::Add) {
                    auto it = behaviors.find(changed);
                    if (it != behaviors.end()) {
                        phase = Phase::Initialize;
                        callback(it->second.onStart, e, 0);
                    }
                }
            } catch (const std::exception& ex) {
                diagnostics.push_back(std::string("structural command rejected: ") + ex.what());
            }
    }
    void fixed() {
        commands();
        phase = Phase::Fixed;
        for (auto [e, d] : registry.view<Pose>().each()) {
            (void)e;
            d.previous = d.current;
        }
        currentInput = queuedInput;
        queuedInput.jumpPressed = false;
        queuedInput.interactPressed = false;
        all(&Behavior::fixedUpdate, config.fixedDelta);
        contacts.clear();
        if (physics) {
            auto events = physics->step(static_cast<float>(config.fixedDelta));
            for (auto e : order)
                if (physics->contains(entt::to_integral(e))) {
                    auto& d = registry.get<Pose>(e);
                    d.current = physics->transform(entt::to_integral(e), d.current);
                }
            for (const auto& event : events) {
                auto a = static_cast<entt::entity>(event.first),
                     b = static_cast<entt::entity>(event.second);
                if (!registry.valid(a) || !registry.valid(b))
                    continue;
                contacts.push_back({handle(a), handle(b), event.began});
            }
            for (const auto& event : contacts)
                for (auto h : {event.first, event.second}) {
                    auto e = entity(h);
                    for (const auto& c : registry.get<Data>(e).document["components"]) {
                        auto it = behaviors.find(c["type"].get<std::string>());
                        if (it != behaviors.end() && it->second.onCollision)
                            try {
                                it->second.onCollision(*owner, h, event);
                            } catch (const std::exception& ex) {
                                diagnostics.push_back(std::string("collision callback: ") +
                                                      ex.what());
                            } catch (...) {
                                diagnostics.push_back("unknown collision callback exception");
                            }
                    }
                }
        }
        ++tick;
        phase = Phase::Idle;
    }
    FrameStats frame(double elapsed, InputState input, bool step) {
        require(std::isfinite(elapsed) && elapsed >= 0,
                "elapsed time must be finite and nonnegative");
        require(!busy, "recursive runtime advance");
        require(std::isfinite(input.horizontal) && std::isfinite(input.vertical),
                "input axes must be finite");
        struct Guard {
            bool& busy;
            ~Guard() {
                busy = false;
            }
        } guard{busy};
        busy = true;
        FrameStats stats{};
        if (paused && !step) {
            accumulator = 0;
            currentInput = {};
            queuedInput = {};
            return {0, 0, alpha, tick};
        }
        queuedInput.horizontal = input.horizontal;
        queuedInput.vertical = input.vertical;
        queuedInput.jumpPressed = queuedInput.jumpPressed || input.jumpPressed;
        queuedInput.interactPressed = queuedInput.interactPressed || input.interactPressed;
        for (auto [e, d] : registry.view<Pose>().each()) {
            (void)e;
            d.changedInUpdate = false;
        }
        accumulator += step ? config.fixedDelta : elapsed;
        while (accumulator + 1e-12 >= config.fixedDelta &&
               stats.fixedTicks < (step ? 1u : config.maxCatchUpTicks)) {
            fixed();
            accumulator = std::max(0.0, accumulator - config.fixedDelta);
            ++stats.fixedTicks;
        }
        if (accumulator >= config.fixedDelta) {
            auto remaining = std::fmod(accumulator, config.fixedDelta);
            stats.droppedTime = accumulator - remaining;
            accumulator = remaining;
            diagnostics.push_back("dropped_time=" + std::to_string(stats.droppedTime));
        }
        currentInput = input;
        phase = Phase::Update;
        all(&Behavior::update, step ? config.fixedDelta : elapsed);
        alpha = step ? 1.0 : std::clamp(accumulator / config.fixedDelta, 0.0, 1.0);
        for (auto [e, d] : registry.view<Pose>().each()) {
            (void)e;
            d.presented = d.changedInUpdate
                              ? d.current
                              : interpolate(d.previous, d.current, static_cast<float>(alpha));
        }
        phase = Phase::Late;
        all(&Behavior::lateUpdate, step ? config.fixedDelta : elapsed);
        phase = Phase::Idle;
        stats.interpolationAlpha = alpha;
        stats.tick = tick;
        return stats;
    }
};

Runtime::Runtime(RuntimeConfig cfg) : impl_(std::make_unique<Impl>(this, cfg)) {
    require(std::isfinite(cfg.fixedDelta) && cfg.fixedDelta > 0 && cfg.fixedDelta <= 1,
            "invalid fixed delta");
    require(cfg.maxCatchUpTicks > 0 && cfg.maxCatchUpTicks <= 1024, "invalid catchup limit");
    require(cfg.physicsSubsteps > 0 && cfg.physicsSubsteps <= 128, "invalid physics substeps");
    for (float value : cfg.gravity)
        require(std::isfinite(value), "invalid gravity");
}
Runtime::~Runtime() {
    try {
        clear();
    } catch (...) {
    }
}
void Runtime::registerBehavior(std::string type, Behavior behavior) {
    require(!impl_->busy && impl_->ids.empty(), "register gameplay before loading scene");
    require(!type.empty() && !impl_->behaviors.contains(type), "duplicate or empty behavior type");
    impl_->behaviors.emplace(std::move(type), std::move(behavior));
}
void Runtime::load(const Json& scene) {
    require(!impl_->busy, "cannot load scene from gameplay callback");
    require(scene.is_object() && scene.value("format", std::string{}) == "faset.scene" &&
                scene.value("version", 0) == 1,
            "unsupported scene format/version");
    const int dimension = scene.value("dimension", 3);
    require(dimension == 2 || dimension == 3, "scene dimension must be 2 or 3");
    require(scene.contains("entities") && scene["entities"].is_array(),
            "scene entities must be an array");
    require(!scene.contains("instances") ||
                (scene["instances"].is_array() && scene["instances"].empty()),
            "resolve template instances before runtime loading");
    std::unordered_map<std::string, Json> entities;
    for (const auto& entity : scene["entities"]) {
        validateEntity(entity, dimension);
        require(entities.emplace(entity["id"].get<std::string>(), entity).second,
                "duplicate scene entity id");
    }
    for (const auto& [id, entity] : entities) {
        std::set<std::string> visited{id};
        auto parent = entity.value("parent", Json{});
        while (!parent.is_null()) {
            auto key = parent.get<std::string>();
            require(entities.contains(key), "unknown parent entity");
            require(visited.insert(key).second, "cyclic parent hierarchy");
            parent = entities.at(key).value("parent", Json{});
        }
    }
    auto next = std::make_unique<Impl>(this, impl_->config);
    next->dimension = dimension;
    next->behaviors = impl_->behaviors;
    next->physics = std::make_unique<detail::Physics>(dimension, next->config.gravity,
                                                      next->config.physicsSubsteps);
    for (const auto& entity : scene["entities"])
        next->create(entity);
    clear();
    impl_ = std::move(next);
    impl_->busy = true;
    impl_->phase = Impl::Phase::Initialize;
    impl_->all(&Behavior::onStart, 0);
    impl_->phase = Impl::Phase::Idle;
    impl_->busy = false;
}
void Runtime::clear() {
    require(!impl_->busy, "cannot clear runtime from gameplay callback");
    impl_->busy = true;
    while (!impl_->order.empty())
        impl_->erase(impl_->order.back());
    impl_->pending.clear();
    impl_->contacts.clear();
    impl_->physics.reset();
    impl_->accumulator = 0;
    impl_->tick = 0;
    impl_->session = nextSession.fetch_add(1);
    impl_->busy = false;
}
FrameStats Runtime::advance(double dt, InputState input) {
    return impl_->frame(dt, input, false);
}
FrameStats Runtime::singleStep(InputState input) {
    return impl_->frame(0, input, true);
}
void Runtime::setPaused(bool value) {
    require(!impl_->busy, "pause control belongs outside gameplay callbacks");
    impl_->paused = value;
    impl_->accumulator = 0;
    impl_->queuedInput = {};
    impl_->alpha = 0;
    for (auto [e, pose] : impl_->registry.view<Impl::Pose>().each()) {
        (void)e;
        pose.previous = pose.presented = pose.current;
    }
}
bool Runtime::paused() const noexcept {
    return impl_->paused;
}
EntityHandle Runtime::find(const std::string& id) const {
    auto it = impl_->ids.find(id);
    return it == impl_->ids.end() ? EntityHandle{} : impl_->handle(it->second);
}
bool Runtime::valid(EntityHandle handle) const noexcept {
    return impl_->valid(handle);
}
Transform Runtime::transform(EntityHandle h) const {
    return impl_->registry.get<Impl::Pose>(impl_->entity(h)).current;
}
Transform Runtime::presentation(EntityHandle h) const {
    return impl_->registry.get<Impl::Pose>(impl_->entity(h)).presented;
}
Json Runtime::fields(EntityHandle h, const std::string& type) const {
    auto c = impl_->component(impl_->entity(h), type);
    if (!c)
        throw std::invalid_argument("entity has no component: " + type);
    return (*c)["fields"];
}
Vec3 Runtime::velocity(EntityHandle h) const {
    impl_->entity(h);
    if (!impl_->physics || !impl_->physics->contains(h.slot))
        throw std::invalid_argument("entity has no physics body");
    return impl_->physics->velocity(h.slot);
}
bool Runtime::grounded(EntityHandle h) const {
    impl_->entity(h);
    if (!impl_->physics || !impl_->physics->contains(h.slot))
        throw std::invalid_argument("entity has no physics body");
    return impl_->physics->grounded(h.slot);
}
InputState Runtime::input() const noexcept {
    return impl_->currentInput;
}
const std::vector<CollisionEvent>& Runtime::collisions() const noexcept {
    return impl_->contacts;
}
void Runtime::setTransform(EntityHandle h, const Transform& value) {
    validateTransform(value);
    auto e = impl_->entity(h);
    if (impl_->physics && impl_->physics->contains(h.slot))
        throw std::invalid_argument("physics transform requires teleport");
    auto& d = impl_->registry.get<Impl::Pose>(e);
    d.current = value;
    if (impl_->phase != Impl::Phase::Fixed) {
        d.previous = d.presented = value;
        d.changedInUpdate = true;
    }
}
void Runtime::setPresentation(EntityHandle h, const Transform& value) {
    validateTransform(value);
    require(impl_->phase == Impl::Phase::Late,
            "presentation may only be changed during LateUpdate");
    impl_->registry.get<Impl::Pose>(impl_->entity(h)).presented = value;
}
void Runtime::teleport(EntityHandle h, const Transform& value) {
    validateTransform(value);
    auto e = impl_->entity(h);
    auto& d = impl_->registry.get<Impl::Pose>(e);
    if (impl_->physics && impl_->physics->contains(h.slot)) {
        require(d.current.scale == value.scale, "changing collider scale requires remove/add body");
        if (impl_->dimension == 2)
            require(value.rotation[0] == 0 && value.rotation[1] == 0,
                    "2D physics rotates only around Z");
        impl_->physics->teleport(h.slot, value);
    }
    d.previous = d.current = d.presented = value;
    d.changedInUpdate = true;
}
void Runtime::setVelocity(EntityHandle h, Vec3 value) {
    impl_->entity(h);
    for (float v : value)
        require(std::isfinite(v), "nonfinite velocity");
    if (!impl_->physics || !impl_->physics->contains(h.slot))
        throw std::invalid_argument("entity has no physics body");
    impl_->physics->setVelocity(h.slot, value);
}
void Runtime::applyImpulse(EntityHandle h, Vec3 value) {
    impl_->entity(h);
    for (float v : value)
        require(std::isfinite(v), "nonfinite impulse");
    if (!impl_->physics || !impl_->physics->contains(h.slot))
        throw std::invalid_argument("entity has no physics body");
    impl_->physics->impulse(h.slot, value);
}
void Runtime::spawn(Json entity) {
    require(bool(impl_->physics), "load a scene before spawning");
    validateEntity(entity, impl_->dimension);
    impl_->pending.push_back({Impl::Kind::Spawn, {}, std::move(entity), {}});
}
void Runtime::destroy(EntityHandle h) {
    impl_->entity(h);
    impl_->pending.push_back({Impl::Kind::Destroy, h, {}, {}});
}
void Runtime::addComponent(EntityHandle h, Json component) {
    impl_->entity(h);
    impl_->pending.push_back({Impl::Kind::Add, h, std::move(component), {}});
}
void Runtime::removeComponent(EntityHandle h, const std::string& type) {
    impl_->entity(h);
    impl_->pending.push_back({Impl::Kind::Remove, h, {}, type});
}
RuntimeSnapshot Runtime::snapshot() const {
    RuntimeSnapshot out{impl_->dimension, impl_->tick, impl_->alpha, {}};
    out.entities.reserve(impl_->order.size());
    for (auto e : impl_->order) {
        const auto& d = impl_->registry.get<Impl::Data>(e);
        RenderEntity item;
        item.id = d.document["id"].get<std::string>();
        item.name = d.document.value("name", item.id);
        item.transform = impl_->registry.get<Impl::Pose>(e).presented;
        if (d.document.contains("parent") && !d.document["parent"].is_null())
            item.parent = d.document["parent"].get<std::string>();
        if (auto sprite = impl_->registry.try_get<Sprite>(e))
            item.sprite = *sprite;
        if (auto mesh = impl_->registry.try_get<Mesh>(e))
            item.mesh = *mesh;
        out.entities.push_back(std::move(item));
    }
    return out;
}
Json Runtime::snapshotJson() const {
    auto value = snapshot();
    Json entities = Json::array();
    for (const auto& e : value.entities) {
        Json item{{"id", e.id},
                  {"name", e.name},
                  {"parent", e.parent ? Json(*e.parent) : Json{}},
                  {"transform", transformJson(e.transform)}};
        if (e.sprite)
            item["sprite"] = {{"color", e.sprite->color},
                              {"size", e.sprite->size},
                              {"texture", e.sprite->texture},
                              {"layer", e.sprite->layer}};
        if (e.mesh)
            item["mesh"] = {{"asset", e.mesh->asset},
                            {"color", e.mesh->color},
                            {"primitive", e.mesh->primitive}};
        const auto entity = impl_->ids.at(e.id);
        for (const auto& type : {"faset.camera", "faset.light"})
            if (auto c = impl_->component(entity, type))
                item[type == std::string("faset.camera") ? "camera" : "light"] = (*c)["fields"];
        entities.push_back(std::move(item));
    }
    return {{"dimension", value.dimension},
            {"tick", value.tick},
            {"alpha", value.alpha},
            {"entities", entities}};
}
std::uint64_t Runtime::session() const noexcept {
    return impl_->session;
}
const std::vector<std::string>& Runtime::diagnostics() const noexcept {
    return impl_->diagnostics;
}
} // namespace faset::runtime
