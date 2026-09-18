#include "Gameplay.hpp"
#include <map>
#include <tuple>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& world) {
    runtime::Behavior spawner;
    spawner.onStart = [](runtime::Runtime& game, runtime::EntityHandle self, double) {
        const auto settings = game.fields(self, "tutorial.spawn_once");
        const auto id = settings.at("spawned_id").get<std::string>();
        // This queues a runtime object; it does not change the saved scene.
        game.spawn(
            {{"id", id},
             {"name", "Temporary box"},
             {"parent", nullptr},
             {"components", nlohmann::json::array(
                                {{{"id", id + "/transform"},
                                  {"type", "faset.transform"},
                                  {"version", 1},
                                  {"fields", {{"position", {0, 0, 0}}}}},
                                 {{"id", id + "/sprite"},
                                  {"type", "faset.sprite"},
                                  {"version", 1},
                                  {"fields", {{"color", {0.9, 0.6, 0.2, 1}}, {"size", {1, 1}}}}},
                                 {{"id", id + "/lifetime"},
                                  {"type", "tutorial.timed_despawn"},
                                  {"version", 1},
                                  {"fields", {{"seconds", settings.value("lifetime", 1.0)}}}}})}});
    };
    world.registerBehavior("tutorial.spawn_once", std::move(spawner));

    // Ordinary C++ state belongs to the gameplay module. Every handle part matters.
    using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>;
    auto ages = std::make_shared<std::map<Key, double>>();
    auto key = [](runtime::EntityHandle h) { return Key{h.session, h.slot, h.generation}; };
    runtime::Behavior lifetime;
    lifetime.onStart = [ages, key](runtime::Runtime&, runtime::EntityHandle self, double) {
        (*ages)[key(self)] = 0.0;
    };
    lifetime.fixedUpdate = [ages, key](runtime::Runtime& game, runtime::EntityHandle self,
                                       double delta) {
        auto& age = ages->at(key(self));
        age += delta;
        if (age >= game.fields(self, "tutorial.timed_despawn").value("seconds", 1.0))
            game.destroy(self); // Still valid until the next fixed-tick barrier.
    };
    lifetime.onDestroy = [ages, key](runtime::Runtime&, runtime::EntityHandle self, double) {
        ages->erase(key(self));
    };
    world.registerBehavior("tutorial.timed_despawn", std::move(lifetime));
}

nlohmann::json schema() {
    return nlohmann::json::array(
        {{{"id", "tutorial.spawn_once"},
          {"version", 1},
          {"name", "Spawn once"},
          {"fields",
           {{"spawned_id",
             {{"id", "spawned_id"}, {"type", "string"}, {"default", "temporary-box"}}},
            {"lifetime",
             {{"id", "lifetime"}, {"type", "number"}, {"default", 1.0}, {"min", 0.0}}}}}},
         {{"id", "tutorial.timed_despawn"},
          {"version", 1},
          {"name", "Timed despawn"},
          {"fields",
           {{"seconds",
             {{"id", "seconds"}, {"type", "number"}, {"default", 1.0}, {"min", 0.0}}}}}}});
}
} // namespace faset::gameplay
