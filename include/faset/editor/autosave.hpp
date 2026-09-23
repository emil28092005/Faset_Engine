#pragma once

#include <faset/core/json.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace faset::editor {

class AutosaveController {
  public:
    using Clock = std::chrono::steady_clock;
    using SaveFn = std::function<Json(const std::string&, std::uint64_t)>;
    explicit AutosaveController(SaveFn save);

    // Called from the Editor event loop with an injected monotonic clock for
    // deterministic tests. Only a named dirty scene becomes eligible to save.
    void observe(const Json& documents, Clock::time_point now, bool enabled);
    Json status() const;

  private:
    struct Entry {
        std::uint64_t revision{};
        std::string path, state = "saved", error;
        Clock::time_point deadline{};
    };
    SaveFn save_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    bool enabled_ = true;
};

} // namespace faset::editor
