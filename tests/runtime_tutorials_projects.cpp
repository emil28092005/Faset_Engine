#include "Gameplay.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace faset::runtime;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
nlohmann::json read(const std::filesystem::path& path) {
    nlohmann::json value;
    std::ifstream input(path);
    input >> value;
    return value;
}
} // namespace
int main() {
    try {
        const std::filesystem::path project = FASET_EXAMPLE_PROJECT;
        const auto manifest = read(project / "project.faset.json");
        const auto scene = read(project / manifest.at("start_scene").get<std::string>());
        check(manifest.at("dimension") == FASET_EXAMPLE_DIMENSION,
              "project dimension matches test");
        std::set<std::string> ids;
        for (const auto& entity : scene.at("entities")) {
            check(ids.insert(entity.at("id").get<std::string>()).second, "unique entity IDs");
            for (const auto& component : entity.at("components"))
                check(ids.insert(component.at("id").get<std::string>()).second,
                      "unique component IDs");
        }
        const auto schemas = faset::gameplay::schema();
        check(schemas.size() == (FASET_EXAMPLE_DIMENSION == 3 ? 2u : 1u),
              "3D module also exports independent Beacon component");
        Runtime world;
        faset::gameplay::registerGameplay(world);
        world.load(scene);
        auto player = world.find("player");
        const auto start = world.transform(player);
        for (int i = 0; i < 15; ++i)
            world.singleStep();
        check(world.grounded(player), "player starts supported by actual physics");
        check(world.transform(world.find("win_marker")).position[1] < -40,
              "victory hidden until completion");
        auto walk = [&](Vec3 target, bool allowJump, float tolerance = .15f) {
            for (int tick = 0; tick < 650; ++tick) {
                const auto p = world.transform(player).position;
                const float dx = target[0] - p[0],
                            dz = FASET_EXAMPLE_DIMENSION == 3 ? target[2] - p[2] : 0;
                if (std::hypot(dx, dz) < tolerance) {
                    for (int i = 0; i < 20; ++i)
                        world.singleStep();
                    return;
                }
                InputState input;
                input.horizontal = std::clamp(dx * 4.0f, -1.0f, 1.0f);
                input.vertical = -std::clamp(dz * 4.0f, -1.0f, 1.0f);
                input.jumpPressed = allowJump && world.grounded(player);
                world.singleStep(input);
            }
            const auto p = world.transform(player).position;
            throw std::runtime_error("Cannot reach waypoint " + std::to_string(target[0]) + "," +
                                     std::to_string(target[2]) + " from " + std::to_string(p[0]) +
                                     "," + std::to_string(p[1]) + "," + std::to_string(p[2]));
        };
        // Drive the published level through input. No teleports are used to collect
        // its tokens or bypass the gate; this exercises level reachability as well
        // as the same C++ module linked into the actual Player.
        const auto first = world.transform(world.find("token_1")).position;
        walk(first, false);
        check(world.transform(world.find("token_1")).position[1] > 1.5f,
              "first token collected through movement");
        const auto middle = world.transform(world.find("token_2")).position;
        walk(middle, true);
        check(world.transform(world.find("token_2")).position[1] > 2,
              "platform token collected with grounded jumps");
        const auto third = world.transform(world.find("token_3")).position;
        walk(third, false);
        check(world.transform(world.find("gate")).position[1] < -20,
              "three pickups open physical gate");
        const auto goal = world.transform(world.find("goal")).position;
        if (FASET_EXAMPLE_DIMENSION == 3)
            walk({4, 0, goal[2]}, false);
        walk(goal, false, .95f);
        check(world.transform(world.find("win_marker")).position[1] > 2,
              "exit completes objective and reveals victory marker");
        world.singleStep({0, 0, false, true});
        check(world.transform(world.find("gate")).position[1] > 0, "E resets gate");
        check(world.transform(world.find("win_marker")).position[1] < -40, "E clears victory");
        check(std::abs(world.transform(player).position[0] - start.position[0]) < .01f,
              "E resets player spawn");
        check(world.transform(world.find("token_1")).position[1] < 1, "E restores pickups");
        const auto previousPlayer = player;
        world.load(scene);
        check(!world.valid(previousPlayer) && world.valid(world.find("player")),
              "restart clears captured state and stale handles");
        check(world.diagnostics().empty(), "playthrough must not hide gameplay exceptions");
        std::cout << "Playable " << FASET_EXAMPLE_DIMENSION
                  << "D project reached its objective and reset\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
