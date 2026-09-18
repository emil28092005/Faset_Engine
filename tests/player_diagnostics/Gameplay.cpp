#include "Gameplay.hpp"
#include <stdexcept>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& runtime) {
    runtime::Behavior behavior;
    behavior.onStart = [](auto&, auto, double) { throw std::runtime_error("TEST_ON_START"); };
    behavior.update = [](auto&, auto, double) { throw std::runtime_error("TEST_UPDATE"); };
    behavior.onDestroy = [](auto&, auto, double) { throw std::runtime_error("TEST_ON_DESTROY"); };
    runtime.registerBehavior("test.diagnostics", std::move(behavior));
}
nlohmann::json schema() {
    return nlohmann::json::array(
        {{{"id", "test.diagnostics"}, {"version", 2}, {"fields", nlohmann::json::object()}}});
}
} // namespace faset::gameplay
