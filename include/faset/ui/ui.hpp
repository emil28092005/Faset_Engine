#pragma once
#include <faset/render/renderer.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faset::ui {
using Json = nlohmann::json;
using Color = render::Color;
struct Rect {
    float x{}, y{}, width{}, height{};
    bool contains(float px, float py) const noexcept;
    Rect intersection(const Rect&) const noexcept;
};
struct Theme {
    Color background{.075f, .078f, .086f, 1}, surface{.10f, .105f, .115f, 1};
    Color raised{.135f, .14f, .15f, 1}, hover{.175f, .18f, .20f, 1}, border{.23f, .235f, .255f, 1};
    Color text{.86f, .875f, .90f, 1}, muted{.55f, .575f, .62f, 1}, accent{.65f, .60f, .88f, 1};
    Color selection{.26f, .245f, .35f, 1}, danger{.92f, .39f, .38f, 1};
    float font_size = 14, row_height = 28, padding = 8, gap = 4;
    static Theme from_json(const Json&);
    static Theme load(const std::filesystem::path&);
    Json to_json() const;
};

// Byte offsets always remain valid UTF-8 boundaries. Undo is local to an
// unfinished edit.
class TextBuffer {
  public:
    explicit TextBuffer(std::string text = {});
    const std::string& text() const noexcept {
        return text_;
    }
    std::size_t cursor() const noexcept {
        return cursor_;
    }
    std::size_t anchor() const noexcept {
        return anchor_;
    }
    bool has_selection() const noexcept {
        return cursor_ != anchor_;
    }
    std::string selected_text() const;
    void reset(std::string text);
    void set_cursor(std::size_t byte_offset, bool select = false);
    void select_all();
    void left(bool select = false, bool by_word = false);
    void right(bool select = false, bool by_word = false);
    void home(bool select = false);
    void end(bool select = false);
    bool insert(std::string_view utf8);
    bool backspace();
    bool delete_forward();
    bool undo();
    bool redo();
    static bool valid_utf8(std::string_view);

  private:
    struct State {
        std::string text;
        std::size_t cursor, anchor;
    };
    std::string text_;
    std::size_t cursor_{}, anchor_{};
    std::vector<State> undo_, redo_;
    void remember();
    void erase_selection();
};
class FontAtlas {
  public:
    explicit FontAtlas(const std::filesystem::path& font);
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    float measure(std::string_view utf8, float pixels);
    // y is the top of the line, not the baseline; clipping also adjusts glyph
    // UVs.
    void draw(render::Snapshot&, std::string_view utf8, float x, float y, float pixels, Color,
              const Rect& clip);
    std::shared_ptr<const render::Texture> texture() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class Kind {
    Panel,
    Row,
    Column,
    Label,
    Button,
    Tab,
    TreeRow,
    TextField,
    NumberField,
    Checkbox,
    Divider,
    Viewport
};
struct Layout {
    float width = -1, height = -1, flex = 0;
    float min_width = 0, min_height = 0, max_width = 100000, max_height = 100000;
    float padding = 0, gap = 4;
    bool absolute = false, scroll = false, clip = true;
    float x = 0, y = 0;
};
struct Widget {
    Kind kind = Kind::Panel;
    std::string id, text;
    Layout layout;
    Rect rect, clip;
    bool visible = true, enabled = true, selected = false, checked = false;
    double value = 0, step = .01;
    int precision = 3, indent = 0;
    float font_size = 0; // Zero inherits the theme typography.
    float scroll_y = 0, content_height = 0;
    std::string error, tooltip;
    Json drag_payload;
    std::string dock_area, dock_panel;
    std::function<void(Widget&)> on_click, on_preview, on_commit, on_cancel;
    std::function<void(Widget&, const Json&)> on_drop;
    std::vector<std::unique_ptr<Widget>> children;
    Widget* parent = nullptr;
    Widget& add(Kind, const std::string& stable_id, const std::string& text = {});
    Widget* find(std::string_view stable_id);
    const Widget* find(std::string_view stable_id) const;
    void remove(std::string_view stable_id);
};
// Small persistent docking model: named areas, ordered tabs/panels and explicit
// sizes.
class DockLayout {
  public:
    void move(const std::string& panel, const std::string& area, std::size_t index);
    std::vector<std::string> panels(const std::string& area) const;
    void set_size(const std::string& panel, float size);
    float size(const std::string& panel, float fallback) const;
    Json to_json() const;
    void from_json(const Json&);
    void save(const std::filesystem::path&) const;
    void load(const std::filesystem::path&);

  private:
    std::unordered_map<std::string, std::vector<std::string>> areas_;
    std::unordered_map<std::string, float> sizes_;
};

class Context {
  public:
    explicit Context(const std::filesystem::path& font);
    ~Context();
    Widget& root();
    Widget* find(std::string_view id);
    void set_theme(Theme);
    const Theme& theme() const;
    // Reloads declarative widget properties; matching IDs retain callbacks and
    // edit state.
    void validate_layout(const Json&) const;
    void apply_layout(const Json&);
    // Layout metrics are logical units; rectangles/events/IME areas are drawable
    // pixels. Scale also selects the glyph raster size (supported range .5–4).
    // Changing it preserves text/focus and cancels active pointer drags.
    void layout(float drawable_width, float drawable_height, float dpi_scale = 1);
    bool handle(const render::Event&);
    void draw(render::Snapshot&);
    bool update_text(const std::string& id, const std::string& value, bool force = false);
    bool update_number(const std::string& id, double value, bool force = false);
    bool focus(const std::string& id);
    const std::string& focused_id() const;
    void clear_focus(bool commit = true);
    bool editing() const;
    FontAtlas& font();
    void set_clipboard(std::function<std::string()> read,
                       std::function<void(const std::string&)> write);
    // Hook to Renderer::set_text_input and set_text_input_area.
    void set_ime(std::function<void(bool)> enabled, std::function<void(Rect)> rectangle);
    void set_docking(DockLayout*, std::function<void()> changed = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace faset::ui
