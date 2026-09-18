#pragma once
#include <faset/render/renderer.hpp>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>

namespace faset::player {
struct CameraSettings {
    // Defaults frame the small demo scenes. An explicit editor camera overrides
    // scene camera components without changing the authoring document.
    bool overrideSceneCamera{false};
    render::Vec3 eye{8, 6, 10};
    render::Vec3 target{0, 0, 0};
    float verticalFovDegrees{60};
    float orthographicHeight{12};
    float nearPlane{0.1f}, farPlane{1000};
};
class SceneView {
  public:
    // cacheRoot contains assets/<AssetId>/current.json and generation folders.
    explicit SceneView(std::filesystem::path cacheRoot);
    ~SceneView();
    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;
    render::Snapshot build(const nlohmann::json& flatSceneOrRuntimeSnapshot, float aspect,
                           CameraSettings camera = {});
    void clearCache();
    const std::vector<std::string>& diagnostics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads JSON development scenes or a strict FASETSCN v1 CBOR envelope.
nlohmann::json readScene(const std::filesystem::path& path);
} // namespace faset::player
