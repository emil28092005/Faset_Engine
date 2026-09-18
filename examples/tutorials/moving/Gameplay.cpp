#include "Gameplay.hpp"

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& world) {
    runtime::Behavior mover;
    mover.update = [](runtime::Runtime& game, runtime::EntityHandle self, double delta) {
        // fields() returns a configuration copy. transform() returns a live pose copy.
        const auto settings = game.fields(self, "tutorial.move_x");
        const float speed = settings.value("speed", 2.0f); // metres per second
        auto pose = game.transform(self);
        pose.position[0] += speed * static_cast<float>(delta);
        game.setTransform(self, pose); // This object has no rigid body.
    };
    world.registerBehavior("tutorial.move_x", std::move(mover));
}

nlohmann::json schema() {
    // The FieldId is the map key. Keep it stable if you change a display name.
    return nlohmann::json::array({{{"id", "tutorial.move_x"},
                                   {"version", 1},
                                   {"name", "Move along X"},
                                   {"fields",
                                    {{"speed",
                                      {{"id", "speed"},
                                       {"type", "number"},
                                       {"default", 2.0},
                                       {"min", -20.0},
                                       {"max", 20.0},
                                       {"units", "m/s"}}}}}}});
}
} // namespace faset::gameplay
