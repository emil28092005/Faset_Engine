#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace faset::render {
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Color = std::array<float, 4>;
using Mat4 = std::array<float, 16>;
inline constexpr Mat4 identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
// Matrices are column-major, vectors are columns; clip depth is Vulkan's [0,1].
Mat4 multiply(const Mat4&, const Mat4&);
Mat4 transform(Vec3 position, Vec3 rotation = {}, Vec3 scale = {1, 1, 1});
Mat4 perspective(float vertical_fov_radians, float aspect, float near_plane, float far_plane);
Mat4 orthographic(float left, float right, float bottom, float top, float near_plane,
                  float far_plane);
Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = {0, 1, 0});
struct Vertex {
    Vec3 position{};
    Vec3 normal{0, 0, 1};
    Color color{1, 1, 1, 1};
    Vec2 uv{};
};
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};
std::shared_ptr<const Mesh> cube_mesh();
struct Texture;
struct DrawItem {
    std::shared_ptr<const Mesh> mesh{};
    Mat4 model{identity};
    Color color{1, 1, 1, 1};
    float roughness{0.65f};
    float metallic{0.0f};
    bool cast_shadow{true};
    std::shared_ptr<const Texture> texture{};
    // Empty means an ad-hoc draw without temporal visibility state. Scene extraction
    // derives this from persistent object and primitive identities, not draw order.
    std::string instance_key{};
    // Optional prepared coarser meshes: element zero is LOD 1; mesh is LOD 0.
    std::vector<std::shared_ptr<const Mesh>> lod_meshes{};
};
struct Sprite {
    Vec3 position{};
    Vec2 size{1, 1};
    Color color{1, 1, 1, 1};
    float rotation{};
    std::shared_ptr<const Texture> texture{};
    // Higher layers draw later. Within a layer, projected depth is sorted back to front;
    // equal-depth sprites retain their submission order.
    int layer{};
};
struct Texture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
    std::uint64_t revision{};
    bool srgb{false};
};
struct Quad {
    float x{}, y{}, width{}, height{};
    Color color{1, 1, 1, 1};
    std::shared_ptr<const Texture> texture{};
    std::array<float, 4> uv_rect{0, 0, 1, 1};
};
struct UiTriangles {
    // Non-indexed triangle list in drawable pixels, rendered after UI quads/text.
    std::vector<Vertex> vertices;
    std::shared_ptr<const Texture> texture{};
    std::array<float, 4> clip_rect{}; // x/y/width/height; zero size uses the full target
};
struct Text {
    float x{}, y{};
    std::string value;
    Color color{0.85f, 0.87f, 0.90f, 1};
    float size{14};
};
struct SunLight {
    std::string stable_id;
    Vec3 direction{-0.5f, -1, -0.3f};
    Color color{1, 1, 1, 1};
    float intensity{1};
    bool casts_shadow{true};
};
struct LocalLight {
    enum class Kind { Point, Spot };
    Kind kind{Kind::Point};
    std::string stable_id;
    Vec3 position{};
    Vec3 direction{0, 0, -1};
    Color color{1, 1, 1, 1};
    float intensity{1};
    float range{10};
    float inner_angle{0.35f};
    float outer_angle{0.7f};
    bool casts_shadow{true};
    int shadow_priority{};
};
struct CameraFrustum {
    Mat4 view{identity};
    Mat4 projection{identity};
    float near_plane{0.1f};
    float far_plane{1000};
    bool perspective{true};
};
struct Snapshot {
    // Optional scene viewport in drawable pixels (x, y, width, height); zero size uses the full
    // target.
    std::array<float, 4> scene_rect{};
    Mat4 view_projection{identity};
    // Supplying the projection separately lets temporal visibility invalidate
    // history on FOV/near/far changes without invalidating normal camera motion.
    Mat4 projection{identity};
    Vec3 eye{4, 3, 5};
    Vec3 light_direction{-0.5f, -1, -0.3f};
    Color clear_color{0.055f, 0.065f, 0.085f, 1};
    std::vector<DrawItem> draws;
    std::vector<Sprite> sprites;
    // UI coordinates are drawable pixels, top-left origin. Order is preserved per list.
    std::vector<Quad> ui_quads;
    std::vector<Text> ui_text;
    std::vector<UiTriangles> ui_triangles;
    // Distinguishes temporal histories when one Renderer displays different views.
    std::string view_id{};
    bool camera_cut{};
    // Empty legacy scenes may use the renderer's compatibility sun. Any authored light
    // component, including an explicitly disabled or opaque future one, suppresses it.
    bool authored_lights_present{};
    std::optional<SunLight> sun{};
    std::vector<LocalLight> local_lights;
    std::optional<CameraFrustum> camera_frustum{};
};
enum class VisibilityMode { Direct, GpuFrustum, GpuOcclusion };
// CPU-only validation used before publishing a game or creating Vulkan pipelines.
void validate_shader_bundle(const std::filesystem::path& directory);
void validate_gpu_shader_bundle(const std::filesystem::path& directory);

struct RendererConfig {
    std::uint32_t width{1280}, height{720};
    std::string title{"Faset Engine"};
    bool headless{false};
    bool validation{true};
    VisibilityMode visibility_mode{VisibilityMode::Direct};
    // GPU counter readback is diagnostic-only; normal visibility uses no CPU feedback.
    bool visibility_diagnostics{false};
    // Optional isolated shader bundle, useful for editor preview and shader reload tests.
    std::filesystem::path shader_directory{};
};
struct Event {
    enum class Type {
        Quit,
        Resize,
        FocusGained,
        FocusLost,
        MouseMove,
        MouseDown,
        MouseUp,
        Wheel,
        KeyDown,
        KeyUp,
        TextInput,
        TextEditing
    };
    Type type{};
    float x{}, y{};
    int button{};
    std::string key;
    std::string text;
    bool control{}, shift{}, alt{}, repeat{};
    int edit_start{}, edit_length{};
};
struct FrameStats {
    std::uint64_t frame{};
    bool validation_enabled{};
    bool gpu_labels_enabled{};
    std::uint32_t gpu_label_count{};
    // Live VkDeviceMemory allocation sizes, including alignment; excludes driver internals.
    std::uint64_t gpu_allocated_bytes{};
    std::uint32_t texture_count{};
    std::uint32_t vertices{}, draw_calls{}, culled_meshes{}, validation_errors{};
    bool gpu_visibility_active{}, hzb_valid{};
    // Requested and actual paths for the last frame; actual may be less capable.
    VisibilityMode requested_visibility_mode{VisibilityMode::Direct};
    VisibilityMode effective_visibility_mode{VisibilityMode::Direct};
    bool visibility_counters_valid{};
    std::uint32_t gpu_bins{}, gpu_visible_instances{}, gpu_frustum_rejected{};
    std::uint32_t gpu_occlusion_deferred{}, gpu_post_visible{};
    std::array<std::uint32_t, 4> lod_counts{};
    double cpu_ms{}, gpu_ms{}, readback_cpu_ms{};
    double gpu_main_cull_ms{}, gpu_main_raster_ms{}, gpu_hzb_ms{};
    double gpu_post_cull_ms{}, gpu_post_raster_ms{};
    std::string device;
};
struct HzbDebugImage {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
class Renderer {
  public:
    explicit Renderer(const RendererConfig& = {});
    ~Renderer();
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    std::vector<Event> poll_events();
    void render(const Snapshot&);
    void resize(std::uint32_t width, std::uint32_t height);
    void set_visibility_mode(VisibilityMode);
    VisibilityMode visibility_mode() const;
    void set_visibility_diagnostics(bool enabled);
    // Reads the most recently completed HZB mip for editor diagnostics only.
    // Normal visibility decisions remain entirely on the GPU.
    std::optional<HzbDebugImage> hzb_debug_image(std::uint32_t mip = 0);
    // Rebuilds graphics pipelines from SPIR-V; a failure preserves the current pipelines.
    bool reload_shaders(std::string& error);
    // Saves the latest completed frame as a portable RGB PPM image.
    void capture(const std::filesystem::path&);
    std::vector<std::uint8_t> pixels() const;
    std::uint32_t width() const;
    std::uint32_t height() const;
    // Effective UI scale in drawable pixels; updates when the window changes display.
    float display_scale() const;
    bool should_close() const;
    const FrameStats& stats() const;
    void set_title(const std::string&);
    void set_text_input(bool enabled);
    void set_text_input_area(float x, float y, float width, float height);
    void set_clipboard(const std::string&);
    std::string clipboard() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace faset::render
