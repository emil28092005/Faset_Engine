#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace faset::assets {
using Json = nlohmann::json;
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
    std::array<float, 16> local_transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool stable_source_id = false;
};
struct Material {
    std::string id, name;
    std::array<float, 4> base_color{1, 1, 1, 1};
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

// Read-only cooked storage. No source parser, import jobs or publication operations.
class AssetStore {
  public:
    explicit AssetStore(std::filesystem::path cache_root);
    Json current_manifest(const std::string& asset_id) const;
    CookedAsset load_asset(const std::string& asset_id) const;
    std::filesystem::path generation_directory(const std::string& asset_id) const;
    const std::filesystem::path& cache_root() const noexcept {
        return cache_root_;
    }

  protected:
    std::filesystem::path cache_root_;
};
} // namespace faset::assets
