#include "Gameplay.hpp"
#include <iostream>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& world) {
    runtime::Behavior character;
    character.fixedUpdate = [](runtime::Runtime& game, runtime::EntityHandle self, double) {
        const auto settings = game.fields(self, "tutorial.character");
        const auto input = game.input();
        auto velocity = game.velocity(self); // Metres per second; preserve the Y component.
        velocity[0] = input.horizontal * settings.value("speed", 4.0f);
        if (input.jumpPressed && game.grounded(self))
            velocity[1] = settings.value("jump_speed", 5.0f);
        // Do not multiply velocity by delta. The physics solver integrates it.
        game.setVelocity(self, velocity);
    };
    character.onCollision = [](runtime::Runtime&, runtime::EntityHandle,
                               const runtime::CollisionEvent& event) {
        if (event.began)
            std::cout << "Character contact began\n";
    };
    world.registerBehavior("tutorial.character", std::move(character));
}

nlohmann::json schema() {
    return nlohmann::json::array({{{"id", "tutorial.character"},
                                   {"version", 1},
                                   {"name", "Physics character"},
                                   {"fields",
                                    {{"speed",
                                      {{"id", "speed"},
                                       {"type", "number"},
                                       {"default", 4.0},
                                       {"min", 0.0},
                                       {"units", "m/s"}}},
                                     {"jump_speed",
                                      {{"id", "jump_speed"},
                                       {"type", "number"},
                                       {"default", 5.0},
                                       {"min", 0.0},
                                       {"units", "m/s"}}}}}}});
}
} // namespace faset::gameplay
