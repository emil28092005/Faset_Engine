#pragma once
#include <faset/assets/asset_data.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace faset::assets {
using Json = nlohmann::json;
inline constexpr const char* importer_version = "faset-gltf-1/cgltf-1.15";

struct ImportProgress {
    float fraction = 0;
    std::string stage;
};
class ImportJob {
  public:
    using Observer = std::function<void(const ImportProgress&)>;
    explicit ImportJob(Observer observer = {});
    void cancel() noexcept;
    bool cancelled() const noexcept;
    ImportProgress progress() const;
    void report(float fraction, std::string stage);

  private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mutex_;
    ImportProgress progress_;
    Observer observer_;
};

enum class ImportStatus { succeeded, failed, cancelled, conflict };
struct ImportRequest {
    std::filesystem::path source;
    std::string asset_id{};  // Empty: restore/create source.faset-import.json identity.
    Json settings = nullptr; // Null restores the sidecar recipe; an object replaces it.
    // Explicit conflict resolution; false keeps the previous generation active.
    bool allow_removed_outputs = false;
    // Optional review guards: reject a changed candidate or active generation.
    // Empty keeps the ordinary import API; the Editor sets both on explicit acceptance.
    std::string expected_generation{}, expected_active_generation{};
};
struct ImportResult {
    ImportStatus status = ImportStatus::failed;
    std::string asset_id, generation, previous_generation;
    std::vector<std::string> diagnostics;
    std::vector<std::string> removed_output_ids;
    Json manifest;
    bool cache_hit = false;
    bool ok() const noexcept {
        return status == ImportStatus::succeeded;
    }
};

// A pipeline is an authoring service. Player only needs read-only cooked data.
// Writers in one process serialize publication; a cache root has one service owner.
class AssetPipeline : public AssetStore {
  public:
    explicit AssetPipeline(std::filesystem::path cache_root);
    ImportResult import_asset(const ImportRequest& request, ImportJob& job);
    ImportResult import_asset(const ImportRequest& request);
    // Overrides are authoring data beside the source, never generated cache contents.
    Json overrides(const std::string& asset_id) const;
    void set_overrides(const std::string& asset_id, const Json& overrides);
};
} // namespace faset::assets
