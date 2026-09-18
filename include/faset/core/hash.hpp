#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace faset {
std::string sha256(std::span<const std::byte> bytes);
inline std::string sha256(std::string_view text) {
    return sha256(std::as_bytes(std::span(text.data(), text.size())));
}
std::string sha256_file(const std::filesystem::path& path);
} // namespace faset
