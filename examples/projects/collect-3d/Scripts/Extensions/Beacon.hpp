#pragma once
#include <faset/runtime/Runtime.hpp>

namespace beacon {
inline nlohmann::json schema() {
    return {{"id", "example.beacon"},
            {"name", "Beacon"},
            {"version", 1},
            {"fields",
             {{"speed",
               {{"id", "speed"},
                {"name", "Rotation speed"},
                {"type", "number"},
                {"default", 1.0},
                {"units", "rad/s"}}}}}};
}
inline void register_behavior(faset::runtime::Runtime& runtime) {
    faset::runtime::Behavior behavior;
    behavior.fixedUpdate = [](faset::runtime::Runtime& world, faset::runtime::EntityHandle self,
                              double dt) {
        auto pose = world.transform(self);
        pose.rotation[1] +=
            world.fields(self, "example.beacon").value("speed", 1.0f) * static_cast<float>(dt);
        world.setTransform(self, pose);
    };
    runtime.registerBehavior("example.beacon", std::move(behavior));
}
} // namespace beacon
