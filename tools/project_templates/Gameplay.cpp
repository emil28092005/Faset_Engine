#include "Gameplay.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& engine) {
    runtime::Behavior character;
    character.fixedUpdate = [](runtime::Runtime& world, runtime::EntityHandle self, double) {
        const auto fields = world.fields(self, "gameplay.character");
        auto velocity = world.velocity(self);
        const auto input = world.input();
        velocity[0] = input.horizontal * fields.value("speed", 4.0f);
        // Support comes from native contact normals, not velocity at the jump apex.
        if (input.jumpPressed && world.grounded(self))
            velocity[1] = fields.value("jump_speed", 5.0f);
        world.setVelocity(self, velocity);
    };
    engine.registerBehavior("gameplay.character", std::move(character));

    runtime::Behavior door;
    auto open = std::make_shared<std::map<std::pair<std::uint64_t, std::uint32_t>, bool>>();
    door.onStart = [open](runtime::Runtime&, runtime::EntityHandle self, double) {
        (*open)[{self.session, self.slot}] = false;
    };
    door.onDestroy = [open](runtime::Runtime&, runtime::EntityHandle self, double) {
        open->erase({self.session, self.slot});
    };
    door.fixedUpdate = [open](runtime::Runtime& world, runtime::EntityHandle self, double dt) {
        const auto fields = world.fields(self, "gameplay.door");
        auto pose = world.transform(self);
        auto& opened = (*open)[{self.session, self.slot}];
        if (world.input().interactPressed)
            opened = !opened;
        const float target =
            opened ? fields.value("open_angle", 1.5707963f) : fields.value("closed_angle", 0.0f);
        const float distance = target - pose.rotation[1];
        const float amount = std::max(0.0f, fields.value("speed", 1.5f)) * static_cast<float>(dt);
        pose.rotation[1] += std::clamp(distance, -amount, amount);
        world.setTransform(self, pose);
    };
    engine.registerBehavior("gameplay.door", std::move(door));
}

nlohmann::json schema() {
    // Explicit declarations shared by Player and SchemaExporter. This function
    // constructs descriptions only: no Runtime, physics world or lifecycle.
    return nlohmann::json::array(
        {{{"id", "gameplay.character"},
          {"version", 1},
          {"name", "Character"},
          {"fields",
           {{"speed", {{"id", "speed"}, {"type", "number"}, {"default", 4.0}, {"min", 0.0}}},
            {"jump_speed",
             {{"id", "jump_speed"}, {"type", "number"}, {"default", 5.0}, {"min", 0.0}}}}}},
         {{"id", "gameplay.door"},
          {"version", 1},
          {"name", "Door"},
          {"fields",
           {{"open_angle",
             {{"id", "open_angle"}, {"type", "number"}, {"default", 1.5707963}, {"units", "rad"}}},
            {"closed_angle",
             {{"id", "closed_angle"}, {"type", "number"}, {"default", 0.0}, {"units", "rad"}}},
            {"speed",
             {{"id", "speed"},
              {"type", "number"},
              {"default", 1.5},
              {"min", 0.0},
              {"units", "rad/s"}}}}}}});
}
} // namespace faset::gameplay
