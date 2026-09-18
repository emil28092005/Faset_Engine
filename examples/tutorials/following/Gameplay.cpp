#include "Gameplay.hpp"

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& world) {
    runtime::Behavior motion;
    motion.fixedUpdate = [](runtime::Runtime& game, runtime::EntityHandle self, double delta) {
        auto pose = game.transform(self);
        pose.position[0] += game.fields(self, "tutorial.fixed_move").value("speed", 2.0f) *
                            static_cast<float>(delta);
        game.setTransform(self, pose);
    };
    world.registerBehavior("tutorial.fixed_move", std::move(motion));

    runtime::Behavior follow;
    follow.lateUpdate = [](runtime::Runtime& game, runtime::EntityHandle self, double) {
        const auto settings = game.fields(self, "tutorial.follow");
        const auto target = game.find(settings.value("target", std::string{}));
        if (!game.valid(target))
            return;                                        // Target may be absent or destroyed.
        const auto targetPose = game.presentation(target); // Already interpolated.
        const auto offset = settings.at("offset").get<runtime::Vec3>();
        auto cameraPose = game.presentation(self);
        for (int axis = 0; axis < 3; ++axis)
            cameraPose.position[axis] = targetPose.position[axis] + offset[axis];
        game.setPresentation(self, cameraPose); // Does not write the simulation pose.
    };
    world.registerBehavior("tutorial.follow", std::move(follow));
}

nlohmann::json schema() {
    return nlohmann::json::array(
        {{{"id", "tutorial.fixed_move"},
          {"version", 1},
          {"name", "Fixed movement"},
          {"fields", {{"speed", {{"id", "speed"}, {"type", "number"}, {"default", 2.0}}}}}},
         {{"id", "tutorial.follow"},
          {"version", 1},
          {"name", "Follow presentation"},
          {"fields",
           {{"target", {{"id", "target"}, {"type", "entity_ref"}, {"default", "actor"}}},
            {"offset", {{"id", "offset"}, {"type", "vec3"}, {"default", {0, 0, 10}}}}}}}});
}
} // namespace faset::gameplay
