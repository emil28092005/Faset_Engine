#pragma once

#include <faset/editor/build_service.hpp>
#include <faset/scripting/project.hpp>
#include <filesystem>
#include <optional>
#include <string>

namespace faset::editor {

struct BuildInputs {
    std::string source_hash;
    std::string recipe_hash;
    std::string toolchain_hash;
    std::string fingerprint() const;
};

// Hash all files in Scripts and the active Lua declaration, not only the two
// conventional gameplay files. Both Editor stale-state and builds use this key.
std::string gameplay_source_hash(const std::filesystem::path& project_root,
                                 const scripting::LuaProject& lua);
BuildInputs capture_build_inputs(const BuildConfig& config, const scripting::LuaProject& lua);
std::optional<std::string> configured_cmake_value(const BuildConfig& config,
                                                   std::string_view key);

// Copy a content-addressed Scripts tree for the native compiler. BuildService
// rehashes it at phase boundaries; returning the same path preserves Ninja incrementality.
std::filesystem::path stage_gameplay_sources(const BuildConfig& config,
                                              const BuildInputs& inputs,
                                              const scripting::LuaProject& lua,
                                              std::string_view job_id);

// A compiler replaced at the same path may be invisible to Ninja. Invalidate
// only this generated native tree; published build generations stay intact.
void ensure_native_toolchain_stamp(const BuildConfig& config,
                                   const std::filesystem::path& native_directory,
                                   const BuildInputs& inputs);

// Native build completes before package reuse is considered. Hash the actual
// generated outputs, not their timestamps or just the gameplay sources.
std::string build_package_key(const BuildInputs& inputs,
                              const std::filesystem::path& native_directory,
                              const std::string& configuration,
                              const std::filesystem::path& player,
                              const std::filesystem::path& exporter);

// A hit is accepted only if the complete published file set and schema match
// the manifest. Malformed or old manifests are safe cache misses.
bool validate_build_generation(const std::filesystem::path& directory,
                               const std::string& package_key);

} // namespace faset::editor
