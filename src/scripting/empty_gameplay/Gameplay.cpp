#include "Gameplay.hpp"

namespace faset::gameplay {
void registerGameplay(runtime::Runtime&) {}
nlohmann::json schema() {
    return nlohmann::json::array();
}
} // namespace faset::gameplay
