#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace faset::assets {
using Json = nlohmann::json;
inline constexpr const char* importer_version = "faset-gltf-1/cgltf-1.15";

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{0, 0, 1};
    std::array<float, 2> uv{};
};
struct Primitive {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    int material = -1;
};
struct Mesh {
    std::string id, name;
    std::vector<Primitive> primitives;
};
struct Node {
    std::string id, name, parent_id;
    int mesh = -1;
    // glTF right-handed, Y-up, metres; column-major matrix.
    std::array<float, 16> local_transform{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool stable_source_id = false;
};
struct Material {
    std::string id, name;
    std::array<float, 4> base_color{1,1,1,1};
    std::array<float, 3> emissive{};
    float metallic = 1, roughness = 1, alpha_cutoff = 0.5f;
    std::string alpha_mode = "OPAQUE";
    bool double_sided = false, unlit = false;
    int base_color_texture = -1, metallic_roughness_texture = -1;
    int normal_texture = -1, occlusion_texture = -1, emissive_texture = -1;
};
struct Texture {
    std::string id, name, mime_type;
    // Encoded image bytes, owned by this value. Renderer selects an image decoder.
    std::vector<std::byte> bytes;
    int wrap_s = 10497, wrap_t = 10497, min_filter = 0, mag_filter = 0;
};
struct CookedAsset {
    std::string asset_id, generation;
    std::vector<Mesh> meshes;
    std::vector<Node> nodes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
};

struct ImportProgress { float fraction = 0; std::string stage; };
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
    std::string asset_id{}; // Empty: restore/create source.faset-import.json identity.
    Json settings = nullptr; // Null restores the sidecar recipe; an object replaces it.
    // Explicit conflict resolution; false keeps the previous generation active.
    bool allow_removed_outputs = false;
};
struct ImportResult {
    ImportStatus status = ImportStatus::failed;
    std::string asset_id, generation;
    std::vector<std::string> diagnostics;
    std::vector<std::string> removed_output_ids;
    Json manifest;
    bool cache_hit = false;
    bool ok() const noexcept { return status == ImportStatus::succeeded; }
};

// A pipeline is an authoring service. Player only needs read-only cooked data.
// Writers in one process serialize publication; a cache root has one service owner.
class AssetPipeline {
public:
    explicit AssetPipeline(std::filesystem::path cache_root);
    ImportResult import_asset(const ImportRequest& request, ImportJob& job);
    ImportResult import_asset(const ImportRequest& request);
    Json current_manifest(const std::string& asset_id) const;
    CookedAsset load_asset(const std::string& asset_id) const;
    // Overrides are authoring data beside the source, never generated cache contents.
    Json overrides(const std::string& asset_id) const;
    void set_overrides(const std::string& asset_id, const Json& overrides);
    std::filesystem::path generation_directory(const std::string& asset_id) const;
    const std::filesystem::path& cache_root() const noexcept { return cache_root_; }
private:
    std::filesystem::path cache_root_;
};
} // namespace faset::assets
