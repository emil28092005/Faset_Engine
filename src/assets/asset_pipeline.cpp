#include <cgltf.h>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
// Import validation owns a private decoder; Player's decoder remains a separate
// binary boundary.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace faset::assets {
namespace {
namespace fs = std::filesystem;
std::mutex writer_mutex;
struct Cancelled {};
void checkpoint(ImportJob& job, float fraction, const std::string& stage) {
    job.report(fraction, stage);
    if (job.cancelled())
        throw Cancelled{};
}
std::string uuid() {
    std::random_device random;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i)
        out << std::hex << std::setw(8) << std::setfill('0') << random();
    return out.str();
}
void valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        }))
        throw std::runtime_error("Invalid AssetId");
}
std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream file(faset::native_io_path(path), std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot read: " + faset::path_to_utf8(path));
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > 1024ull * 1024 * 1024)
        throw std::runtime_error("Input exceeds 1 GiB limit: " + faset::path_to_utf8(path));
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!data.empty() &&
        !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Short read: " + faset::path_to_utf8(path));
    return data;
}
void write_bytes(const fs::path& path, const std::vector<std::byte>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(faset::native_io_path(path), std::ios::binary | std::ios::trunc);
    if (!out || (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()),
                                             static_cast<std::streamsize>(data.size()))))
        throw std::runtime_error("Cannot write: " + faset::path_to_utf8(path));
    out.close();
    if (!out)
        throw std::runtime_error("Cannot close: " + faset::path_to_utf8(path));
}
Json read_json(const fs::path& path) {
    std::ifstream in(faset::native_io_path(path));
    if (!in)
        throw std::runtime_error("Cannot read JSON: " + faset::path_to_utf8(path));
    return Json::parse(in);
}
void write_json(const fs::path& path, const Json& value) {
    const auto text = value.dump(2) + "\n";
    write_bytes(path, std::vector<std::byte>(
                          reinterpret_cast<const std::byte*>(text.data()),
                          reinterpret_cast<const std::byte*>(text.data() + text.size())));
}
void atomic_json(const fs::path& path, const Json& value) {
    fs::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp-" + uuid();
    try {
        write_json(temporary, value);
#ifdef _WIN32
        if (!MoveFileExW(faset::native_io_path(temporary).c_str(),
                         faset::native_io_path(path).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Atomic replace failed: " + faset::path_to_utf8(path));
#else
        fs::rename(temporary, path);
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(temporary, ec);
        throw;
    }
}
std::string hash_bytes(const std::vector<std::byte>& bytes) {
    return faset::sha256(std::span<const std::byte>(bytes));
}
std::string stable_id(const std::string& kind, const std::string& key) {
    return kind + "-" + faset::sha256(kind + ":" + key).substr(0, 32);
}
std::string safe_name(const char* name) {
    return name ? name : "";
}
std::string source_id(const cgltf_extras& extras) {
    if (!extras.data)
        return {};
    auto value = Json::parse(extras.data, nullptr, false);
    if (value.is_object() && value.contains("faset_id") && value["faset_id"].is_string())
        return value["faset_id"].get<std::string>();
    return {};
}
std::string uri_decode(std::string value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = value.substr(i + 1, 2);
            std::size_t count = 0;
            const int c = std::stoi(hex, &count, 16);
            if (count != 2 || c == 0)
                throw std::runtime_error("Invalid URI escape");
            result += static_cast<char>(c);
            i += 2;
        } else
            result += value[i];
    }
    return result;
}
std::vector<std::byte> decode_data_uri(const std::string& uri) {
    const auto comma = uri.find(',');
    if (comma == std::string::npos || uri.substr(0, comma).find(";base64") == std::string::npos)
        throw std::runtime_error("Only base64 data URIs are supported");
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::byte> data;
    std::uint32_t bits = 0;
    int count = 0;
    for (std::size_t i = comma + 1; i < uri.size(); ++i) {
        if (uri[i] == '=')
            break;
        const auto v = alphabet.find(uri[i]);
        if (v == std::string_view::npos)
            throw std::runtime_error("Invalid base64 image");
        bits = (bits << 6) | static_cast<unsigned>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            data.push_back(static_cast<std::byte>((bits >> count) & 255));
        }
    }
    return data;
}
fs::path external_path(const fs::path& source, const std::string& uri) {
    if (uri.find("://") != std::string::npos)
        throw std::runtime_error("Network URI is not an import dependency: " + uri);
    const fs::path relative = faset::path_from_utf8(uri_decode(uri));
    if (relative.is_absolute())
        throw std::runtime_error("glTF URI must be relative");
    return (source.parent_path() / relative).lexically_normal();
}
struct Dependency {
    fs::path path;
    std::string digest;
    std::vector<std::byte> bytes;
};
using Dependencies = std::map<std::string, Dependency>;
std::vector<std::byte> dependency_bytes(const fs::path& source, const std::string& uri,
                                        Dependencies& dependencies) {
    if (auto found = dependencies.find(uri); found != dependencies.end())
        return found->second.bytes;
    auto path = external_path(source, uri);
    auto bytes = read_bytes(path);
    dependencies[uri] = {path, hash_bytes(bytes), bytes};
    return bytes;
}
std::string image_mime(const cgltf_image& image, const std::vector<std::byte>& bytes) {
    if (image.mime_type)
        return image.mime_type;
    if (bytes.size() >= 4 && bytes[0] == std::byte{0x89} && bytes[1] == std::byte{'P'})
        return "image/png";
    if (bytes.size() >= 2 && bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xd8})
        return "image/jpeg";
    return "application/octet-stream";
}
std::vector<std::byte> image_bytes(const cgltf_image& image, const fs::path& source,
                                   Dependencies& dependencies) {
    if (image.uri) {
        const std::string uri = image.uri;
        return uri.starts_with("data:") ? decode_data_uri(uri)
                                        : dependency_bytes(source, uri, dependencies);
    }
    if (image.buffer_view && image.buffer_view->buffer && image.buffer_view->buffer->data) {
        const auto& view = *image.buffer_view;
        if (view.offset > view.buffer->size || view.size > view.buffer->size - view.offset)
            throw std::runtime_error("Image buffer view out of bounds");
        const auto* begin = static_cast<const std::byte*>(view.buffer->data) + view.offset;
        return {begin, begin + view.size};
    }
    throw std::runtime_error("Texture has no supported image payload");
}
void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 255));
}
void put_float(std::vector<std::byte>& out, float v) {
    if (!std::isfinite(v))
        throw std::runtime_error("Non-finite mesh value");
    put_u32(out, std::bit_cast<std::uint32_t>(v));
}
std::vector<std::byte> encode_primitive(const Primitive& p) {
    std::vector<std::byte> out;
    put_u32(out, 0x48534d46);
    put_u32(out, 1);
    put_u32(out, static_cast<std::uint32_t>(p.vertices.size()));
    put_u32(out, static_cast<std::uint32_t>(p.indices.size()));
    for (const auto& v : p.vertices) {
        for (auto f : v.position)
            put_float(out, f);
        for (auto f : v.normal)
            put_float(out, f);
        for (auto f : v.uv)
            put_float(out, f);
    }
    for (auto i : p.indices)
        put_u32(out, i);
    return out;
}
std::vector<float> unpack(const cgltf_accessor* accessor, std::size_t elements) {
    if (!accessor || cgltf_num_components(accessor->type) != elements)
        throw std::runtime_error("Unexpected vertex attribute type");
    if (accessor->count > 10000000)
        throw std::runtime_error("Mesh exceeds vertex limit");
    std::vector<float> values(accessor->count * elements);
    if (cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) != values.size())
        throw std::runtime_error("Cannot unpack vertex attribute");
    if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }))
        throw std::runtime_error("Non-finite vertex attribute");
    return values;
}
void calculate_normals(Primitive& primitive) {
    for (auto& v : primitive.vertices)
        v.normal = {0, 0, 0};
    for (std::size_t i = 0; i < primitive.indices.size(); i += 3) {
        auto& a = primitive.vertices[primitive.indices[i]];
        auto& b = primitive.vertices[primitive.indices[i + 1]];
        auto& c = primitive.vertices[primitive.indices[i + 2]];
        std::array<float, 3> u{}, v{}, n{};
        for (int j = 0; j < 3; ++j) {
            u[j] = b.position[j] - a.position[j];
            v[j] = c.position[j] - a.position[j];
        }
        n = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (auto* vertex : {&a, &b, &c})
            for (int j = 0; j < 3; ++j)
                vertex->normal[j] += n[j];
    }
    for (auto& v : primitive.vertices) {
        const auto length =
            std::sqrt(std::inner_product(v.normal.begin(), v.normal.end(), v.normal.begin(), 0.f));
        if (length > 1e-12f)
            for (auto& n : v.normal)
                n /= length;
        else
            v.normal = {0, 0, 1};
    }
}
int texture_index(const cgltf_texture_view& view, const cgltf_data& data) {
    if (!view.texture)
        return -1;
    if (view.texcoord != 0 || view.has_transform)
        throw std::runtime_error("Only TEXCOORD_0 without texture transform is "
                                 "supported by this import profile");
    return static_cast<int>(view.texture - data.textures);
}
void add_file(Json& manifest, const fs::path& stage, const std::string& path,
              const std::vector<std::byte>& bytes) {
    write_bytes(stage / path, bytes);
    manifest["files"].push_back(
        {{"path", path}, {"sha256", hash_bytes(bytes)}, {"size", bytes.size()}});
}
void validate_generation(const fs::path& directory, const Json& manifest) {
    if (manifest.at("schema_version") != 1)
        throw std::runtime_error("Unsupported asset manifest version");
    for (const auto& file : manifest.at("files")) {
        const fs::path relative = faset::path_from_utf8(file.at("path").get<std::string>());
        if (relative.is_absolute() ||
            faset::generic_path_to_utf8(relative).find("..") != std::string::npos)
            throw std::runtime_error("Invalid cooked file path");
        auto bytes = read_bytes(directory / relative);
        if (bytes.size() != file.at("size").get<std::size_t>() ||
            hash_bytes(bytes) != file.at("sha256").get<std::string>())
            throw std::runtime_error("Corrupt cooked file: " +
                                     faset::generic_path_to_utf8(relative));
    }
}
Json material_json(const Material& m) {
    return {{"format", "faset.material"},
            {"version", 1},
            {"id", m.id},
            {"name", m.name},
            {"base_color", m.base_color},
            {"metallic", m.metallic},
            {"roughness", m.roughness},
            {"emissive", m.emissive},
            {"alpha_mode", m.alpha_mode},
            {"alpha_cutoff", m.alpha_cutoff},
            {"double_sided", m.double_sided},
            {"unlit", m.unlit},
            {"base_color_texture", m.base_color_texture},
            {"metallic_roughness_texture", m.metallic_roughness_texture},
            {"normal_texture", m.normal_texture},
            {"occlusion_texture", m.occlusion_texture},
            {"emissive_texture", m.emissive_texture}};
}
} // namespace

ImportJob::ImportJob(Observer observer) : observer_(std::move(observer)) {}
void ImportJob::cancel() noexcept {
    cancelled_.store(true);
}
bool ImportJob::cancelled() const noexcept {
    return cancelled_.load();
}
ImportProgress ImportJob::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}
void ImportJob::report(float fraction, std::string stage) {
    ImportProgress progress{fraction, std::move(stage)};
    {
        std::lock_guard lock(mutex_);
        progress_ = progress;
    }
    if (observer_) {
        try {
            observer_(progress);
        } catch (...) { /* Observers cannot roll back a published result. */
        }
    }
}
AssetPipeline::AssetPipeline(fs::path root) : AssetStore(std::move(root)) {}
ImportResult AssetPipeline::import_asset(const ImportRequest& request) {
    ImportJob job;
    return import_asset(request, job);
}
ImportResult AssetPipeline::import_asset(const ImportRequest& request, ImportJob& job) {
    std::lock_guard writer(writer_mutex);
    ImportResult result;
    fs::path stage;
    try {
        checkpoint(job, 0, "reading source");
        const auto logical_source = fs::absolute(request.source).lexically_normal();
        auto source = logical_source;
        const auto logical_bytes = read_bytes(logical_source);
        const auto logical_hash = hash_bytes(logical_bytes);
        std::string bundle_asset_id;
        std::vector<std::byte> payload_snapshot;
        if (logical_source.extension() == ".json") {
            auto bundle = Json::parse(
                reinterpret_cast<const char*>(logical_bytes.data()),
                reinterpret_cast<const char*>(logical_bytes.data() + logical_bytes.size()));
            if (bundle.at("schema_version") != 1 || bundle.at("files").empty())
                throw std::runtime_error("Invalid bundle manifest");
            bundle_asset_id = bundle.at("asset_id").get<std::string>();
            valid_id(bundle_asset_id);
            bool found_payload = false;
            for (const auto& file : bundle.at("files")) {
                const auto relative = faset::path_from_utf8(file.at("path").get<std::string>());
                if (relative.is_absolute() ||
                    faset::generic_path_to_utf8(relative).find("..") != std::string::npos)
                    throw std::runtime_error("Invalid bundle payload path");
                const auto candidate = logical_source.parent_path() / relative;
                const auto bytes = read_bytes(candidate);
                if (hash_bytes(bytes) != file.at("sha256").get<std::string>())
                    throw std::runtime_error("Bundle payload digest mismatch");
                if (file.contains("size") && file.at("size").get<std::size_t>() != bytes.size())
                    throw std::runtime_error("Bundle payload size mismatch");
                if (!found_payload && relative.extension() == ".glb") {
                    source = candidate;
                    payload_snapshot = bytes;
                    found_payload = true;
                }
            }
            if (!found_payload)
                throw std::runtime_error("Bundle has no GLB payload");
        }
        const auto source_bytes = source == logical_source ? logical_bytes : payload_snapshot;
        const auto source_hash = hash_bytes(source_bytes);
        auto sidecar = logical_source;
        sidecar += ".faset-import.json";
        Json metadata = fs::exists(sidecar) ? read_json(sidecar) : Json::object();
        if (!metadata.is_object() ||
            (!metadata.empty() && metadata.value("schema_version", 0) != 1))
            throw std::runtime_error(
                "Unsupported import settings version; source metadata is unchanged");
        const auto settings = request.settings.is_null()
                                  ? metadata.value("settings", Json::object())
                                  : request.settings;
        if (!settings.is_object())
            throw std::runtime_error("Import settings must be an object");
        result.asset_id =
            request.asset_id.empty()
                ? metadata.value("asset_id", bundle_asset_id.empty() ? uuid() : bundle_asset_id)
                : request.asset_id;
        valid_id(result.asset_id);
        if (!bundle_asset_id.empty() && bundle_asset_id != result.asset_id)
            throw std::runtime_error("Bundle AssetId disagrees with import identity");
        if (metadata.contains("asset_id") && metadata.at("asset_id") != result.asset_id)
            throw std::runtime_error("Explicit AssetId disagrees with source sidecar");
        const auto asset_root = cache_root_ / "assets" / result.asset_id;
        Json previous =
            fs::exists(asset_root / "current.json") ? current_manifest(result.asset_id) : Json();
        if (!previous.is_null()) {
            const auto previous_source =
                faset::path_from_utf8(previous.at("source").get<std::string>());
            if (previous_source != logical_source && fs::exists(previous_source))
                throw std::runtime_error("Duplicate AssetId: previous source still exists");
        }
        Dependencies dependencies;
        CookedAsset asset;
        asset.asset_id = result.asset_id;
        auto extension = faset::path_to_utf8(source.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool standalone_image =
            extension == ".png" || extension == ".jpg" || extension == ".jpeg";
        const std::string recipe_version =
            standalone_image ? "faset-image-1/stb-2.30" : importer_version;
        Json image_info;
        if (standalone_image) {
            checkpoint(job, .15f, "validating image");
            if (source_bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("Encoded image exceeds decoder limit");
            const auto* encoded = reinterpret_cast<const stbi_uc*>(source_bytes.data());
            const auto length = static_cast<int>(source_bytes.size());
            const bool png_signature =
                source_bytes.size() >= 8 && source_bytes[0] == std::byte{137} &&
                source_bytes[1] == std::byte{80} && source_bytes[2] == std::byte{78} &&
                source_bytes[3] == std::byte{71} && source_bytes[4] == std::byte{13} &&
                source_bytes[5] == std::byte{10} && source_bytes[6] == std::byte{26} &&
                source_bytes[7] == std::byte{10};
            const bool jpeg_signature =
                source_bytes.size() >= 3 && source_bytes[0] == std::byte{255} &&
                source_bytes[1] == std::byte{216} && source_bytes[2] == std::byte{255};
            if ((extension == ".png" && !png_signature) || (extension != ".png" && !jpeg_signature))
                throw std::runtime_error("Image content does not match PNG/JPEG extension");
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(encoded, length, &width, &height, &channels))
                throw std::runtime_error("Invalid PNG/JPEG image header");
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
                std::uint64_t(width) * std::uint64_t(height) > 64 * 1024 * 1024)
                throw std::runtime_error("Image exceeds 16384 dimension or 64 megapixel limit");
            auto* pixels = stbi_load_from_memory(encoded, length, &width, &height, &channels, 4);
            if (!pixels)
                throw std::runtime_error(
                    "Cannot decode PNG/JPEG image: " +
                    std::string(stbi_failure_reason() ? stbi_failure_reason() : "invalid image"));
            stbi_image_free(pixels);
            const auto pixels_per_unit = settings.value("pixels_per_unit", 100.0);
            if (!std::isfinite(pixels_per_unit) || pixels_per_unit <= 0 ||
                pixels_per_unit > 1000000)
                throw std::runtime_error("pixels_per_unit must be a finite positive "
                                         "number no greater than 1000000");
            image_info = {{"width", width},
                          {"height", height},
                          {"channels", channels},
                          {"pixels_per_unit", pixels_per_unit}};
            Texture texture;
            texture.id = stable_id("texture", "image:" + result.asset_id);
            texture.name = "Image";
            texture.mime_type = extension == ".png" ? "image/png" : "image/jpeg";
            texture.bytes = source_bytes;
            texture.wrap_s = texture.wrap_t = 33071;
            asset.textures.push_back(std::move(texture));
            checkpoint(job, .5f, "preparing image payload");
        } else {
            cgltf_options options{};
            cgltf_data* raw = nullptr;
            auto parse = cgltf_parse(&options, source_bytes.data(), source_bytes.size(), &raw);
            if (parse != cgltf_result_success)
                throw std::runtime_error("Invalid glTF/GLB (parse " + std::to_string(parse) + ")");
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
            for (std::size_t i = 0; i < data->extensions_required_count; ++i)
                if (std::string(data->extensions_required[i]) != "KHR_materials_unlit")
                    throw std::runtime_error("Unsupported required extension: " +
                                             std::string(data->extensions_required[i]));
            for (std::size_t i = 0; i < data->buffers_count; ++i) {
                const auto* uri = data->buffers[i].uri;
                if (uri && !std::string_view(uri).starts_with("data:")) {
                    dependency_bytes(source, uri, dependencies);
                    auto& snapshot = dependencies.at(uri).bytes;
                    if (snapshot.size() < data->buffers[i].size)
                        throw std::runtime_error("External buffer shorter than declared");
                    data->buffers[i].data = snapshot.data();
                    data->buffers[i].data_free_method = cgltf_data_free_method_none;
                }
            }
            if (cgltf_load_buffers(&options, data.get(), faset::path_to_utf8(source).c_str()) !=
                cgltf_result_success)
                throw std::runtime_error("Cannot load glTF buffers");
            if (cgltf_validate(data.get()) != cgltf_result_success)
                throw std::runtime_error("Invalid glTF buffer/accessor layout");
            checkpoint(job, .15f, "extracting geometry");
            std::set<std::string> identifiers;
            auto identify = [&](const std::string& kind, const cgltf_extras& extras,
                                const std::string& fallback) {
                const auto source_identity = source_id(extras);
                const auto id =
                    stable_id(kind, source_identity.empty() ? "fallback:" + fallback
                                                            : "source:" + source_identity);
                if (!identifiers.insert(id).second)
                    throw std::runtime_error("DuplicateSourceId: " + kind + " " + source_identity);
                return id;
            };
            for (std::size_t mi = 0; mi < data->meshes_count; ++mi) {
                checkpoint(job,
                           .15f + .3f * static_cast<float>(mi) /
                                      std::max<std::size_t>(1, data->meshes_count),
                           "extracting meshes");
                const auto& mesh = data->meshes[mi];
                Mesh cooked;
                cooked.name = safe_name(mesh.name);
                cooked.id = identify("mesh", mesh.extras, std::to_string(mi) + ":" + cooked.name);
                for (std::size_t pi = 0; pi < mesh.primitives_count; ++pi) {
                    const auto& primitive = mesh.primitives[pi];
                    if (primitive.type != cgltf_primitive_type_triangles ||
                        primitive.has_draco_mesh_compression)
                        throw std::runtime_error(
                            "Only uncompressed triangle primitives are supported");
                    const cgltf_accessor *positions = nullptr, *normals = nullptr, *uv = nullptr;
                    for (std::size_t ai = 0; ai < primitive.attributes_count; ++ai) {
                        const auto& a = primitive.attributes[ai];
                        if (a.type == cgltf_attribute_type_position)
                            positions = a.data;
                        if (a.type == cgltf_attribute_type_normal)
                            normals = a.data;
                        if (a.type == cgltf_attribute_type_texcoord && a.index == 0)
                            uv = a.data;
                    }
                    const auto xyz = unpack(positions, 3);
                    const auto normal = normals ? unpack(normals, 3) : std::vector<float>{};
                    const auto tex = uv ? unpack(uv, 2) : std::vector<float>{};
                    const auto count = xyz.size() / 3;
                    if ((normals && normal.size() != count * 3) || (uv && tex.size() != count * 2))
                        throw std::runtime_error("Vertex attribute counts differ");
                    Primitive output;
                    output.material = primitive.material
                                          ? static_cast<int>(primitive.material - data->materials)
                                          : -1;
                    output.vertices.resize(count);
                    for (std::size_t i = 0; i < count; ++i) {
                        if (i % 4096 == 0 && job.cancelled())
                            throw Cancelled{};
                        std::copy_n(xyz.data() + i * 3, 3, output.vertices[i].position.begin());
                        if (normals)
                            std::copy_n(normal.data() + i * 3, 3,
                                        output.vertices[i].normal.begin());
                        if (uv)
                            std::copy_n(tex.data() + i * 2, 2, output.vertices[i].uv.begin());
                    }
                    if (primitive.indices &&
                        (primitive.indices->is_sparse ||
                         primitive.indices->type != cgltf_type_scalar ||
                         (primitive.indices->component_type != cgltf_component_type_r_8u &&
                          primitive.indices->component_type != cgltf_component_type_r_16u &&
                          primitive.indices->component_type != cgltf_component_type_r_32u)))
                        throw std::runtime_error(
                            "Indices require a dense unsigned integer accessor");
                    const auto index_count = primitive.indices ? primitive.indices->count : count;
                    if (index_count % 3 || index_count > 30000000)
                        throw std::runtime_error("Invalid triangle index count");
                    output.indices.resize(index_count);
                    for (std::size_t i = 0; i < index_count; ++i) {
                        if (i % 4096 == 0 && job.cancelled())
                            throw Cancelled{};
                        const auto index =
                            primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
                        if (index >= count)
                            throw std::runtime_error("Index outside vertex array");
                        output.indices[i] = static_cast<std::uint32_t>(index);
                    }
                    if (!normals)
                        calculate_normals(output);
                    cooked.primitives.push_back(std::move(output));
                    if (primitive.targets_count)
                        result.diagnostics.push_back(
                            "Morph targets imported as static base geometry");
                }
                asset.meshes.push_back(std::move(cooked));
            }
            for (std::size_t ni = 0; ni < data->nodes_count; ++ni) {
                const auto& node = data->nodes[ni];
                Node output;
                output.name = safe_name(node.name);
                output.stable_source_id = !source_id(node.extras).empty();
                output.id = identify("node", node.extras, std::to_string(ni) + ":" + output.name);
                output.mesh = node.mesh ? static_cast<int>(node.mesh - data->meshes) : -1;
                cgltf_node_transform_local(&node, output.local_transform.data());
                if (!std::all_of(output.local_transform.begin(), output.local_transform.end(),
                                 [](float v) { return std::isfinite(v); }))
                    throw std::runtime_error("Non-finite node transform");
                asset.nodes.push_back(std::move(output));
                if (node.skin)
                    result.diagnostics.push_back(
                        "Skinned node imported in static rest pose; animation playback "
                        "is not cooked");
            }
            for (std::size_t ni = 0; ni < data->nodes_count; ++ni)
                if (data->nodes[ni].parent)
                    asset.nodes[ni].parent_id =
                        asset.nodes[static_cast<std::size_t>(data->nodes[ni].parent - data->nodes)]
                            .id;
            // Only instantiate the selected/default scene. Unused resources remain
            // reusable outputs.
            if (data->scenes_count) {
                const auto* selected = data->scene ? data->scene : &data->scenes[0];
                std::set<std::size_t> active_nodes;
                std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* n) {
                    auto index = static_cast<std::size_t>(n - data->nodes);
                    if (!active_nodes.insert(index).second)
                        return;
                    for (std::size_t i = 0; i < n->children_count; ++i)
                        visit(n->children[i]);
                };
                for (std::size_t i = 0; i < selected->nodes_count; ++i)
                    visit(selected->nodes[i]);
                std::vector<Node> active;
                for (std::size_t i = 0; i < asset.nodes.size(); ++i)
                    if (active_nodes.contains(i))
                        active.push_back(std::move(asset.nodes[i]));
                asset.nodes = std::move(active);
            }
            checkpoint(job, .5f, "extracting materials and textures");
            for (std::size_t mi = 0; mi < data->materials_count; ++mi) {
                const auto& m = data->materials[mi];
                Material out;
                out.name = safe_name(m.name);
                out.id = identify("material", m.extras, std::to_string(mi) + ":" + out.name);
                if (m.has_pbr_metallic_roughness) {
                    const auto& p = m.pbr_metallic_roughness;
                    std::copy_n(p.base_color_factor, 4, out.base_color.begin());
                    out.metallic = p.metallic_factor;
                    out.roughness = p.roughness_factor;
                    out.base_color_texture = texture_index(p.base_color_texture, *data);
                    out.metallic_roughness_texture =
                        texture_index(p.metallic_roughness_texture, *data);
                }
                std::copy_n(m.emissive_factor, 3, out.emissive.begin());
                out.alpha_mode = m.alpha_mode == cgltf_alpha_mode_blend  ? "BLEND"
                                 : m.alpha_mode == cgltf_alpha_mode_mask ? "MASK"
                                                                         : "OPAQUE";
                out.alpha_cutoff = m.alpha_cutoff;
                out.double_sided = m.double_sided;
                out.unlit = m.unlit;
                out.normal_texture = texture_index(m.normal_texture, *data);
                out.occlusion_texture = texture_index(m.occlusion_texture, *data);
                out.emissive_texture = texture_index(m.emissive_texture, *data);
                asset.materials.push_back(std::move(out));
            }
            for (std::size_t ti = 0; ti < data->textures_count; ++ti) {
                const auto& texture = data->textures[ti];
                if (!texture.image)
                    throw std::runtime_error("Texture extension has no supported fallback image");
                Texture out;
                out.name = safe_name(texture.name);
                out.id = identify("texture", texture.extras, std::to_string(ti) + ":" + out.name);
                out.bytes = image_bytes(*texture.image, source, dependencies);
                out.mime_type = image_mime(*texture.image, out.bytes);
                if (texture.sampler) {
                    out.wrap_s = texture.sampler->wrap_s;
                    out.wrap_t = texture.sampler->wrap_t;
                    out.min_filter = texture.sampler->min_filter;
                    out.mag_filter = texture.sampler->mag_filter;
                }
                asset.textures.push_back(std::move(out));
            }
        }
        Json key{
            {"source", source_hash},
            {"settings", settings},
            {"importer", recipe_version},
            {"target_profile", standalone_image ? "desktop-image-v1" : "desktop-static-pbr-v1"},
            {"toolchain", {{"cgltf", FASET_CGLTF_COMMIT}, {"stb", FASET_STB_COMMIT}}},
            {"dependencies", Json::object()}};
        if (source != logical_source)
            key["bundle_sha256"] = logical_hash;
        for (const auto& [name, item] : dependencies)
            key["dependencies"][name] = item.digest;
        result.generation = faset::sha256(key.dump());
        asset.generation = result.generation;
        Json manifest{{"schema_version", 1},
                      {"asset_id", result.asset_id},
                      {"generation", result.generation},
                      {"source", faset::path_to_utf8(logical_source)},
                      {"payload_source", faset::path_to_utf8(source)},
                      {"source_sha256", source_hash},
                      {"importer", recipe_version},
                      {"settings", settings},
                      {"input_key", key},
                      {"nodes", Json::array()},
                      {"meshes", Json::array()},
                      {"materials", Json::array()},
                      {"textures", Json::array()},
                      {"files", Json::array()},
                      {"outputs", Json::array()}};
        manifest["kind"] = standalone_image ? "image" : "scene";
        if (standalone_image)
            manifest["image"] = image_info;
        stage = cache_root_ / "staging" / uuid();
        fs::create_directories(stage);
        for (const auto& node : asset.nodes) {
            manifest["nodes"].push_back({{"id", node.id},
                                         {"name", node.name},
                                         {"parent_id", node.parent_id},
                                         {"mesh", node.mesh},
                                         {"local_transform", node.local_transform},
                                         {"stable_source_id", node.stable_source_id}});
            manifest["outputs"].push_back(node.id);
        }
        for (const auto& mesh : asset.meshes) {
            Json m{{"id", mesh.id}, {"name", mesh.name}, {"primitives", Json::array()}};
            for (std::size_t i = 0; i < mesh.primitives.size(); ++i) {
                const auto file = "meshes/" + mesh.id + "-" + std::to_string(i) + ".fmesh";
                add_file(manifest, stage, file, encode_primitive(mesh.primitives[i]));
                m["primitives"].push_back(
                    {{"path", file}, {"material", mesh.primitives[i].material}});
            }
            manifest["meshes"].push_back(m);
            manifest["outputs"].push_back(mesh.id);
        }
        for (const auto& material : asset.materials) {
            manifest["materials"].push_back(material_json(material));
            manifest["outputs"].push_back(material.id);
        }
        for (const auto& texture : asset.textures) {
            const auto file = "textures/" + texture.id + ".image";
            add_file(manifest, stage, file, texture.bytes);
            manifest["textures"].push_back({{"id", texture.id},
                                            {"name", texture.name},
                                            {"mime_type", texture.mime_type},
                                            {"path", file},
                                            {"wrap_s", texture.wrap_s},
                                            {"wrap_t", texture.wrap_t},
                                            {"min_filter", texture.min_filter},
                                            {"mag_filter", texture.mag_filter}});
            manifest["outputs"].push_back(texture.id);
        }
        checkpoint(job, .75f, "validating candidate generation");
        validate_generation(stage, manifest);
        const auto published_ids = manifest.at("outputs").get<std::set<std::string>>();
        if (!previous.is_null())
            for (const auto& old : previous.at("outputs"))
                if (!published_ids.contains(old.get<std::string>()))
                    result.removed_output_ids.push_back(old.get<std::string>());
        result.manifest = manifest;
        result.previous_generation =
            previous.is_null() ? "" : previous.at("generation").get<std::string>();
        if ((!request.expected_generation.empty() &&
             request.expected_generation != result.generation) ||
            (!request.expected_active_generation.empty() &&
             request.expected_active_generation != result.previous_generation)) {
            result.status = ImportStatus::conflict;
            result.diagnostics.push_back(
                "Import changed since review; reimport and review the current removed outputs. "
                "Active generation preserved");
            fs::remove_all(stage);
            return result;
        }
        if (!result.removed_output_ids.empty() && !request.allow_removed_outputs) {
            result.status = ImportStatus::conflict;
            result.diagnostics.push_back(
                "Removed or renamed outputs require explicit remap/removal approval; "
                "active generation preserved");
            fs::remove_all(stage);
            return result;
        }
        if (hash_bytes(read_bytes(source)) != source_hash)
            throw std::runtime_error("Source changed during import; retry");
        if (source != logical_source && hash_bytes(read_bytes(logical_source)) != logical_hash)
            throw std::runtime_error("Bundle manifest changed during import; retry");
        for (const auto& [name, item] : dependencies)
            if (hash_bytes(read_bytes(item.path)) != item.digest)
                throw std::runtime_error("Dependency changed during import: " + name);
        write_json(stage / "manifest.json", manifest);
        checkpoint(job, .9f, "publishing generation");
        const auto destination = asset_root / "generations" / result.generation;
        fs::create_directories(destination.parent_path());
        if (fs::exists(destination)) {
            auto existing = read_json(destination / "manifest.json");
            validate_generation(destination, existing);
            if (existing.at("input_key") != key)
                throw std::runtime_error("Digest collision detected");
            result.cache_hit = true;
            fs::remove_all(stage);
        } else
            fs::rename(stage, destination);
        // Persist identity outside the disposable cache, then atomically publish
        // one pointer.
        metadata = {{"schema_version", 1}, {"asset_id", result.asset_id}, {"settings", settings}};
        atomic_json(sidecar, metadata);
        if (job.cancelled())
            throw Cancelled{};
        atomic_json(asset_root / "current.json", {{"schema_version", 1},
                                                  {"generation", result.generation},
                                                  {"source", faset::path_to_utf8(logical_source)},
                                                  {"payload_source", faset::path_to_utf8(source)}});
        result.status = ImportStatus::succeeded;
        job.report(1, "complete");
    } catch (const Cancelled&) {
        result.status = ImportStatus::cancelled;
        result.diagnostics.push_back("Import cancelled; active generation unchanged");
    } catch (const std::exception& e) {
        result.status = ImportStatus::failed;
        result.diagnostics.push_back(e.what());
    }
    if (!stage.empty()) {
        std::error_code ec;
        fs::remove_all(stage, ec);
    }
    return result;
}
Json AssetPipeline::freshness(const std::string& id) const {
    Json result{{"state", "current"}, {"reasons", Json::array()}};
    auto stale = [&](const std::string& code, const fs::path& path, const std::string& message) {
        result["state"] = "stale";
        result["reasons"].push_back(
            {{"code", code}, {"path", faset::path_to_utf8(path)}, {"message", message}});
    };
    try {
        const auto manifest = current_manifest(id);
        result["generation"] = manifest.at("generation");
        const auto source = faset::path_from_utf8(manifest.at("source").get<std::string>());
        const auto payload =
            faset::path_from_utf8(manifest.at("payload_source").get<std::string>());
        const auto& key = manifest.at("input_key");
        auto check_file = [&](const fs::path& path, const std::string& digest,
                              const std::string& code) {
            try {
                if (faset::sha256_file(path) != digest)
                    stale(code, path, "Input changed; reimport to update the active generation");
            } catch (const std::exception& error) {
                stale("input.unavailable", path, error.what());
            }
        };
        check_file(payload, manifest.at("source_sha256"), "source.changed");
        if (key.contains("bundle_sha256")) {
            check_file(source, key.at("bundle_sha256"), "bundle.changed");
            // The bundle can declare more than its selected GLB. Validate every
            // declared payload, as import does, even when the manifest is unchanged.
            try {
                const auto bundle = read_json(source);
                for (const auto& file : bundle.at("files")) {
                    const auto relative = faset::path_from_utf8(file.at("path").get<std::string>());
                    if (relative.is_absolute() ||
                        faset::generic_path_to_utf8(relative).find("..") != std::string::npos)
                        throw std::runtime_error("Invalid bundle payload path");
                    check_file(source.parent_path() / relative, file.at("sha256"),
                               "bundle.payload_changed");
                }
            } catch (const std::exception& error) {
                stale("bundle.unavailable", source, error.what());
            }
        }
        for (const auto& [uri, digest] : key.at("dependencies").items())
            check_file(external_path(payload, uri), digest, "dependency.changed");

        const auto sidecar =
            faset::path_from_utf8(faset::path_to_utf8(source) + ".faset-import.json");
        try {
            const auto metadata = read_json(sidecar);
            if (metadata.value("schema_version", 0) != 1 || metadata.value("asset_id", "") != id ||
                metadata.value("settings", Json::object()) != manifest.at("settings"))
                stale("settings.changed", sidecar,
                      "Import identity or recipe changed; reimport required");
        } catch (const std::exception& error) {
            stale("settings.unavailable", sidecar, error.what());
        }
        const bool image = manifest.value("kind", "scene") == "image";
        const std::string recipe = image ? "faset-image-1/stb-2.30" : importer_version;
        const std::string profile = image ? "desktop-image-v1" : "desktop-static-pbr-v1";
        const Json toolchain{{"cgltf", FASET_CGLTF_COMMIT}, {"stb", FASET_STB_COMMIT}};
        if (key.at("importer") != recipe || key.at("target_profile") != profile ||
            key.at("toolchain") != toolchain)
            stale("importer.changed", source,
                  "Importer, toolchain or target profile changed; reimport required");
    } catch (const std::exception& error) {
        result["state"] = "unavailable";
        result["reasons"].push_back({{"code", "manifest.unavailable"}, {"message", error.what()}});
    }
    return result;
}
Json AssetPipeline::overrides(const std::string& id) const {
    const auto source = current_manifest(id).at("source").get<std::string>();
    const auto path = faset::path_from_utf8(source + ".faset-overrides.json");
    return fs::exists(path) ? read_json(path) : Json::object();
}
void AssetPipeline::set_overrides(const std::string& id, const Json& values) {
    if (!values.is_object())
        throw std::runtime_error("Overrides must be an object keyed by stable output IDs");
    std::lock_guard lock(writer_mutex);
    atomic_json(faset::path_from_utf8(current_manifest(id).at("source").get<std::string>() +
                                      ".faset-overrides.json"),
                values);
}
} // namespace faset::assets
