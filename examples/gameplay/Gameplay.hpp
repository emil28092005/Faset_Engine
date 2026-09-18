#pragma once
#include <faset/runtime/Runtime.hpp>

namespace faset::gameplay {
void registerGameplay(runtime::Runtime& runtime);
nlohmann::json schema();
}
