#pragma once
#include <faset/runtime/Runtime.hpp>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& world);
nlohmann::json schema();
} // namespace faset::gameplay
