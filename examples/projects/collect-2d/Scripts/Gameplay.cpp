#include "Gameplay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>

namespace faset::gameplay {
namespace {
constexpr bool is3D = false;
constexpr const char* collectorType = "example.collector_2d";
using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>;
Key key(runtime::EntityHandle handle) {
    return {handle.session, handle.slot, handle.generation};
}
struct Round {
    runtime::Transform spawn, gatePose, victoryPose;
    runtime::EntityHandle gate, goal, victory;
    std::array<runtime::EntityHandle, 3> tokens;
    std::array<runtime::Transform, 3> tokenPoses;
    std::array<bool, 3> collected{};
    bool won{};
    bool gateOpened{};
};
float distance(runtime::Vec3 a, runtime::Vec3 b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = is3D ? a[2] - b[2] : 0;
    return std::sqrt(x * x + y * y + z * z);
}
void reset(runtime::Runtime& game, runtime::EntityHandle self, Round& round) {
    game.teleport(self, round.spawn);
    game.setVelocity(self, {0, 0, 0});
    game.teleport(round.gate, round.gatePose);
    for (std::size_t i = 0; i < 3; ++i)
        game.setTransform(round.tokens[i], round.tokenPoses[i]);
    auto hidden = round.victoryPose;
    hidden.position[1] = -50;
    game.setTransform(round.victory, hidden);
    round.collected = {};
    round.won = false;
    round.gateOpened = false;
    std::cout << "New round: collect the three gold cubes, then reach the green exit. E resets.\n";
}
} // namespace

void registerGameplay(runtime::Runtime& world) {

    auto rounds = std::make_shared<std::map<Key, Round>>();
    runtime::Behavior collector;
    collector.onStart = [rounds](runtime::Runtime& game, runtime::EntityHandle self, double) {
        const auto fields = game.fields(self, collectorType);
        Round round;
        round.spawn = game.transform(self);
        auto resolve = [&](const char* field) {
            const auto handle = game.find(fields.at(field).get<std::string>());
            if (!game.valid(handle))
                throw std::runtime_error(std::string("Missing level reference: ") + field);
            return handle;
        };
        round.gate = resolve("gate");
        round.goal = resolve("goal");
        round.victory = resolve("win_marker");
        round.gatePose = game.transform(round.gate);
        round.victoryPose = game.transform(round.victory);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto field = "token_" + std::to_string(i + 1);
            round.tokens[i] = resolve(field.c_str());
            round.tokenPoses[i] = game.transform(round.tokens[i]);
        }
        auto& stored = rounds->insert_or_assign(key(self), std::move(round)).first->second;
        reset(game, self, stored);
    };
    collector.onDestroy = [rounds](runtime::Runtime&, runtime::EntityHandle self, double) {
        rounds->erase(key(self));
    };
    collector.fixedUpdate = [rounds](runtime::Runtime& game, runtime::EntityHandle self, double) {
        auto& round = rounds->at(key(self));
        const auto input = game.input();
        const auto position = game.transform(self).position;
        if (input.interactPressed || position[1] < -8) {
            reset(game, self, round);
            return;
        }
        const auto fields = game.fields(self, collectorType);
        auto velocity = game.velocity(self);
        float x = input.horizontal, z = is3D ? -input.vertical : 0;
        const float length = std::sqrt(x * x + z * z);
        if (length > 1) {
            x /= length;
            z /= length;
        }
        const auto speed = fields.value("speed", 4.0f);
        velocity[0] = round.won ? 0 : x * speed;
        if (is3D)
            velocity[2] = round.won ? 0 : z * speed;
        if (!round.won && input.jumpPressed && game.grounded(self))
            velocity[1] = fields.value("jump_speed", 6.0f);
        game.setVelocity(self, velocity);
        if (round.won)
            return;
        for (std::size_t i = 0; i < 3; ++i)
            if (!round.collected[i] && distance(position, round.tokenPoses[i].position) < 0.95f) {
                round.collected[i] = true;
                auto display = round.tokenPoses[i];
                display.position = is3D ? runtime::Vec3{6, 2 + float(i) * 0.8f, -4}
                                        : runtime::Vec3{-1.2f + float(i) * 1.2f, 4.5f, 0};
                game.setTransform(round.tokens[i], display);
                std::cout << "Collected "
                          << std::count(round.collected.begin(), round.collected.end(), true)
                          << "/3\n";
            }
        if (std::all_of(round.collected.begin(), round.collected.end(),
                        [](bool value) { return value; })) {
            if (!round.gateOpened) {
                auto open = round.gatePose;
                open.position[1] = -30;
                game.teleport(round.gate, open);
                round.gateOpened = true;
            }
            if (distance(position, game.transform(round.goal).position) < 1.0f) {
                round.won = true;
                game.setTransform(round.victory, round.victoryPose);
                std::cout << "Level complete! The gold victory marker is visible. Press E to play "
                             "again.\n";
            }
        }
    };
    world.registerBehavior(collectorType, std::move(collector));
}

nlohmann::json schema() {
    nlohmann::json fields = {{"speed",
                              {{"id", "speed"},
                               {"name", "Move speed"},
                               {"type", "number"},
                               {"default", 4.0},
                               {"min", 0.0},
                               {"units", "m/s"}}},
                             {"jump_speed",
                              {{"id", "jump_speed"},
                               {"name", "Jump speed"},
                               {"type", "number"},
                               {"default", 6.0},
                               {"min", 0.0},
                               {"units", "m/s"}}}};
    for (const auto* id : {"gate", "goal", "win_marker", "token_1", "token_2", "token_3"})
        fields[id] = {{"id", id}, {"name", id}, {"type", "entity_ref"}, {"default", id}};
    auto types = nlohmann::json::array({{{"id", collectorType},
                                         {"name", "Collect three and escape"},
                                         {"version", 1},
                                         {"fields", std::move(fields)}}});

    return types;
}
} // namespace faset::gameplay
