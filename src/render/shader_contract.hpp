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
// Direct graphics plus the independent 16x16 light-tile compute entry.
std::array<ShaderCode, 4> load_shader_bundle(const std::filesystem::path& directory);
// Order: opaque vertex, optional instanced shadow vertex, main cull, HZB, post cull.
std::array<ShaderCode, 5> load_gpu_shader_bundle(const std::filesystem::path& directory);
// Order: temporal resolve compute, full-screen composite vertex and fragment.
std::array<ShaderCode, 3> load_temporal_shader_bundle(const std::filesystem::path& directory);
// Direct temporal vertex/fragment and GPU temporal vertex; set 0 material,
// set 1 lighting and set 2 GPU scene stay at their established bindings.
std::array<ShaderCode, 3> load_temporal_scene_shader_bundle(const std::filesystem::path& directory);
} // namespace faset::render::detail
