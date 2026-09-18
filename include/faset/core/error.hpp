#pragma once
#include <faset/core/json.hpp>
#include <stdexcept>
#include <string>

namespace faset {
class Error : public std::runtime_error {
  public:
    Error(std::string code, std::string message, Json details = Json::object())
        : std::runtime_error(std::move(message)), code_(std::move(code)),
          details_(std::move(details)) {}
    const std::string& code() const noexcept {
        return code_;
    }
    Json json() const {
        return {{"code", code_}, {"message", what()}, {"details", details_}};
    }

  private:
    std::string code_;
    Json details_;
};
inline void require(bool condition, const std::string& code, const std::string& message) {
    if (!condition)
        throw Error(code, message);
}
} // namespace faset
