#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <faset/assets/asset_data.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <fstream>
#include <set>
#include <stdexcept>

namespace faset::assets {
namespace fs = std::filesystem;
namespace {
void valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        }))
        throw std::runtime_error("Invalid AssetId");
}
std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream file(faset::native_io_path(path), std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot read cooked file: " + faset::path_to_utf8(path));
    auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > 1024ull * 1024 * 1024)
        throw std::runtime_error("Cooked file exceeds 1 GiB limit");
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Truncated cooked file");
    return bytes;
}
Json read_json(const fs::path& path) {
    auto bytes = read_bytes(path);
    if (bytes.empty())
        throw std::runtime_error("Empty cooked JSON manifest");
    return Json::parse(reinterpret_cast<const char*>(bytes.data()),
                       reinterpret_cast<const char*>(bytes.data() + bytes.size()));
}
fs::path cooked_path(const fs::path& directory, const std::string& name) {
    if (name.empty())
        throw std::runtime_error("Empty cooked path");
    return faset::project_path(directory, faset::path_from_utf8(name));
}
void validate_generation(const fs::path& directory, const Json& manifest) {
    if (manifest.at("schema_version") != 1)
        throw std::runtime_error("Unsupported asset manifest version");
    std::set<std::string> files;
    for (const auto& file : manifest.at("files")) {
        auto name = file.at("path").get<std::string>();
        if (!files.insert(name).second)
            throw std::runtime_error("Duplicate cooked file path");
        auto bytes = read_bytes(cooked_path(directory, name));
        if (bytes.size() != file.at("size").get<std::size_t>() ||
            faset::sha256(std::span<const std::byte>(bytes)) !=
                file.at("sha256").get<std::string>())
            throw std::runtime_error("Corrupt cooked file: " + name);
    }
    for (const auto& mesh : manifest.at("meshes"))
        for (const auto& primitive : mesh.at("primitives"))
            if (!files.contains(primitive.at("path").get<std::string>()))
                throw std::runtime_error("Mesh payload is missing from its manifest");
    for (const auto& texture : manifest.at("textures"))
        if (!files.contains(texture.at("path").get<std::string>()))
            throw std::runtime_error("Texture payload is missing from its manifest");
}
struct BinaryReader {
    const std::vector<std::byte>& bytes;
    std::size_t cursor = 0;
    std::uint32_t u32() {
        if (bytes.size() - cursor < 4)
            throw std::runtime_error("Truncated cooked mesh");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= std::to_integer<std::uint32_t>(bytes[cursor++]) << (8 * i);
        return v;
    }
    float number() {
        auto v = std::bit_cast<float>(u32());
        if (!std::isfinite(v))
            throw std::runtime_error("Invalid cooked float");
        return v;
    }
};
Primitive decode_primitive(const std::vector<std::byte>& bytes, int material) {
    BinaryReader in{bytes};
    if (in.u32() != 0x48534d46 || in.u32() != 1)
        throw std::runtime_error("Unsupported cooked mesh format");
    const auto nv = in.u32(), ni = in.u32();
    if (static_cast<std::uint64_t>(nv) * 32 + static_cast<std::uint64_t>(ni) * 4 + 16 !=
        bytes.size())
        throw std::runtime_error("Invalid cooked mesh size");
    Primitive p;
    p.material = material;
    p.vertices.resize(nv);
    p.indices.resize(ni);
    for (auto& v : p.vertices) {
        for (auto& x : v.position)
            x = in.number();
        for (auto& x : v.normal)
            x = in.number();
        for (auto& x : v.uv)
            x = in.number();
    }
    for (auto& i : p.indices) {
        i = in.u32();
        if (i >= nv)
            throw std::runtime_error("Cooked mesh index out of range");
    }
    return p;
}
} // namespace
AssetStore::AssetStore(fs::path root)
    : cache_root_(fs::absolute(std::move(root)).lexically_normal()) {}
fs::path AssetStore::generation_directory(const std::string& id) const {
    valid_id(id);
    const auto root = faset::project_path(cache_root_, fs::path("assets") / id);
    const auto pointer = read_json(root / "current.json");
    const auto generation = pointer.at("generation").get<std::string>();
    valid_id(generation);
    return faset::project_path(root, fs::path("generations") / generation);
}
Json AssetStore::current_manifest(const std::string& id) const {
    valid_id(id);
    const auto root = faset::project_path(cache_root_, fs::path("assets") / id);
    const auto pointer = read_json(root / "current.json");
    const auto generation = pointer.at("generation").get<std::string>();
    valid_id(generation);
    auto manifest = read_json(
        faset::project_path(root, fs::path("generations") / generation / "manifest.json"));
    if (manifest.at("asset_id").get<std::string>() != id ||
        manifest.at("generation").get<std::string>() != generation)
        throw std::runtime_error("Cooked manifest identity does not match its generation");
    // One pointer snapshot prevents mixing two concurrently published generations.
    manifest["source"] = pointer.at("source");
    return manifest;
}
CookedAsset AssetStore::load_asset(const std::string& id) const {
    const auto directory = generation_directory(id);
    const auto m = read_json(directory / "manifest.json");
    validate_generation(directory, m);
    if (m.at("asset_id").get<std::string>() != id ||
        m.at("generation").get<std::string>() != faset::path_to_utf8(directory.filename()))
        throw std::runtime_error("Cooked asset identity does not match its generation");
    CookedAsset asset;
    asset.asset_id = m.at("asset_id");
    asset.generation = m.at("generation");
    for (const auto& n : m.at("nodes")) {
        Node node;
        node.id = n.at("id");
        node.name = n.at("name");
        node.parent_id = n.at("parent_id");
        node.mesh = n.at("mesh");
        node.local_transform = n.at("local_transform").get<std::array<float, 16>>();
        node.stable_source_id = n.at("stable_source_id");
        asset.nodes.push_back(std::move(node));
    }
    for (const auto& j : m.at("meshes")) {
        Mesh mesh;
        mesh.id = j.at("id");
        mesh.name = j.at("name");
        for (const auto& primitive : j.at("primitives"))
            mesh.primitives.push_back(decode_primitive(
                read_bytes(cooked_path(directory, primitive.at("path").get<std::string>())),
                primitive.at("material")));
        asset.meshes.push_back(std::move(mesh));
    }
    for (const auto& j : m.at("materials")) {
        // Early development manifests embedded v1 materials without a tag.
        // Preserve readability, but never interpret an explicitly unknown version.
        if (j.value("format", "faset.material") != "faset.material" || j.value("version", 1) != 1)
            throw std::runtime_error("Unsupported cooked material format or version");
        Material material;
        material.id = j.at("id");
        material.name = j.at("name");
        material.base_color = j.at("base_color").get<std::array<float, 4>>();
        material.emissive = j.at("emissive").get<std::array<float, 3>>();
        material.metallic = j.at("metallic");
        material.roughness = j.at("roughness");
        material.alpha_mode = j.at("alpha_mode");
        material.alpha_cutoff = j.at("alpha_cutoff");
        material.double_sided = j.at("double_sided");
        material.unlit = j.at("unlit");
        material.base_color_texture = j.at("base_color_texture");
        material.metallic_roughness_texture = j.at("metallic_roughness_texture");
        material.normal_texture = j.at("normal_texture");
        material.occlusion_texture = j.at("occlusion_texture");
        material.emissive_texture = j.at("emissive_texture");
        asset.materials.push_back(std::move(material));
    }
    for (const auto& j : m.at("textures")) {
        Texture texture;
        texture.id = j.at("id");
        texture.name = j.at("name");
        texture.mime_type = j.at("mime_type");
        texture.bytes = read_bytes(cooked_path(directory, j.at("path").get<std::string>()));
        texture.wrap_s = j.at("wrap_s");
        texture.wrap_t = j.at("wrap_t");
        texture.min_filter = j.at("min_filter");
        texture.mag_filter = j.at("mag_filter");
        asset.textures.push_back(std::move(texture));
    }
    return asset;
}
} // namespace faset::assets
