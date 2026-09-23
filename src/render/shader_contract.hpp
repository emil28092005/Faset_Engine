#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace faset::render::detail {
struct ShaderCode {
    std::vector<std::uint32_t> words;
    std::string layout_fingerprint;
};
std::array<ShaderCode, 3> load_shader_bundle(const std::filesystem::path& directory);
// Order: opaque vertex, optional instanced shadow vertex, main cull, HZB, post cull.
std::array<ShaderCode, 5> load_gpu_shader_bundle(const std::filesystem::path& directory);
} // namespace faset::render::detail
