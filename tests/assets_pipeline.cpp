#include "assets_image_fixtures.hpp"
#include <bit>
#include <chrono>
#include <faset/assets/asset_pipeline.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace faset::assets;
namespace fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void u32(std::vector<unsigned char>& data, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void save(const fs::path& file, const std::vector<unsigned char>& data) {
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}
void save(const fs::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary) << text;
}
std::vector<unsigned char> geometry(float x) {
    std::vector<unsigned char> bin;
    for (float f : {x, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
        u32(bin, std::bit_cast<std::uint32_t>(f));
    for (unsigned char c : {0, 0, 1, 0, 2, 0})
        bin.push_back(c);
    return bin;
}
Json document(const std::string& name, bool stable, bool second, float x = 0) {
    Json node{{"name", name}, {"mesh", 0}};
    if (stable)
        node["extras"] = {{"faset_id", "node-door"}};
    Json j = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
        {"nodes", Json::array({node})},
        {"buffers", Json::array({{{"byteLength", 42}}})},
        {"bufferViews",
         Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}, {"target", 34962}},
                      {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 6}, {"target", 34963}}})},
        {"accessors",
         Json::array(
             {{{"bufferView", 0},
               {"componentType", 5126},
               {"count", 3},
               {"type", "VEC3"},
               {"min", {std::min(0.f, x), 0, 0}},
               {"max", {std::max(1.f, x), 1, 0}}},
              {{"bufferView", 1}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}})},
        {"meshes", Json::array({{{"name", "Triangle"},
                                 {"extras", {{"faset_id", "mesh-triangle"}}},
                                 {"primitives", Json::array({{{"attributes", {{"POSITION", 0}}},
                                                              {"indices", 1},
                                                              {"material", 0}}})}}})},
        {"materials", Json::array({{{"name", "Red"},
                                    {"pbrMetallicRoughness",
                                     {{"baseColorFactor", {1.0, 0.2, 0.1, 1.0}},
                                      {"metallicFactor", 0.2},
                                      {"roughnessFactor", 0.6}}}}})}};
    if (second) {
        j["nodes"].push_back(
            {{"name", "Handle"}, {"mesh", 0}, {"extras", {{"faset_id", "node-handle"}}}});
        j["scenes"][0]["nodes"].push_back(1);
    }
    return j;
}
void glb(const fs::path& path, const std::string& name = "Door", bool stable = true,
         bool second = false, float x = 0) {
    auto json = document(name, stable, second, x).dump();
    while (json.size() % 4)
        json += ' ';
    auto binary = geometry(x);
    while (binary.size() % 4)
        binary.push_back(0);
    std::vector<unsigned char> result;
    u32(result, 0x46546c67);
    u32(result, 2);
    u32(result, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
    u32(result, static_cast<std::uint32_t>(json.size()));
    u32(result, 0x4e4f534a);
    result.insert(result.end(), json.begin(), json.end());
    u32(result, static_cast<std::uint32_t>(binary.size()));
    u32(result, 0x004e4942);
    result.insert(result.end(), binary.begin(), binary.end());
    save(path, result);
}
void success(const ImportResult& result) {
    if (!result.ok()) {
        std::string text = "Import failed: ";
        for (const auto& d : result.diagnostics)
            text += d + "; ";
        throw std::runtime_error(text);
    }
}
} // namespace
int main() {
    const auto root =
        fs::temp_directory_path() /
        faset::path_from_utf8(
            "Faset Café 世界 assets " +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        AssetPipeline pipeline(root / "cache");
        const auto source = root / faset::path_from_utf8("дверь 世界.glb");
        glb(source);
        auto first = pipeline.import_asset({source});
        success(first);
        require(first.manifest.at("input_key").at("target_profile") == "desktop-static-pbr-v1" &&
                    first.manifest.at("input_key")
                            .at("toolchain")
                            .at("cgltf")
                            .get<std::string>()
                            .size() == 40,
                "cache recipe omits the desktop profile or pinned importer toolchain");
        require(first.manifest.at("materials").at(0).at("format") == "faset.material" &&
                    first.manifest.at("materials").at(0).at("version") == 1,
                "material data is not explicitly versioned");
        require(!first.asset_id.empty(), "persistent identity missing");
        auto asset = pipeline.load_asset(first.asset_id);
        require(asset.meshes.size() == 1 && asset.nodes.size() == 1, "mesh/node extraction");
        require(asset.meshes[0].primitives[0].indices == std::vector<std::uint32_t>({0, 1, 2}),
                "index extraction");
        require(asset.meshes[0].primitives[0].vertices[0].normal[2] == 1, "generated normal");
        require(asset.materials[0].base_color[1] > .19f && asset.materials[0].metallic == .2f,
                "PBR extraction");
        const auto node_id = asset.nodes[0].id;
        const Json custom{{node_id,
                           {{"gameplay", {{"locked", true}}},
                            {"physics", {{"mass", 12}}},
                            {"material", "custom-brass"}}}};
        pipeline.set_overrides(first.asset_id, custom);
        AssetPipeline rebuilt_cache(root / "rebuilt-cache");
        const auto recovered = rebuilt_cache.import_asset({source});
        success(recovered);
        require(recovered.asset_id == first.asset_id && recovered.generation == first.generation &&
                    rebuilt_cache.load_asset(first.asset_id).nodes[0].id == node_id &&
                    rebuilt_cache.overrides(first.asset_id) == custom,
                "fresh cache changed source identity, outputs, or authoring overrides");
        auto unchanged = pipeline.import_asset({source});
        success(unchanged);
        require(unchanged.cache_hit && unchanged.generation == first.generation,
                "content cache hit");
        glb(source, "Renamed panel", true, false, .25f);
        auto modified = pipeline.import_asset({source});
        success(modified);
        require(modified.asset_id == first.asset_id && modified.generation != first.generation,
                "stable asset identity and changed generation");
        asset = pipeline.load_asset(first.asset_id);
        require(asset.nodes[0].id == node_id && asset.nodes[0].name == "Renamed panel",
                "faset_id survives rename");
        require(asset.meshes[0].primitives[0].vertices[0].position[0] == .25f,
                "new binary geometry loaded");
        require(pipeline.overrides(first.asset_id) == custom,
                "reimport erased authoring overrides");
        glb(source, "Renamed panel", true, true, .25f);
        auto added = pipeline.import_asset({source});
        success(added);
        glb(source, "Renamed panel", true, false, .25f);
        auto removed = pipeline.import_asset({source});
        require(removed.status == ImportStatus::conflict && !removed.removed_output_ids.empty(),
                "deletion must conflict");
        require(pipeline.load_asset(first.asset_id).generation == added.generation,
                "conflict replaced active generation");
        require(pipeline.overrides(first.asset_id) == custom, "conflict erased overrides");
        ImportRequest resolve{source};
        resolve.allow_removed_outputs = true;
        resolve.expected_generation = removed.generation;
        resolve.expected_active_generation = added.generation;
        glb(source, "Renamed panel", true, false, .5f);
        auto changed_since_review = pipeline.import_asset(resolve);
        require(changed_since_review.status == ImportStatus::conflict &&
                    pipeline.load_asset(first.asset_id).generation == added.generation,
                "review approval must not publish a changed candidate");
        glb(source, "Renamed panel", true, false, .25f);
        resolve.expected_active_generation = first.generation;
        require(pipeline.import_asset(resolve).status == ImportStatus::conflict &&
                    pipeline.load_asset(first.asset_id).generation == added.generation,
                "review approval must not replace a different active generation");
        resolve.expected_active_generation = added.generation;
        auto resolved = pipeline.import_asset(resolve);
        success(resolved);
        require(pipeline.load_asset(first.asset_id).nodes.size() == 1,
                "explicit deletion resolution");
        const auto active = resolved.generation;
        save(source, std::string("not a GLB"));
        auto failed = pipeline.import_asset({source});
        require(failed.status == ImportStatus::failed, "invalid source accepted");
        require(pipeline.load_asset(first.asset_id).generation == active,
                "failed import replaced active");
        glb(source, "Renamed panel", true, false, .5f);
        ImportJob* job_ptr = nullptr;
        ImportJob job([&](const ImportProgress& p) {
            if (p.fraction >= .9f)
                job_ptr->cancel();
        });
        job_ptr = &job;
        auto cancelled = pipeline.import_asset({source}, job);
        require(cancelled.status == ImportStatus::cancelled, "cancel before commit failed");
        require(pipeline.load_asset(first.asset_id).generation == active, "cancel replaced active");
        ImportRequest changed_settings{source};
        changed_settings.settings = {{"target", "test-profile"}};
        auto settings = pipeline.import_asset(changed_settings);
        success(settings);
        require(settings.generation != active, "recipe omitted from cache key");
        const auto plain = root / "ordinary.glb";
        glb(plain, "Ordinary", false);
        auto standard = pipeline.import_asset({plain});
        success(standard);
        require(!pipeline.load_asset(standard.asset_id).nodes[0].stable_source_id,
                "ordinary GLB wrongly marked stable source");
        glb(plain, "Renamed without ID", false);
        require(pipeline.import_asset({plain}).status == ImportStatus::conflict,
                "ambiguous rename silently matched");
        auto duplicate = document("Duplicate", true, true);
        duplicate["nodes"][1]["extras"]["faset_id"] = "node-door";
        duplicate["buffers"][0]["uri"] = "геометрия.bin";
        save(root / faset::path_from_utf8("геометрия.bin"), geometry(0));
        save(root / "duplicate.gltf", duplicate.dump());
        require(pipeline.import_asset({root / "duplicate.gltf"}).status == ImportStatus::failed,
                "duplicate source ID accepted");
        auto external = document("External", true, false);
        external["buffers"][0]["uri"] = "геометрия.bin";
        external["images"] = Json::array({{{"uri", "пиксель.png"}, {"mimeType", "image/png"}}});
        external["textures"] = Json::array({{{"source", 0}}});
        external["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 0}};
        // Real 1x1 PNG payload, transparent pixel; importer owns encoded bytes.
        const std::vector<unsigned char> png = {
            137, 80,  78,  71, 13, 10, 26, 10,  0,   0,  0,  13, 73, 72,  68,  82,  0,
            0,   0,   1,   0,  0,  0,  1,  8,   6,   0,  0,  0,  31, 21,  196, 137, 0,
            0,   0,   11,  73, 68, 65, 84, 120, 156, 99, 96, 0,  2,  0,   0,   5,   0,
            1,   165, 246, 69, 64, 0,  0,  0,   0,   73, 69, 78, 68, 174, 66,  96,  130};
        save(root / faset::path_from_utf8("пиксель.png"), png);
        save(root / "external.gltf", external.dump());
        auto ext = pipeline.import_asset({root / "external.gltf"});
        success(ext);
        auto ext_asset = pipeline.load_asset(ext.asset_id);
        require(ext_asset.textures.size() == 1 && ext_asset.textures[0].bytes.size() == png.size(),
                "external image not extracted");
        require(ext_asset.materials[0].base_color_texture == 0, "material texture reference lost");
        save(root / faset::path_from_utf8("геометрия.bin"), geometry(.3f));
        auto dependent = pipeline.import_asset({root / "external.gltf"});
        success(dependent);
        require(dependent.generation != ext.generation, "buffer dependency not invalidated");
        // A Blender bundle keeps its logical source stable while immutable payload paths change.
        const auto bundle_dir = root / "bundle";
        fs::create_directories(bundle_dir / "payload");
        glb(bundle_dir / "payload" / "first.glb", "Bundle", true, false);
        Json bundle{{"schema_version", 1},
                    {"asset_id", "bundle-asset"},
                    {"files", Json::array({{{"path", "payload/first.glb"},
                                            {"sha256", faset::sha256_file(bundle_dir / "payload" /
                                                                          "first.glb")}}})}};
        save(bundle_dir / "manifest.json", bundle.dump());
        auto bundle_first = pipeline.import_asset({bundle_dir / "manifest.json"});
        success(bundle_first);
        require(bundle_first.asset_id == "bundle-asset", "bundle identity lost");
        glb(bundle_dir / "payload" / "second.glb", "Bundle renamed", true, false, .4f);
        bundle["files"][0] = {
            {"path", "payload/second.glb"},
            {"sha256", faset::sha256_file(bundle_dir / "payload" / "second.glb")}};
        save(bundle_dir / "manifest.json", bundle.dump());
        auto bundle_second = pipeline.import_asset({bundle_dir / "manifest.json"});
        success(bundle_second);
        require(bundle_second.asset_id == bundle_first.asset_id &&
                    bundle_second.generation != bundle_first.generation,
                "bundle reimport identity/generation");
        bundle["files"][0]["sha256"] = std::string(64, '0');
        save(bundle_dir / "manifest.json", bundle.dump());
        require(pipeline.import_asset({bundle_dir / "manifest.json"}).status ==
                    ImportStatus::failed,
                "bundle checksum ignored");
        require(pipeline.load_asset("bundle-asset").generation == bundle_second.generation,
                "bad bundle replaced active");
        // Changing a dependency after it was snapshotted cannot publish mixed content.
        const auto dependency_active = pipeline.load_asset(ext.asset_id).generation;
        ImportJob mutate([&](const ImportProgress& progress) {
            if (progress.fraction == .5f)
                save(root / faset::path_from_utf8("геометрия.bin"), geometry(.7f));
        });
        require(pipeline.import_asset({root / "external.gltf"}, mutate).status ==
                    ImportStatus::failed,
                "concurrent dependency edit accepted");
        require(pipeline.load_asset(ext.asset_id).generation == dependency_active,
                "concurrent edit changed active");
        // Standalone images share the same identity, generation and failure guarantees.
        const auto picture = root / faset::path_from_utf8("спрайт Café.PNG");
        save(picture, faset::test_images::png_red_green);
        ImportRequest image_request{picture};
        image_request.settings = {{"pixels_per_unit", 20.0}};
        auto image_first = pipeline.import_asset(image_request);
        success(image_first);
        auto image_asset = pipeline.load_asset(image_first.asset_id);
        require(image_asset.meshes.empty() && image_asset.textures.size() == 1,
                "Standalone PNG should produce one owned texture");
        require(image_asset.textures[0].mime_type == "image/png" &&
                    image_first.manifest["kind"] == "image",
                "PNG kind/MIME");
        require(image_first.manifest["image"]["width"] == 2 &&
                    image_first.manifest["image"]["height"] == 1 &&
                    image_first.manifest["image"]["pixels_per_unit"] == 20.0,
                "PNG dimensions and sprite scale recipe");
        const auto texture_id = image_asset.textures[0].id;
        pipeline.set_overrides(image_first.asset_id, {{texture_id, {{"gameplay", "preserved"}}}});
        auto image_cache = pipeline.import_asset({picture});
        success(image_cache);
        require(image_cache.cache_hit && image_cache.generation == image_first.generation,
                "PNG cache/recipe persistence");
        save(picture, faset::test_images::png_blue_white);
        auto image_changed = pipeline.import_asset({picture});
        success(image_changed);
        require(image_changed.asset_id == image_first.asset_id &&
                    image_changed.generation != image_first.generation,
                "PNG reimport content invalidation");
        require(pipeline.load_asset(image_first.asset_id).textures[0].id == texture_id,
                "Image subasset ID changed after content edit");
        image_request.settings = {{"pixels_per_unit", 40.0}};
        auto image_recipe = pipeline.import_asset(image_request);
        success(image_recipe);
        require(image_recipe.generation != image_changed.generation,
                "Image settings omitted from recipe hash");
        save(picture, std::string("broken PNG"));
        require(pipeline.import_asset({picture}).status == ImportStatus::failed,
                "Broken PNG published");
        require(pipeline.load_asset(image_first.asset_id).generation == image_recipe.generation,
                "Broken PNG replaced successful generation");
        save(picture, faset::test_images::jpeg_red_green);
        require(pipeline.import_asset({picture}).status == ImportStatus::failed,
                "Mismatched JPEG content accepted as PNG");
        auto oversized = faset::test_images::png_red_green;
        oversized[16] = 0;
        oversized[17] = 1;
        oversized[18] = 0;
        oversized[19] = 0;
        save(picture, oversized);
        require(pipeline.import_asset({picture}).status == ImportStatus::failed,
                "Oversized image accepted");
        save(picture, faset::test_images::png_red_green);
        ImportJob* image_job_pointer = nullptr;
        ImportJob image_job([&](const ImportProgress& progress) {
            if (progress.fraction >= .5f)
                image_job_pointer->cancel();
        });
        image_job_pointer = &image_job;
        require(pipeline.import_asset({picture}, image_job).status == ImportStatus::cancelled,
                "PNG cancellation ignored");
        require(pipeline.load_asset(image_first.asset_id).generation == image_recipe.generation,
                "Cancelled PNG replaced active generation");
        image_request.settings = {{"pixels_per_unit", 0}};
        require(pipeline.import_asset(image_request).status == ImportStatus::failed,
                "Zero pixels_per_unit accepted");
        save(picture, faset::test_images::png_blue_white);
        const auto renamed = root / "renamed.PNG";
        fs::rename(picture, renamed);
        fs::rename(faset::path_from_utf8(faset::path_to_utf8(picture) + ".faset-import.json"),
                   faset::path_from_utf8(faset::path_to_utf8(renamed) + ".faset-import.json"));
        fs::rename(faset::path_from_utf8(faset::path_to_utf8(picture) + ".faset-overrides.json"),
                   faset::path_from_utf8(faset::path_to_utf8(renamed) + ".faset-overrides.json"));
        auto image_rename = pipeline.import_asset({renamed});
        success(image_rename);
        require(image_rename.cache_hit && image_rename.asset_id == image_first.asset_id,
                "Image source rename with sidecar lost identity/cache");
        require(pipeline.current_manifest(image_first.asset_id)["source"] ==
                    faset::path_to_utf8(renamed),
                "Image rename retained old source pointer");
        require(pipeline.overrides(image_first.asset_id).contains(texture_id),
                "Image rename/reimport lost overrides");
        const auto jpeg = root / "sprite.JPEG";
        save(jpeg, faset::test_images::jpeg_red_green);
        auto jpeg_import = pipeline.import_asset({jpeg});
        success(jpeg_import);
        require(jpeg_import.manifest["image"]["width"] == 2 &&
                    pipeline.load_asset(jpeg_import.asset_id).textures[0].mime_type == "image/jpeg",
                "JPEG decoding/dimensions");
        save(root / "same.png", faset::test_images::png_blue_white);
        auto other_image = pipeline.import_asset({root / "same.png"});
        success(other_image);
        require(other_image.asset_id != image_first.asset_id,
                "Independent same-content source must have independent AssetId");
        fs::remove_all(root / "cache");
        auto restored = pipeline.import_asset({source});
        success(restored);
        require(restored.asset_id == first.asset_id, "cleared cache changed AssetId");
        require(restored.manifest["settings"] == changed_settings.settings,
                "persisted recipe lost");
        require(pipeline.overrides(first.asset_id) == custom,
                "cleared cache lost authoring overrides");
        fs::remove_all(root);
        std::cout << "assets: geometry/PBR/texture, GLB/glTF, cache, rename, deletion, overrides, "
                     "failure, cancellation, standalone PNG/JPEG OK\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nFixtures retained: " << root << '\n';
        return 1;
    }
}
