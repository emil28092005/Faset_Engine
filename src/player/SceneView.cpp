#include <algorithm>
#include <cmath>
#include <faset/assets/asset_data.hpp>
#include <faset/player/SceneView.hpp>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#if defined(FASET_HAS_STB)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#endif

namespace faset::player {
namespace {
using Json = nlohmann::json;
std::string render_key(std::initializer_list<std::string_view> parts) {
    std::string key;
    for (const auto part : parts) {
        key += std::to_string(part.size());
        key += ':';
        key += part;
    }
    return key;
}
Json properties(const Json& entity, const std::string& name) {
    if (entity.contains("components")) {
        for (const auto& component : entity["components"])
            if (component.at("type") == "faset." + name) {
                // Future authoring schemas remain opaque until an explicit migration.
                if (component.value("version", 1) != 1)
                    return Json{};
                return component.at("fields");
            }
        return Json{};
    }
    // Runtime presentation snapshots already contain validated typed components.
    if (entity.contains(name))
        return entity.at(name);
    return Json{};
}
template <std::size_t N>
std::array<float, N> vec(const Json& value, const char* name, std::array<float, N> fallback) {
    if (value.is_null() || !value.contains(name))
        return fallback;
    auto result = value.at(name).get<std::array<float, N>>();
    for (float v : result)
        if (!std::isfinite(v))
            throw std::runtime_error("nonfinite scene vector");
    return result;
}
render::Vec3 point(const render::Mat4& m, render::Vec3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
render::Vec3 direction(const render::Mat4& m, render::Vec3 p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2], m[1] * p[0] + m[5] * p[1] + m[9] * p[2],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2]};
}
render::Vec3 normalized(render::Vec3 value, const std::string& entityId,
                        std::string_view field) {
    const auto length = std::hypot(value[0], value[1], value[2]);
    if (!std::isfinite(length) || length < 1e-6f)
        throw std::invalid_argument("Light on entity " + entityId + " has invalid " +
                                    std::string(field));
    for (auto& axis : value)
        axis /= length;
    return value;
}
std::pair<std::string, std::string> reference(const std::string& ref) {
    const auto hash = ref.find('#');
    return {ref.substr(0, hash), hash == std::string::npos ? std::string{} : ref.substr(hash + 1)};
}
std::shared_ptr<const render::Mesh> plane() {
    static const auto mesh = []() {
        auto out = std::make_shared<render::Mesh>();
        out->vertices = {{{-.5f, 0, -.5f}, {0, 1, 0}, {1, 1, 1, 1}, {0, 0}},
                         {{.5f, 0, -.5f}, {0, 1, 0}, {1, 1, 1, 1}, {1, 0}},
                         {{.5f, 0, .5f}, {0, 1, 0}, {1, 1, 1, 1}, {1, 1}},
                         {{-.5f, 0, .5f}, {0, 1, 0}, {1, 1, 1, 1}, {0, 1}}};
        out->indices = {0, 2, 1, 0, 3, 2};
        return out;
    }();
    return mesh;
}
} // namespace
struct SceneView::Impl {
    struct Bundle {
        assets::CookedAsset data;
        std::vector<std::vector<std::shared_ptr<const render::Mesh>>> meshes;
        std::vector<std::shared_ptr<const render::Texture>> textures;
    };
    assets::AssetStore pipeline;
    std::unordered_map<std::string, Bundle> bundles;
    std::vector<std::string> messages;
    explicit Impl(std::filesystem::path path) : pipeline(std::move(path)) {}
    Bundle& bundle(const std::string& id) {
        if (auto it = bundles.find(id); it != bundles.end())
            return it->second;
        Bundle result;
        result.data = pipeline.load_asset(id);
        for (const auto& mesh : result.data.meshes) {
            auto& primitives = result.meshes.emplace_back();
            for (const auto& primitive : mesh.primitives) {
                auto converted = std::make_shared<render::Mesh>();
                converted->indices = primitive.indices;
                converted->vertices.reserve(primitive.vertices.size());
                for (const auto& vertex : primitive.vertices)
                    converted->vertices.push_back(
                        {vertex.position, vertex.normal, {1, 1, 1, 1}, vertex.uv});
                primitives.push_back(std::move(converted));
            }
        }
        for (const auto& texture : result.data.textures) {
            std::shared_ptr<render::Texture> converted;
#if defined(FASET_HAS_STB)
            if (texture.bytes.size() > std::size_t(std::numeric_limits<int>::max()))
                throw std::runtime_error("Encoded texture exceeds decoder limit");
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(reinterpret_cast<const unsigned char*>(texture.bytes.data()),
                                       static_cast<int>(texture.bytes.size()), &width, &height,
                                       &channels))
                throw std::runtime_error("Cannot read texture dimensions: " + texture.id);
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
                std::uint64_t(width) * std::uint64_t(height) > 64 * 1024 * 1024)
                throw std::runtime_error("Texture exceeds decoder image limits");
            auto pixels = stbi_load_from_memory(
                reinterpret_cast<const unsigned char*>(texture.bytes.data()),
                static_cast<int>(texture.bytes.size()), &width, &height, &channels, 4);
            if (!pixels)
                throw std::runtime_error("Cannot decode texture " + texture.id + ": " +
                                         std::string(stbi_failure_reason() ? stbi_failure_reason()
                                                                           : "unsupported image"));
            std::unique_ptr<unsigned char, decltype(&stbi_image_free)> guard(pixels,
                                                                             &stbi_image_free);
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
                throw std::runtime_error("Texture exceeds 16384 dimension limit");
            converted = std::make_shared<render::Texture>();
            converted->width = width;
            converted->height = height;
            converted->srgb = true;
            converted->rgba.assign(pixels, pixels + std::size_t(width) * std::size_t(height) * 4);
#else
            throw std::runtime_error("Image decoding was not enabled for this Player build");
#endif
            result.textures.push_back(std::move(converted));
        }
        return bundles.emplace(id, std::move(result)).first->second;
    }
    std::shared_ptr<const render::Texture> texture(const std::string& ref) {
        auto [id, selector] = reference(ref);
        auto& asset = bundle(id);
        if (asset.textures.empty())
            throw std::runtime_error("Asset has no texture: " + ref);
        if (selector.empty())
            return asset.textures.front();
        for (std::size_t i = 0; i < asset.data.textures.size(); ++i)
            if (asset.data.textures[i].id == selector)
                return asset.textures[i];
        throw std::runtime_error("Texture subasset does not exist: " + ref);
    }
    void imported(render::Snapshot& out, const std::string& ref, const render::Mat4& model,
                  render::Color tint, std::string_view entity_id) {
        const auto [id, selector] = reference(ref);
        auto& asset = bundle(id);
        auto emit = [&](std::size_t meshIndex, const render::Mat4& local,
                        std::string_view node_id) {
            if (meshIndex >= asset.meshes.size())
                throw std::runtime_error("Invalid cooked mesh index");
            for (std::size_t p = 0; p < asset.meshes[meshIndex].size(); ++p) {
                render::DrawItem draw;
                draw.mesh = asset.meshes[meshIndex][p];
                draw.model = render::multiply(model, local);
                draw.color = tint;
                const auto primitive_id = std::to_string(p);
                draw.instance_key = render_key(
                    {entity_id, "asset", ref, node_id, asset.data.meshes[meshIndex].id,
                     primitive_id});
                const auto material = asset.data.meshes[meshIndex].primitives[p].material;
                if (material >= 0) {
                    if (std::size_t(material) >= asset.data.materials.size())
                        throw std::runtime_error("Invalid cooked material index");
                    const auto& m = asset.data.materials[material];
                    for (int i = 0; i < 4; ++i)
                        draw.color[i] *= m.base_color[i];
                    draw.roughness = m.roughness;
                    draw.metallic = m.metallic;
                    if (m.base_color_texture >= 0) {
                        if (std::size_t(m.base_color_texture) >= asset.textures.size())
                            throw std::runtime_error("Invalid base-color texture index");
                        draw.texture = asset.textures[m.base_color_texture];
                    }
                    if (m.normal_texture >= 0 || m.metallic_roughness_texture >= 0 ||
                        m.alpha_mode != "OPAQUE" || m.unlit || m.double_sided)
                        messages.push_back(
                            "warning: material " + m.id +
                            " has features beyond the initial base-color/PBR renderer");
                }
                out.draws.push_back(std::move(draw));
            }
        };
        if (!selector.empty())
            for (std::size_t i = 0; i < asset.data.meshes.size(); ++i)
                if (asset.data.meshes[i].id == selector) {
                    emit(i, render::identity, "direct-mesh");
                    return;
                }
        std::unordered_map<std::string, const assets::Node*> nodes;
        for (const auto& node : asset.data.nodes)
            nodes.emplace(node.id, &node);
        if (!selector.empty() && !nodes.contains(selector))
            throw std::runtime_error("Node/mesh subasset does not exist: " + ref);
        std::unordered_map<std::string, render::Mat4> matrices;
        std::set<std::string> active;
        auto world = [&](auto&& self, const assets::Node& node) -> render::Mat4 {
            if (auto it = matrices.find(node.id); it != matrices.end())
                return it->second;
            if (!active.insert(node.id).second)
                throw std::runtime_error("Cyclic cooked node hierarchy");
            auto matrix = node.local_transform;
            if (!node.parent_id.empty()) {
                auto parent = nodes.find(node.parent_id);
                if (parent == nodes.end())
                    throw std::runtime_error("Missing cooked parent node");
                matrix = render::multiply(self(self, *parent->second), matrix);
            }
            active.erase(node.id);
            return matrices.emplace(node.id, matrix).first->second;
        };
        if (asset.data.nodes.empty())
            for (std::size_t i = 0; i < asset.meshes.size(); ++i)
                emit(i, render::identity, "unparented-mesh");
        for (const auto& node : asset.data.nodes)
            if (node.mesh >= 0) {
                bool selected = selector.empty();
                auto current = &node;
                std::set<std::string> seen;
                while (!selected && current && seen.insert(current->id).second) {
                    selected = current->id == selector;
                    auto parent = nodes.find(current->parent_id);
                    current = parent == nodes.end() ? nullptr : parent->second;
                }
                if (selected)
                    emit(static_cast<std::size_t>(node.mesh), world(world, node), node.id);
            }
    }
};
SceneView::SceneView(std::filesystem::path cacheRoot)
    : impl_(std::make_unique<Impl>(std::move(cacheRoot))) {}
SceneView::~SceneView() = default;
void SceneView::clearCache() {
    impl_->bundles.clear();
}
const std::vector<std::string>& SceneView::diagnostics() const {
    return impl_->messages;
}
render::Snapshot SceneView::build(const Json& scene, float aspect, CameraSettings camera) {
    if (!std::isfinite(aspect) || aspect <= 0)
        throw std::invalid_argument("Viewport aspect must be positive");
    impl_->messages.clear();
    render::Snapshot out;
    const auto scene_id = scene.value("id", std::string{});
    std::string camera_id = camera.overrideSceneCamera ? "override" : "default";
    const auto& entities = scene.at("entities");
    if (!entities.is_array())
        throw std::invalid_argument("Scene entities must be an array");
    std::unordered_map<std::string, const Json*> byId;
    for (const auto& entity : entities) {
        const auto id = entity.at("id").get<std::string>();
        if (!byId.emplace(id, &entity).second)
            throw std::invalid_argument("Duplicate scene ID");
        if (!entity.contains("components") && entity.contains("light"))
            out.authored_lights_present = true;
        for (const auto& component : entity.value("components", Json::array())) {
            const auto type = component.at("type").get<std::string>();
            if (type == "faset.light")
                out.authored_lights_present = true;
            if (component.value("version", 1) != 1 &&
                (type == "faset.transform" || type == "faset.sprite" || type == "faset.mesh" ||
                 type == "faset.camera" || type == "faset.light"))
                impl_->messages.push_back("warning: preview skips unsupported " + type +
                                          " version " +
                                          std::to_string(component.at("version").get<int>()) +
                                          " on entity " + id + "; opaque data is preserved");
        }
    }
    std::unordered_map<std::string, render::Mat4> matrices;
    std::set<std::string> active;
    auto world = [&](auto&& self, const Json& entity) -> render::Mat4 {
        auto id = entity.at("id").get<std::string>();
        if (auto it = matrices.find(id); it != matrices.end())
            return it->second;
        if (!active.insert(id).second)
            throw std::invalid_argument("Cyclic scene hierarchy");
        const auto fields = properties(entity, "transform");
        auto matrix = render::transform(vec<3>(fields, "position", {0, 0, 0}),
                                        vec<3>(fields, "rotation", {0, 0, 0}),
                                        vec<3>(fields, "scale", {1, 1, 1}));
        if (entity.contains("parent") && !entity["parent"].is_null()) {
            auto parent = byId.find(entity["parent"].get<std::string>());
            if (parent == byId.end())
                throw std::invalid_argument("Missing scene parent");
            matrix = render::multiply(self(self, *parent->second), matrix);
        }
        active.erase(id);
        return matrices.emplace(id, matrix).first->second;
    };
    const int dimension = scene.value("dimension", 3);
    if (dimension != 2 && dimension != 3)
        throw std::invalid_argument("Scene dimension must be 2 or 3");
    render::Vec3 cameraUp{0, 1, 0};
    bool foundCamera = false;
    std::vector<std::pair<int, render::Sprite>> sprites;
    std::vector<render::SunLight> directionalLights;
    for (const auto& entity : entities) {
        const auto id = entity.at("id").get<std::string>();
        render::Mat4 model;
        try {
            model = world(world, entity);
        } catch (const std::exception& error) {
            throw std::invalid_argument("Entity " + id + " transform: " + error.what());
        }
        if (auto fields = properties(entity, "camera");
            !fields.is_null() && !camera.overrideSceneCamera && !foundCamera) {
            camera.eye = point(model, {0, 0, 0});
            camera.target = point(model, {0, 0, -1});
            cameraUp = direction(model, {0, 1, 0});
            camera.verticalFovDegrees = fields.value("fov", 60.0f);
            camera.nearPlane = fields.value("near", 0.1f);
            camera.farPlane = fields.value("far", 1000.0f);
            foundCamera = true;
            camera_id = entity.at("id").get<std::string>();
        }
        if (auto fields = properties(entity, "light"); !fields.is_null()) {
            auto invalid = [&](std::string_view field) -> void {
                throw std::invalid_argument("Light on entity " + id + " has invalid " +
                                            std::string(field));
            };
            for (const auto entry : model)
                if (!std::isfinite(entry))
                    invalid("transform");
            auto number = [&](const char* field, float fallback) {
                if (!fields.contains(field))
                    return fallback;
                const auto& value = fields.at(field);
                if (!value.is_number())
                    invalid(field);
                const auto decimal = value.get<double>();
                if (!std::isfinite(decimal) ||
                    std::abs(decimal) > std::numeric_limits<float>::max())
                    invalid(field);
                return static_cast<float>(decimal);
            };
            auto boolean = [&](const char* field, bool fallback) {
                if (!fields.contains(field))
                    return fallback;
                if (!fields.at(field).is_boolean())
                    invalid(field);
                return fields.at(field).get<bool>();
            };
            const auto enabled = boolean("enabled", true);
            if (enabled) {
                if (fields.contains("kind") && !fields.at("kind").is_string())
                    invalid("kind");
                const auto kind = fields.value("kind", std::string("directional"));
                if (kind != "directional" && kind != "point" && kind != "spot")
                    invalid("kind");
                render::Color color;
                try {
                    color = vec<4>(fields, "color", {1, 1, 1, 1});
                } catch (const std::exception&) {
                    invalid("color");
                }
                for (const auto channel : color)
                    if (channel < 0)
                        invalid("color");
                const auto intensity = number("intensity", 1);
                if (intensity < 0)
                    invalid("intensity");
                const auto castsShadow = boolean("casts_shadow", true);
                const auto stableId = id;
                if (kind == "directional") {
                    directionalLights.push_back({stableId,
                                                 normalized(direction(model, {-0.5f, -1, -0.3f}), id,
                                                            "direction"),
                                                 color, intensity, castsShadow});
                } else {
                    render::LocalLight local;
                    local.kind = kind == "point" ? render::LocalLight::Kind::Point
                                                 : render::LocalLight::Kind::Spot;
                    local.stable_id = stableId;
                    local.position = point(model, {0, 0, 0});
                    for (const auto coordinate : local.position)
                        if (!std::isfinite(coordinate))
                            invalid("position");
                    local.direction = normalized(direction(model, {0, 0, -1}), id, "direction");
                    local.color = color;
                    local.intensity = intensity;
                    local.range = number("range", 10);
                    if (local.range <= 0)
                        invalid("range");
                    local.inner_angle = number("inner_angle", 0.35f);
                    local.outer_angle = number("outer_angle", 0.7f);
                    if (local.inner_angle < 0 || local.inner_angle > local.outer_angle ||
                        local.outer_angle <= 0 || local.outer_angle >= std::numbers::pi_v<float> / 2)
                        invalid("inner_angle/outer_angle");
                    local.casts_shadow = castsShadow;
                    if (fields.contains("shadow_priority")) {
                        if (!fields.at("shadow_priority").is_number_integer())
                            invalid("shadow_priority");
                        local.shadow_priority = fields.at("shadow_priority").get<int>();
                    }
                    out.local_lights.push_back(std::move(local));
                }
            }
        }
        if (auto fields = properties(entity, "sprite"); !fields.is_null()) {
            render::Sprite sprite;
            sprite.layer = fields.value("layer", 0);
            sprite.position = point(model, {0, 0, 0});
            auto size = vec<2>(fields, "size", {1, 1});
            float sx = std::hypot(model[0], model[1]), sy = std::hypot(model[4], model[5]);
            sprite.size = {size[0] * sx, size[1] * sy};
            sprite.rotation = std::atan2(model[1], model[0]);
            sprite.color = vec<4>(fields, "color", {1, 1, 1, 1});
            if (model[0] * model[5] - model[1] * model[4] < 0)
                sprite.size[1] = -sprite.size[1];
            if (sx > 0 && sy > 0 &&
                std::abs((model[0] * model[4] + model[1] * model[5]) / (sx * sy)) > 0.0001f)
                impl_->messages.push_back("warning: sprite hierarchy shear is approximated");
            auto texture = fields.value("texture", std::string{});
            if (!texture.empty())
                try {
                    sprite.texture = impl_->texture(texture);
                } catch (const std::exception& e) {
                    impl_->messages.push_back("error: " + std::string(e.what()));
                    sprite.color = {1, 0, 1, 1};
                }
            sprites.emplace_back(fields.value("layer", 0), std::move(sprite));
        }
        if (auto fields = properties(entity, "mesh"); !fields.is_null()) {
            const auto tint = vec<4>(fields, "color", {1, 1, 1, 1});
            const auto asset = fields.value("asset", std::string{});
            if (asset.empty() || asset.starts_with("builtin:")) {
                const auto primitive = asset.empty()
                                           ? fields.value("primitive", std::string("cube"))
                                           : asset.substr(8);
                if (primitive != "plane" && primitive != "cube")
                    throw std::invalid_argument("Unsupported builtin mesh: " + primitive);
                render::DrawItem draw{primitive == "plane" ? plane() : render::cube_mesh(),
                                      model, tint, 0.65f, 0.0f, true, {}};
                const auto entity_id = entity.at("id").get<std::string>();
                draw.instance_key = render_key({entity_id, "builtin", primitive});
                out.draws.push_back(std::move(draw));
            } else
                try {
                    impl_->imported(out, asset, model, tint,
                                    entity.at("id").get<std::string>());
                } catch (const std::exception& e) {
                    impl_->messages.push_back("error: " + std::string(e.what()));
                    render::DrawItem draw{render::cube_mesh(), model, {1, 0, 1, 1},
                                          0.65f, 0.0f, true, {}};
                    const auto entity_id = entity.at("id").get<std::string>();
                    draw.instance_key = render_key({entity_id, "error", asset});
                    out.draws.push_back(std::move(draw));
                }
        }
    }
    std::stable_sort(sprites.begin(), sprites.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& pair : sprites)
        out.sprites.push_back(std::move(pair.second));
    std::sort(directionalLights.begin(), directionalLights.end(),
              [](const auto& a, const auto& b) { return a.stable_id < b.stable_id; });
    std::sort(out.local_lights.begin(), out.local_lights.end(),
              [](const auto& a, const auto& b) { return a.stable_id < b.stable_id; });
    if (!directionalLights.empty()) {
        out.sun = directionalLights.front();
        out.light_direction = out.sun->direction;
        if (directionalLights.size() > 1)
            impl_->messages.push_back("warning: multiple enabled directional lights; using " +
                                      out.sun->stable_id + " and ignoring " +
                                      std::to_string(directionalLights.size() - 1) + " others");
    }
    if (dimension == 2) {
        const float height = camera.orthographicHeight;
        if (!std::isfinite(height) || height <= 0)
            throw std::invalid_argument("Orthographic height must be positive");
        const auto center = foundCamera ? camera.eye : camera.target;
        out.projection = render::orthographic(-height * aspect / 2, height * aspect / 2,
                                              -height / 2, height / 2, -100, 100);
        out.view_projection = render::multiply(
            out.projection, render::transform({-center[0], -center[1], 0}));
        out.eye = {center[0], center[1], 10};
    } else {
        if (!std::isfinite(camera.verticalFovDegrees) || camera.verticalFovDegrees <= 0 ||
            camera.verticalFovDegrees >= 179)
            throw std::invalid_argument("Camera FOV out of range");
        if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane) ||
            camera.nearPlane <= 0 || camera.farPlane <= camera.nearPlane)
            throw std::invalid_argument("Camera depth range is invalid");
        float distance = 0;
        render::Vec3 delta{};
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(camera.eye[i]) || !std::isfinite(camera.target[i]) ||
                !std::isfinite(cameraUp[i]))
                throw std::invalid_argument("Camera basis must be finite");
            delta[i] = camera.target[i] - camera.eye[i];
            distance += delta[i] * delta[i];
        }
        const render::Vec3 cross{delta[1] * cameraUp[2] - delta[2] * cameraUp[1],
                                 delta[2] * cameraUp[0] - delta[0] * cameraUp[2],
                                 delta[0] * cameraUp[1] - delta[1] * cameraUp[0]};
        if (distance < 1e-10f ||
            cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] < 1e-10f)
            throw std::invalid_argument("Camera basis is degenerate");
        out.eye = camera.eye;
        out.projection = render::perspective(
            camera.verticalFovDegrees * std::numbers::pi_v<float> / 180, aspect,
            camera.nearPlane, camera.farPlane);
        const auto view = render::look_at(camera.eye, camera.target, cameraUp);
        out.view_projection = render::multiply(out.projection, view);
        out.camera_frustum = render::CameraFrustum{view, out.projection, camera.nearPlane,
                                                   camera.farPlane, true};
    }
    std::sort(impl_->messages.begin(), impl_->messages.end());
    impl_->messages.erase(std::unique(impl_->messages.begin(), impl_->messages.end()),
                          impl_->messages.end());
    out.view_id = render_key({scene_id, "camera", camera_id});
    return out;
}
void SceneView::appendPhysicsDebug(render::Snapshot& snapshot, const Json& scene,
                                   float thickness) const {
    if (!std::isfinite(thickness) || thickness <= 0)
        throw std::invalid_argument("Physics debug thickness must be positive");
    const int dimension = scene.at("dimension");
    if (dimension != 2 && dimension != 3)
        throw std::invalid_argument("Physics debug dimension must be 2 or 3");
    static const auto edgeMesh = [] {
        auto mesh = std::make_shared<render::Mesh>(*render::cube_mesh());
        for (auto& vertex : mesh->vertices)
            vertex.normal = {0, 0, 0}; // Unlit debug color.
        return mesh;
    }();
    for (const auto& entity : scene.at("entities")) {
        const auto body = properties(entity, dimension == 2 ? "rigid_body_2d" : "rigid_body_3d");
        if (body.is_null())
            continue;
        if (entity.contains("parent") && !entity.at("parent").is_null())
            throw std::invalid_argument(
                "Physics debug bodies must be roots, like the runtime adapters");
        const auto pose = properties(entity, "transform");
        const auto position = vec<3>(pose, "position", {0, 0, 0});
        const auto rotation = vec<3>(pose, "rotation", {0, 0, 0});
        const auto scale = vec<3>(pose, "scale", {1, 1, 1});
        if (dimension == 2 && (rotation[0] != 0 || rotation[1] != 0))
            throw std::invalid_argument("2D physics debug rotates only around Z");
        render::Vec3 half{};
        if (dimension == 2) {
            const auto value = vec<2>(body, "half_extents", {.5f, .5f});
            half = {value[0], value[1], 0};
        } else
            half = vec<3>(body, "half_extents", {.5f, .5f, .5f});
        for (int axis = 0; axis < dimension; ++axis) {
            if (half[axis] <= 0 || std::abs(scale[axis]) <= .00001f)
                throw std::invalid_argument("Invalid physics debug box extent/scale");
            half[axis] *= std::abs(scale[axis]);
            if (!std::isfinite(half[axis]))
                throw std::invalid_argument("Nonfinite physics debug box");
        }
        const auto type = body.value("body_type", std::string("dynamic"));
        const render::Color color = type == "static"      ? render::Color{.2f, 1, .3f, 1}
                                    : type == "kinematic" ? render::Color{1, .7f, .15f, 1}
                                                          : render::Color{.15f, .85f, 1, 1};
        const auto model = render::transform(position, rotation);
        auto edge = [&](render::Vec3 center, render::Vec3 size) {
            snapshot.draws.push_back({edgeMesh,
                                      render::multiply(model, render::transform(center, {}, size)),
                                      color,
                                      .65f,
                                      0,
                                      false,
                                      {}});
        };
        if (dimension == 2) {
            for (float side : {-1.0f, 1.0f}) {
                edge({0, side * half[1], 0}, {2 * half[0], thickness, thickness});
                edge({side * half[0], 0, 0}, {thickness, 2 * half[1], thickness});
            }
        } else {
            for (int axis = 0; axis < 3; ++axis) {
                const int first = (axis + 1) % 3, second = (axis + 2) % 3;
                for (float a : {-1.0f, 1.0f})
                    for (float b : {-1.0f, 1.0f}) {
                        render::Vec3 center{}, size{thickness, thickness, thickness};
                        center[first] = a * half[first];
                        center[second] = b * half[second];
                        size[axis] = 2 * half[axis];
                        edge(center, size);
                    }
            }
        }
    }
}
} // namespace faset::player
