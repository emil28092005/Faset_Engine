#include "Gameplay.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(float a, float b, const char* message) {
    check(std::abs(a - b) < 0.002f, message);
}
} // namespace
int main() {
    try {
        nlohmann::json scene;
        std::ifstream input(FASET_TUTORIAL_SCENE);
        input >> scene;
        std::set<std::string> stableIds;
        for (const auto& entity : scene.at("entities")) {
            check(stableIds.insert(entity.at("id").get<std::string>()).second,
                  "entity stable ID must be globally unique");
            for (const auto& component : entity.at("components"))
                check(stableIds.insert(component.at("id").get<std::string>()).second,
                      "component stable ID must be globally unique for Editor authoring");
        }
        const auto types = faset::gameplay::schema();
        check(types.is_array() && !types.empty(), "tutorial must export real component schemas");
        for (const auto& type : types)
            for (const auto& [id, field] : type.at("fields").items())
                check(field.at("id") == id && field.contains("default"),
                      "schema FieldId/default contract");
        faset::runtime::Runtime world;
        faset::gameplay::registerGameplay(world);
        world.load(scene);
        const std::string tutorial = FASET_TUTORIAL_NAME;
        if (tutorial == "moving") {
            for (int i = 0; i < 60; ++i)
                world.advance(1.0 / 60);
            near(world.transform(world.find("actor")).position[0], 2,
                 "60 Hz motion covers two metres per second");
            world.load(scene);
            for (int i = 0; i < 30; ++i)
                world.advance(1.0 / 30);
            near(world.transform(world.find("actor")).position[0], 2,
                 "30 Hz motion covers the same distance");
        } else if (tutorial == "following") {
            world.advance(1.5 / 60);
            near(world.presentation(world.find("actor")).position[0], 1.0f / 60,
                 "presentation halfway between completed fixed poses");
            near(world.presentation(world.find("camera")).position[0],
                 world.presentation(world.find("actor")).position[0],
                 "LateUpdate follows interpolated target");
            near(world.transform(world.find("camera")).position[0], 0,
                 "following does not change simulation transform");
            const auto target = world.find("actor");
            world.destroy(target);
            world.singleStep();
            check(!world.valid(target), "target handle invalid after destruction");
        } else if (tutorial == "spawning") {
            check(!world.find("temporary-box"), "OnStart spawn deferred until first tick");
            world.singleStep();
            auto spawned = world.find("temporary-box");
            check(world.valid(spawned), "child created at first barrier");
            for (int i = 0; i < 90; ++i)
                world.singleStep();
            check(!world.valid(spawned) && !world.find("temporary-box"),
                  "lifetime removes temporary entity");
            world.load(scene);
            world.singleStep();
            check(world.valid(world.find("temporary-box")) && !world.valid(spawned),
                  "module state and handles work across scene restart");
        } else if (tutorial == "physics") {
            auto self = world.find("actor");
            for (int i = 0; i < 10; ++i)
                world.advance(1.0 / 60);
            check(world.grounded(self), "controller starts on floor");
            world.advance(1.0 / 60, {1, 0, true, false});
            check(world.velocity(self)[0] > 3.5f && world.velocity(self)[1] > 4,
                  "input applies velocity and grounded jump");
            for (int i = 0; i < 90 && world.velocity(self)[1] > 0.1f; ++i)
                world.advance(1.0 / 60);
            check(!world.grounded(self), "apex has no ground contact");
            const float before = world.velocity(self)[1];
            world.advance(1.0 / 60, {0, 0, true, false});
            check(world.velocity(self)[1] < before, "controller refuses air jump");
            for (int i = 0; i < 180; ++i)
                world.advance(1.0 / 60);
            check(world.grounded(self), "controller lands again");
        } else
            throw std::runtime_error("Unknown compiled tutorial");
        check(world.diagnostics().empty(), "tutorial callbacks must not silently report errors");
        std::cout << tutorial << " tutorial compiled and passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
