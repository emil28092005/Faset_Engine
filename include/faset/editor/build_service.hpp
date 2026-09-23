#pragma once
#include <faset/core/json.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace faset::editor {
struct BuildConfig {
    std::filesystem::path project_root;
    std::filesystem::path engine_root;
    std::filesystem::path build_directory;
    std::filesystem::path cache_root;
    // Separate CMake caches prevent an export from changing the active development build.
    std::string configuration{"Debug"};
    std::string export_configuration{"Release"};
    std::string cmake{"cmake"};
    std::string generator{"Ninja"};
    std::vector<std::string> configure_arguments;
};
struct JobStatus {
    std::string id, kind, state{"queued"}, stage{"queued"};
    double progress{};
    std::string log, error;
    Json diagnostics = Json::array();
    Json result = Json::object();
    bool finished() const {
        return state == "succeeded" || state == "failed" || state == "cancelled";
    }
    Json json() const;
};
// One serialized build/cook worker per project. GUI and MCP use the same service.
// Input scenes must be resolved authoring snapshots, independent of live runtime state.
class BuildService {
  public:
    explicit BuildService(BuildConfig);
    ~BuildService();
    BuildService(const BuildService&) = delete;
    BuildService& operator=(const BuildService&) = delete;
    void scaffold(const std::string& name, int dimension);
    std::string start_build();
    std::string start_cook(Json resolved_scene);
    // Publishes output/generations/<id>; current.json changes only after all validation succeeds.
    std::string start_export(Json resolved_scene, const std::filesystem::path& output_directory);
    JobStatus job(const std::string& id) const;
    std::vector<JobStatus> jobs() const;
    void cancel(const std::string& id);
    JobStatus wait(const std::string& id);
    const BuildConfig& config() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void write_cooked_scene(const std::filesystem::path& path, const Json& resolved_scene);
} // namespace faset::editor
