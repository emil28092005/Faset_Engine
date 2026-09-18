#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace faset::render {
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Color = std::array<float, 4>;
using Mat4 = std::array<float, 16>;
inline constexpr Mat4 identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
// Matrices are column-major, vectors are columns; clip depth is Vulkan's [0,1].
Mat4 multiply(const Mat4&, const Mat4&);
Mat4 transform(Vec3 position, Vec3 rotation = {}, Vec3 scale = {1,1,1});
Mat4 perspective(float vertical_fov_radians, float aspect, float near_plane, float far_plane);
Mat4 orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane);
Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = {0,1,0});
struct Vertex { Vec3 position{}; Vec3 normal{0,0,1}; Color color{1,1,1,1}; Vec2 uv{}; };
struct Mesh { std::vector<Vertex> vertices; std::vector<std::uint32_t> indices; };
std::shared_ptr<const Mesh> cube_mesh();
struct Texture;
struct DrawItem {
    std::shared_ptr<const Mesh> mesh;
    Mat4 model{identity};
    Color color{1,1,1,1};
    float roughness{0.65f};
    float metallic{0.0f};
    bool cast_shadow{true};
    std::shared_ptr<const Texture> texture;
};
struct Sprite { Vec3 position{}; Vec2 size{1,1}; Color color{1,1,1,1}; float rotation{}; std::shared_ptr<const Texture> texture; };
struct Texture { std::uint32_t width{}, height{}; std::vector<std::uint8_t> rgba; std::uint64_t revision{}; bool srgb{false}; };
struct Quad { float x{}, y{}, width{}, height{}; Color color{1,1,1,1}; std::shared_ptr<const Texture> texture; std::array<float,4> uv_rect{0,0,1,1}; };
struct Text { float x{}, y{}; std::string value; Color color{0.85f,0.87f,0.90f,1}; float size{14}; };
struct Snapshot {
    // Optional scene viewport in drawable pixels (x, y, width, height); zero size uses the full target.
    std::array<float,4> scene_rect{};
    Mat4 view_projection{identity};
    Vec3 eye{4,3,5};
    Vec3 light_direction{-0.5f,-1,-0.3f};
    Color clear_color{0.055f,0.065f,0.085f,1};
    std::vector<DrawItem> draws;
    std::vector<Sprite> sprites;
    // UI coordinates are drawable pixels, top-left origin. Order is preserved per list.
    std::vector<Quad> ui_quads;
    std::vector<Text> ui_text;
};
struct RendererConfig {
    std::uint32_t width{1280}, height{720};
    std::string title{"Faset Engine"};
    bool headless{false};
    bool validation{true};
};
struct Event {
    enum class Type { Quit, Resize, FocusGained, FocusLost, MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, TextInput, TextEditing };
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
    std::uint32_t vertices{}, draw_calls{}, culled_meshes{}, validation_errors{};
    double cpu_ms{}, gpu_ms{};
    std::string device;
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
    // Rebuilds graphics pipelines from SPIR-V; a failure preserves the current pipelines.
    bool reload_shaders(std::string& error);
    // Saves the latest completed frame as a portable RGB PPM image.
    void capture(const std::filesystem::path&);
    std::vector<std::uint8_t> pixels() const;
    std::uint32_t width() const;
    std::uint32_t height() const;
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
}
