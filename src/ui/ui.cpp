#include <algorithm>
#include <chrono>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/ui/ui.hpp>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace faset::ui {
bool Rect::contains(float px, float py) const noexcept {
    return px >= x && py >= y && px < x + width && py < y + height;
}
Rect Rect::intersection(const Rect& b) const noexcept {
    const auto left = std::max(x, b.x), top = std::max(y, b.y);
    return {left, top, std::max(0.f, std::min(x + width, b.x + b.width) - left),
            std::max(0.f, std::min(y + height, b.y + b.height) - top)};
}
namespace {
Color color(const Json& j, const char* key, Color fallback) {
    if (!j.contains(key))
        return fallback;
    auto value = j.at(key).get<Color>();
    for (float c : value)
        if (!std::isfinite(c) || c < 0 || c > 1)
            throw std::runtime_error("Theme colors require normalized RGBA");
    return value;
}
float positive(const Json& j, const char* key, float fallback) {
    float value = j.value(key, fallback);
    if (!std::isfinite(value) || value <= 0 || value > 256)
        throw std::runtime_error("Invalid theme metric");
    return value;
}
std::string number(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(std::clamp(precision, 0, 9)) << value;
    return out.str();
}
bool field(const Widget& w) {
    return w.kind == Kind::TextField || w.kind == Kind::NumberField;
}
bool focusable(const Widget& w) {
    for (auto* parent = w.parent; parent; parent = parent->parent)
        if (!parent->visible || !parent->enabled)
            return false;
    return w.visible && w.enabled &&
           (field(w) || w.kind == Kind::Button || w.kind == Kind::Checkbox || w.kind == Kind::Tab ||
            w.kind == Kind::TreeRow);
}
void collect_focus(Widget& w, std::vector<std::string>& ids) {
    if (!w.visible)
        return;
    if (focusable(w))
        ids.push_back(w.id);
    for (auto& child : w.children)
        collect_focus(*child, ids);
}
void fill(render::Snapshot& frame, Rect rect, Color color, const Rect& clip) {
    rect = rect.intersection(clip);
    if (rect.width > 0 && rect.height > 0)
        frame.ui_quads.push_back(
            {rect.x, rect.y, rect.width, rect.height, color, {}, {0, 0, 1, 1}});
}
void outline(render::Snapshot& f, const Rect& r, Color c, const Rect& clip, float thickness = 1) {
    fill(f, {r.x, r.y, r.width, thickness}, c, clip);
    fill(f, {r.x, r.y + r.height - thickness, r.width, thickness}, c, clip);
    fill(f, {r.x, r.y, thickness, r.height}, c, clip);
    fill(f, {r.x + r.width - thickness, r.y, thickness, r.height}, c, clip);
}
Kind kind_from_string(const std::string& name) {
    static const std::unordered_map<std::string, Kind> kinds = {{"panel", Kind::Panel},
                                                                {"row", Kind::Row},
                                                                {"column", Kind::Column},
                                                                {"label", Kind::Label},
                                                                {"button", Kind::Button},
                                                                {"tab", Kind::Tab},
                                                                {"tree_row", Kind::TreeRow},
                                                                {"text_field", Kind::TextField},
                                                                {"number_field", Kind::NumberField},
                                                                {"checkbox", Kind::Checkbox},
                                                                {"divider", Kind::Divider},
                                                                {"viewport", Kind::Viewport}};
    auto found = kinds.find(name);
    if (found == kinds.end())
        throw std::runtime_error("Unknown widget kind: " + name);
    return found->second;
}
Layout parse_layout(const Json& j, Layout l = {}) {
    if (!j.is_object())
        throw std::runtime_error("Layout must be an object");
#define UI_FLOAT(name)                                                                             \
    l.name = j.value(#name, l.name);                                                               \
    if (!std::isfinite(l.name))                                                                    \
    throw std::runtime_error("Non-finite layout metric: " #name)
    UI_FLOAT(width);
    UI_FLOAT(height);
    UI_FLOAT(flex);
    UI_FLOAT(min_width);
    UI_FLOAT(min_height);
    UI_FLOAT(max_width);
    UI_FLOAT(max_height);
    UI_FLOAT(padding);
    UI_FLOAT(gap);
    UI_FLOAT(x);
    UI_FLOAT(y);
#undef UI_FLOAT
    l.absolute = j.value("absolute", l.absolute);
    l.scroll = j.value("scroll", l.scroll);
    l.clip = j.value("clip", l.clip);
    l.stack_vertical = j.value("stack_vertical", l.stack_vertical);
    if (l.flex < 0 || l.padding < 0 || l.gap < 0 || l.min_width < 0 || l.min_height < 0 ||
        l.max_width < l.min_width || l.max_height < l.min_height)
        throw std::runtime_error("Invalid layout constraints");
    return l;
}
} // namespace
Theme Theme::from_json(const Json& j) {
    if (!j.is_object())
        throw std::runtime_error("Theme must be an object");
    Theme t;
#define UI_COLOR(name) t.name = color(j, #name, t.name)
    UI_COLOR(background);
    UI_COLOR(surface);
    UI_COLOR(raised);
    UI_COLOR(hover);
    UI_COLOR(border);
    UI_COLOR(text);
    UI_COLOR(muted);
    UI_COLOR(accent);
    UI_COLOR(selection);
    UI_COLOR(danger);
#undef UI_COLOR
    t.font_size = positive(j, "font_size", t.font_size);
    t.row_height = positive(j, "row_height", t.row_height);
    t.padding = positive(j, "padding", t.padding);
    t.gap = positive(j, "gap", t.gap);
    return t;
}
Theme Theme::load(const std::filesystem::path& path) {
    return from_json(faset::read_json(path));
}
Json Theme::to_json() const {
    return {{"background", background}, {"surface", surface},
            {"raised", raised},         {"hover", hover},
            {"border", border},         {"text", text},
            {"muted", muted},           {"accent", accent},
            {"selection", selection},   {"danger", danger},
            {"font_size", font_size},   {"row_height", row_height},
            {"padding", padding},       {"gap", gap}};
}
Widget& Widget::add(Kind type, const std::string& stable_id, const std::string& label) {
    for (auto& child : children)
        if (child->id == stable_id) {
            if (child->kind != type)
                throw std::runtime_error("Widget ID reused with different kind: " + stable_id);
            if (!label.empty())
                child->text = label;
            return *child;
        }
    auto child = std::make_unique<Widget>();
    child->kind = type;
    child->id = stable_id;
    child->text = label;
    child->parent = this;
    children.push_back(std::move(child));
    return *children.back();
}
Widget* Widget::find(std::string_view wanted) {
    if (id == wanted)
        return this;
    for (auto& child : children)
        if (auto* result = child->find(wanted))
            return result;
    return nullptr;
}
const Widget* Widget::find(std::string_view wanted) const {
    if (id == wanted)
        return this;
    for (const auto& child : children)
        if (auto* result = child->find(wanted))
            return result;
    return nullptr;
}
void Widget::remove(std::string_view wanted) {
    std::erase_if(children, [&](const auto& child) { return child->id == wanted; });
}
void DockLayout::move(const std::string& panel, const std::string& area, std::size_t index) {
    if (panel.empty() || area.empty())
        throw std::runtime_error("Dock IDs cannot be empty");
    for (auto& [name, panels] : areas_) {
        (void)name;
        std::erase(panels, panel);
    }
    auto& panels = areas_[area];
    panels.insert(panels.begin() + static_cast<std::ptrdiff_t>(std::min(index, panels.size())),
                  panel);
}
std::vector<std::string> DockLayout::panels(const std::string& area) const {
    auto found = areas_.find(area);
    return found == areas_.end() ? std::vector<std::string>{} : found->second;
}
void DockLayout::set_size(const std::string& panel, float size) {
    if (!std::isfinite(size) || size < 24 || size > 10000)
        throw std::runtime_error("Invalid dock panel size");
    sizes_[panel] = size;
}
float DockLayout::size(const std::string& panel, float fallback) const {
    auto found = sizes_.find(panel);
    return found == sizes_.end() ? fallback : found->second;
}
Json DockLayout::to_json() const {
    return {{"version", 1}, {"areas", areas_}, {"sizes", sizes_}};
}
void DockLayout::from_json(const Json& j) {
    if (j.at("version") != 1)
        throw std::runtime_error("Unsupported dock layout version");
    auto areas = j.at("areas").get<decltype(areas_)>();
    auto sizes = j.at("sizes").get<decltype(sizes_)>();
    std::set<std::string> seen;
    for (const auto& [area, panels] : areas) {
        if (area.empty())
            throw std::runtime_error("Empty dock area");
        for (const auto& panel : panels)
            if (panel.empty() || !seen.insert(panel).second)
                throw std::runtime_error("Duplicate dock panel");
    }
    for (const auto& [id, size] : sizes)
        if (id.empty() || !std::isfinite(size) || size < 24 || size > 10000)
            throw std::runtime_error("Invalid dock size");
    areas_ = std::move(areas);
    sizes_ = std::move(sizes);
}
void DockLayout::save(const std::filesystem::path& path) const {
    faset::atomic_write_json(path, to_json());
}
void DockLayout::load(const std::filesystem::path& path) {
    from_json(faset::read_json(path));
}

struct Context::Impl {
    Widget root;
    Theme theme;
    FontAtlas font;
    float width = 0, height = 0, scale = 1, mouse_x = 0, mouse_y = 0;
    std::string focused, hovered, captured;
    std::chrono::steady_clock::time_point hover_started = std::chrono::steady_clock::now();
    struct Edit {
        TextBuffer buffer;
        std::string original, composition;
        double original_value = 0;
        float scroll = 0;
        int composition_start = 0, composition_length = 0;
    };
    std::unordered_map<std::string, Edit> edits;
    float down_x = 0, down_y = 0;
    double down_value = 0;
    bool dragging = false;
    Json payload;
    float before_size = 0, after_size = 0;
    Layout before_layout, after_layout;
    std::function<std::string()> clipboard_read;
    std::function<void(const std::string&)> clipboard_write;
    std::function<void(bool)> ime_enabled;
    std::function<void(Rect)> ime_rectangle;
    DockLayout* docking = nullptr;
    std::function<void()> docking_changed;
    explicit Impl(const std::filesystem::path& file) : font(file) {
        root.kind = Kind::Column;
        root.id = "root";
        root.layout.gap = 0;
    }
    Widget* find(std::string_view id) {
        return root.find(id);
    }
    float intrinsic(Widget& w, bool horizontal) {
        const auto fixed = horizontal ? w.layout.width : w.layout.height;
        if (fixed >= 0)
            return fixed * scale;
        if (w.kind == Kind::Divider)
            return 5 * scale;
        if (horizontal) {
            if (w.kind == Kind::Label || w.kind == Kind::Button || w.kind == Kind::Tab)
                return font.measure(w.text,
                                    (w.font_size > 0 ? w.font_size : theme.font_size) * scale) +
                       theme.padding * 2 * scale;
            return 80 * scale;
        }
        if (w.kind == Kind::Panel || w.kind == Kind::Column || w.kind == Kind::Row) {
            const bool horizontal = w.kind == Kind::Row && !w.layout.stack_vertical;
            float total = 0;
            std::size_t count = 0;
            for (auto& child : w.children)
                if (child->visible && !child->layout.absolute) {
                    const auto value = intrinsic(*child, false);
                    if (horizontal)
                        total = std::max(total, value);
                    else
                        total += value;
                    ++count;
                }
            if (!horizontal && count)
                total += float(count - 1) * w.layout.gap * scale;
            return total + w.layout.padding * 2 * scale;
        }
        return theme.row_height * scale;
    }
    void arrange(Widget& w, Rect rect, Rect clip) {
        w.rect = rect;
        w.clip = w.layout.clip ? clip.intersection(rect) : clip;
        if (!w.visible)
            return;
        const auto pad = w.layout.padding * scale;
        Rect inner{rect.x + pad, rect.y + pad, std::max(0.f, rect.width - 2 * pad),
                   std::max(0.f, rect.height - 2 * pad)};
        const bool horizontal = w.kind == Kind::Row && !w.layout.stack_vertical;
        const float available = horizontal ? inner.width : inner.height;
        std::vector<Widget*> flow;
        float fixed = 0, flex = 0;
        for (auto& child : w.children)
            if (child->visible && !child->layout.absolute) {
                flow.push_back(child.get());
                if (child->layout.flex > 0) {
                    flex += child->layout.flex;
                    fixed +=
                        (horizontal ? child->layout.min_width : child->layout.min_height) * scale;
                } else
                    fixed += intrinsic(*child, horizontal);
            }
        if (!flow.empty())
            fixed += float(flow.size() - 1) * w.layout.gap * scale;
        const auto extra = std::max(0.f, available - fixed);
        std::vector<float> extents;
        float content = 0;
        for (auto* child : flow) {
            auto extent =
                child->layout.flex > 0
                    ? (horizontal ? child->layout.min_width : child->layout.min_height) * scale +
                          extra * child->layout.flex / std::max(.001f, flex)
                    : intrinsic(*child, horizontal);
            extent = std::clamp(
                extent, (horizontal ? child->layout.min_width : child->layout.min_height) * scale,
                (horizontal ? child->layout.max_width : child->layout.max_height) * scale);
            extents.push_back(extent);
            content += extent;
        }
        if (!flow.empty())
            content += float(flow.size() - 1) * w.layout.gap * scale;
        w.content_height = horizontal ? inner.height : content;
        w.scroll_y = std::clamp(w.scroll_y, 0.f, std::max(0.f, w.content_height - inner.height));
        float position = horizontal ? inner.x : inner.y - (w.layout.scroll ? w.scroll_y : 0);
        for (std::size_t i = 0; i < flow.size(); ++i) {
            auto& child = *flow[i];
            float cross =
                horizontal ? (child.layout.height >= 0 ? child.layout.height * scale : inner.height)
                           : (child.layout.width >= 0 ? child.layout.width * scale : inner.width);
            cross = std::clamp(
                cross, (horizontal ? child.layout.min_height : child.layout.min_width) * scale,
                (horizontal ? child.layout.max_height : child.layout.max_width) * scale);
            Rect target = horizontal ? Rect{position, inner.y, extents[i], cross}
                                     : Rect{inner.x, position, cross, extents[i]};
            arrange(child, target, w.clip.intersection(inner));
            position += extents[i] + w.layout.gap * scale;
        }
        for (auto& child : w.children)
            if (child->visible && child->layout.absolute)
                arrange(*child,
                        {inner.x + child->layout.x * scale, inner.y + child->layout.y * scale,
                         child->layout.width >= 0 ? child->layout.width * scale : inner.width,
                         child->layout.height >= 0 ? child->layout.height * scale
                                                   : intrinsic(*child, false)},
                        w.clip.intersection(inner));
    }
    Widget* hit(Widget& w, float x, float y) {
        if (!w.visible || !w.clip.contains(x, y))
            return nullptr;
        if (!w.enabled)
            return w.rect.contains(x, y) ? &w : nullptr;
        for (auto child = w.children.rbegin(); child != w.children.rend(); ++child)
            if (auto* target = hit(**child, x, y))
                return target;
        return w.rect.contains(x, y) ? &w : nullptr;
    }
    bool commit() {
        if (focused.empty())
            return true;
        auto* widget = find(focused);
        auto found = edits.find(focused);
        if (!widget || !field(*widget) || found == edits.end())
            return true;
        auto& edit = found->second;
        if (!edit.composition.empty())
            return false;
        const bool changed = edit.buffer.text() != edit.original;
        if (widget->kind == Kind::NumberField) {
            try {
                std::size_t end = 0;
                const auto value = std::stod(edit.buffer.text(), &end);
                if (end != edit.buffer.text().size() || !std::isfinite(value))
                    throw std::runtime_error("number");
                widget->value = value;
            } catch (...) {
                widget->error = "Enter a finite number";
                return false;
            }
        }
        widget->error.clear();
        widget->text = edit.buffer.text();
        edit.original = edit.buffer.text();
        edit.original_value = widget->value;
        const auto cursor = edit.buffer.cursor();
        edit.buffer.reset(edit.original);
        edit.buffer.set_cursor(cursor);
        auto callback = widget->on_commit;
        if (changed && callback)
            callback(*widget);
        return true;
    }
    void unfocus(bool save) {
        if (save && !commit())
            return;
        focused.clear();
        if (ime_enabled)
            ime_enabled(false);
    }
    void reveal(Widget& widget) {
        // Reveal inside-out: an offscreen nested scroller still needs its own
        // contents positioned before an outer scroller brings it into view.
        auto* branch = &widget;
        for (auto* parent = widget.parent; parent; branch = parent, parent = parent->parent) {
            if (!parent->layout.scroll || parent->kind == Kind::Row || branch->layout.absolute)
                continue;
            const float padding = parent->layout.padding * scale;
            const float top = parent->rect.y + padding;
            const float extent = std::max(0.f, parent->rect.height - 2 * padding);
            if (extent <= 0)
                continue;
            float delta = 0;
            if (widget.rect.y < top || widget.rect.height > extent)
                delta = widget.rect.y - top;
            else if (widget.rect.y + widget.rect.height > top + extent)
                delta = widget.rect.y + widget.rect.height - top - extent;
            const float scroll = std::clamp(parent->scroll_y + delta, 0.f,
                                            std::max(0.f, parent->content_height - extent));
            if (scroll != parent->scroll_y) {
                parent->scroll_y = scroll;
                arrange(root, {0, 0, width, height}, {0, 0, width, height});
            }
        }
    }
    bool focus(const std::string& id) {
        auto* widget = find(id);
        if (!widget || !focusable(*widget))
            return false;
        if (focused == id) {
            reveal(*widget);
            if (field(*widget) && ime_rectangle)
                ime_rectangle(widget->rect);
            return true;
        }
        if (!commit())
            return false;
        // A commit callback may reconcile the retained tree.
        widget = find(id);
        if (!widget || !focusable(*widget))
            return false;
        focused = id;
        reveal(*widget);
        if (field(*widget)) {
            auto& edit = edits[id];
            const auto text = widget->kind == Kind::NumberField
                                  ? number(widget->value, widget->precision)
                                  : widget->text;
            edit.buffer.reset(text);
            edit.original = text;
            edit.original_value = widget->value;
            edit.composition.clear();
            edit.scroll = 0;
            if (widget->kind == Kind::NumberField)
                edit.buffer.select_all();
            if (ime_enabled)
                ime_enabled(true);
            if (ime_rectangle)
                ime_rectangle(widget->rect);
        } else if (ime_enabled)
            ime_enabled(false);
        return true;
    }
    void preview() {
        auto* widget = find(focused);
        auto found = edits.find(focused);
        if (!widget || found == edits.end())
            return;
        widget->text = found->second.buffer.text();
        widget->error.clear();
        auto callback = widget->on_preview;
        if (callback)
            callback(*widget);
    }
    std::size_t text_position(const Widget& w, const Edit& e, float x) {
        const auto local = x - w.rect.x - theme.padding * scale + e.scroll;
        float previous_width = 0;
        std::size_t previous_byte = 0;
        for (std::size_t i = 0; i < e.buffer.text().size();) {
            ++i;
            while (i < e.buffer.text().size() &&
                   (static_cast<unsigned char>(e.buffer.text()[i]) & 0xc0) == 0x80)
                ++i;
            const auto width =
                font.measure(std::string_view(e.buffer.text()).substr(0, i),
                             (w.font_size > 0 ? w.font_size : theme.font_size) * scale);
            if (local < (previous_width + width) * .5f)
                return previous_byte;
            previous_width = width;
            previous_byte = i;
        }
        return e.buffer.text().size();
    }
    std::pair<Widget*, Widget*> neighbors(Widget& divider) {
        if (!divider.parent)
            return {};
        auto& list = divider.parent->children;
        for (std::size_t i = 1; i + 1 < list.size(); ++i)
            if (list[i].get() == &divider)
                return {list[i - 1].get(), list[i + 1].get()};
        return {};
    }
    void cancel_capture() {
        if (auto* w = find(captured); w && w->kind == Kind::NumberField && dragging) {
            w->value = down_value;
            auto& edit = edits[w->id];
            edit.buffer.reset(number(w->value, w->precision));
            w->text = edit.buffer.text();
            auto callback = w->on_preview;
            if (callback)
                callback(*w);
        }
        if (auto* w = find(captured); w && w->kind == Kind::Divider && dragging) {
            auto [before, after] = neighbors(*w);
            if (before && after) {
                before->layout = before_layout;
                after->layout = after_layout;
                arrange(root, {0, 0, width, height}, {0, 0, width, height});
            }
        }
        if (auto* w = find(captured); w && dragging) {
            auto callback = w->on_cancel;
            if (callback)
                callback(*w);
        }
        captured.clear();
        payload = nullptr;
        dragging = false;
    }
    void draw_widget(Widget& w, render::Snapshot& frame) {
        if (!w.visible || w.clip.width <= 0 || w.clip.height <= 0)
            return;
        const bool hover = w.id == hovered, focus = w.id == focused;
        auto ink = w.enabled ? theme.text : theme.muted;
        const auto& rect = w.rect;
        if (w.kind == Kind::Panel) {
            fill(frame, rect, theme.surface, w.clip);
            if (w.layout.absolute)
                outline(frame, rect, theme.border, w.clip);
        } else if (w.kind == Kind::Button) {
            if (w.appearance == Appearance::Primary && w.enabled) {
                fill(frame, rect, theme.accent, w.clip);
                ink = theme.background;
            } else if (w.appearance == Appearance::Quiet) {
                if (w.selected || (hover && w.enabled))
                    fill(frame, rect, w.selected ? theme.selection : theme.hover, w.clip);
            } else {
                fill(frame, rect,
                     hover && w.enabled ? theme.hover
                     : w.selected       ? theme.selection
                                        : theme.raised,
                     w.clip);
                outline(frame, rect, focus ? theme.accent : theme.border, w.clip);
            }
            if (focus && w.appearance != Appearance::Default)
                outline(frame, rect, theme.accent, w.clip);
        } else if (w.kind == Kind::Tab || w.kind == Kind::TreeRow) {
            if (w.kind == Kind::TreeRow && (w.selected || hover))
                fill(frame, rect, w.selected ? theme.selection : theme.hover, w.clip);
            else if (w.kind == Kind::Tab && hover && !w.selected)
                fill(frame, rect, theme.hover, w.clip);
            if (w.selected) {
                if (w.kind == Kind::Tab)
                    fill(frame, {rect.x, rect.y + rect.height - 2 * scale, rect.width, 2 * scale},
                         theme.accent, w.clip);
                else
                    fill(frame, {rect.x, rect.y, 2 * scale, rect.height}, theme.accent, w.clip);
            }
            if (w.kind == Kind::Tab && !w.selected)
                ink = theme.muted;
            if (focus)
                outline(frame, rect, theme.accent, w.clip);
        } else if (w.kind == Kind::Row && w.appearance == Appearance::Section) {
            fill(frame, {rect.x, rect.y, rect.width, scale}, theme.border, w.clip);
        } else if (field(w)) {
            fill(frame, rect, theme.background, w.clip);
            outline(frame, rect,
                    !w.error.empty() ? theme.danger
                    : focus          ? theme.accent
                                     : theme.border,
                    w.clip);
        } else if (w.kind == Kind::Divider) {
            fill(frame, rect, hover || w.id == captured ? theme.border : theme.background, w.clip);
        }
        float tx = rect.x + theme.padding * scale + w.indent * 14 * scale;
        const auto font_size = (w.font_size > 0 ? w.font_size : theme.font_size) * scale;
        const auto ty = rect.y + std::max(0.f, (rect.height - font_size * 1.45f) * .5f);
        if (w.kind == Kind::Checkbox) {
            Rect box{tx, rect.y + (rect.height - 14 * scale) / 2, 14 * scale, 14 * scale};
            fill(frame, box, theme.background, w.clip);
            outline(frame, box, focus ? theme.accent : theme.border, w.clip);
            if (w.checked)
                fill(frame, {box.x + 3 * scale, box.y + 3 * scale, 8 * scale, 8 * scale},
                     theme.accent, w.clip);
            tx += 23 * scale;
        }
        if (field(w) && focus && edits.contains(w.id)) {
            auto& edit = edits[w.id];
            const auto available = std::max(1.f, rect.width - theme.padding * 2 * scale);
            const auto selection_begin = std::min(edit.buffer.cursor(), edit.buffer.anchor());
            const auto selection_end = std::max(edit.buffer.cursor(), edit.buffer.anchor());
            auto composition_byte = [&](int codepoints) {
                std::size_t byte = 0;
                for (int i = 0; i < std::max(0, codepoints) && byte < edit.composition.size();
                     ++i) {
                    ++byte;
                    while (byte < edit.composition.size() &&
                           (static_cast<unsigned char>(edit.composition[byte]) & 0xc0) == 0x80)
                        ++byte;
                }
                return byte;
            };
            const auto prefix = edit.buffer.text().substr(0, selection_begin);
            const auto composition_left = font.measure(prefix, font_size);
            const auto composition_cursor = composition_byte(edit.composition_start);
            const auto caret =
                edit.composition.empty()
                    ? font.measure(
                          std::string_view(edit.buffer.text()).substr(0, edit.buffer.cursor()),
                          font_size)
                    : composition_left +
                          font.measure(
                              std::string_view(edit.composition).substr(0, composition_cursor),
                              font_size);
            if (caret - edit.scroll > available - 2 * scale)
                edit.scroll = caret - available + 2 * scale;
            if (caret < edit.scroll)
                edit.scroll = caret;
            edit.scroll = std::max(0.f, edit.scroll);
            const Rect text_clip =
                w.clip.intersection({rect.x + 3 * scale, rect.y + 2 * scale, rect.width - 6 * scale,
                                     rect.height - 4 * scale});
            tx -= edit.scroll;
            if (edit.buffer.has_selection() && edit.composition.empty()) {
                const auto begin = std::min(edit.buffer.cursor(), edit.buffer.anchor()),
                           end = std::max(edit.buffer.cursor(), edit.buffer.anchor());
                const auto left = font.measure(
                               std::string_view(edit.buffer.text()).substr(0, begin), font_size),
                           right = font.measure(std::string_view(edit.buffer.text()).substr(0, end),
                                                font_size);
                fill(frame, {tx + left, rect.y + 4 * scale, right - left, rect.height - 8 * scale},
                     theme.selection, text_clip);
            }
            if (edit.composition.empty()) {
                font.draw(frame, edit.buffer.text(), tx, ty, font_size, ink, text_clip);
            } else {
                const auto composition_width = font.measure(edit.composition, font_size);
                const auto selection_width = font.measure(
                    std::string_view(edit.composition)
                        .substr(composition_cursor,
                                composition_byte(edit.composition_start +
                                                 std::max(0, edit.composition_length)) -
                                    composition_cursor),
                    font_size);
                if (selection_width > 0)
                    fill(frame,
                         {tx + caret, rect.y + 4 * scale, selection_width, rect.height - 8 * scale},
                         theme.selection, text_clip);
                font.draw(frame, prefix, tx, ty, font_size, ink, text_clip);
                font.draw(frame, edit.composition, tx + composition_left, ty, font_size,
                          theme.accent, text_clip);
                font.draw(frame, std::string_view(edit.buffer.text()).substr(selection_end),
                          tx + composition_left + composition_width, ty, font_size, ink, text_clip);
                fill(frame,
                     {tx + composition_left, rect.y + rect.height - 4 * scale, composition_width,
                      scale},
                     theme.accent, text_clip);
            }
            fill(frame, {tx + caret, rect.y + 5 * scale, scale, rect.height - 10 * scale},
                 theme.accent, text_clip);
        } else if (field(w) && w.text.empty() && !w.placeholder.empty())
            font.draw(frame, w.placeholder, tx, ty, font_size, theme.muted,
                      w.clip.intersection(
                          {rect.x + 2 * scale, rect.y, rect.width - 4 * scale, rect.height}));
        else if (w.kind != Kind::Divider && w.kind != Kind::Viewport && !w.text.empty())
            font.draw(frame, w.kind == Kind::NumberField ? number(w.value, w.precision) : w.text,
                      tx, ty, font_size, ink,
                      w.clip.intersection(
                          {rect.x + 2 * scale, rect.y, rect.width - 4 * scale, rect.height}));
        else if (w.kind == Kind::NumberField)
            font.draw(frame, number(w.value, w.precision), tx, ty, font_size, ink, w.clip);
        for (auto& child : w.children)
            draw_widget(*child, frame);
        if (w.layout.scroll && w.content_height > rect.height) {
            const float track = rect.height - 8 * scale,
                        thumb = std::max(16 * scale, track * rect.height / w.content_height),
                        offset = (track - thumb) * w.scroll_y /
                                 std::max(1.f, w.content_height - rect.height);
            fill(frame,
                 {rect.x + rect.width - 5 * scale, rect.y + 4 * scale + offset, 3 * scale, thumb},
                 theme.border, w.clip);
        }
    }
};

Context::Context(const std::filesystem::path& font) : impl_(std::make_unique<Impl>(font)) {}
Context::~Context() = default;
Widget& Context::root() {
    return impl_->root;
}
Widget* Context::find(std::string_view id) {
    return impl_->find(id);
}
void Context::set_theme(Theme theme) {
    impl_->theme = theme;
}
const Theme& Context::theme() const {
    return impl_->theme;
}
FontAtlas& Context::font() {
    return impl_->font;
}
void Context::validate_layout(const Json& document) const {
    if (!document.is_object())
        throw std::runtime_error("Layout must be an object");
    const auto& definition = document.contains("root") ? document.at("root") : document;
    if (definition.at("id") != impl_->root.id)
        throw std::runtime_error("Layout root ID must match retained root");
    std::set<std::string> ids;
    std::function<void(const Json&, const std::string&)> validate = [&](const Json& j,
                                                                        const std::string& parent) {
        const auto id = j.at("id").get<std::string>();
        if (id.empty() || !ids.insert(id).second)
            throw std::runtime_error("Duplicate layout widget ID");
        const auto* existing = impl_->find(id);
        if (existing && ((existing->parent ? existing->parent->id : std::string()) != parent))
            throw std::runtime_error("Hot layout cannot reparent existing widget: " + id);
        if (j.contains("kind")) {
            const auto type = kind_from_string(j.at("kind"));
            if (existing && existing->kind != type)
                throw std::runtime_error("Hot layout cannot replace widget kind");
        }
        if (j.contains("text") &&
            (!j.at("text").is_string() || !TextBuffer::valid_utf8(j.at("text").get<std::string>())))
            throw std::runtime_error("Widget text must be valid UTF-8");
        if (j.contains("layout"))
            parse_layout(j.at("layout"), existing ? existing->layout : Layout{});
        if (j.contains("font_size")) {
            const auto size = j.at("font_size").get<float>();
            if (!std::isfinite(size) || size < 0 || size > 128)
                throw std::runtime_error("Invalid widget font size");
        }
        if (j.contains("children")) {
            if (!j.at("children").is_array())
                throw std::runtime_error("Layout children must be an array");
            for (const auto& child : j.at("children"))
                validate(child, id);
        }
    };
    validate(definition, "");
}
void Context::apply_layout(const Json& document) {
    validate_layout(document);
    const auto& definition = document.contains("root") ? document.at("root") : document;
    std::function<void(Widget&, const Json&)> apply = [&](Widget& w, const Json& j) {
        if (j.contains("text"))
            update_text(w.id, j.at("text"));
        if (j.contains("layout"))
            w.layout = parse_layout(j.at("layout"), w.layout);
        if (j.contains("font_size"))
            w.font_size = j.at("font_size");
        if (j.contains("children"))
            for (const auto& child : j.at("children")) {
                const auto id = child.at("id").get<std::string>();
                auto& target =
                    w.add(child.contains("kind") ? kind_from_string(child.at("kind"))
                                                 : (w.find(id) ? w.find(id)->kind : Kind::Panel),
                          id);
                apply(target, child);
            }
    };
    apply(root(), definition);
}
void Context::layout(float width, float height, float scale) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("UI dimensions and display scale must be finite and valid");
    scale = std::clamp(scale, .5f, 4.f);
    if (impl_->scale != scale) {
        // Pointer captures use the old drawable coordinate system. Keep edits,
        // but cancel a drag rather than committing a jump after a monitor change.
        impl_->cancel_capture();
        const auto ratio = scale / impl_->scale;
        std::function<void(Widget&)> rescale = [&](Widget& widget) {
            widget.scroll_y *= ratio;
            for (auto& child : widget.children)
                rescale(*child);
        };
        rescale(root());
        for (auto& [id, edit] : impl_->edits)
            edit.scroll *= ratio;
    }
    impl_->width = std::max(0.f, width);
    impl_->height = std::max(0.f, height);
    impl_->scale = scale;
    std::set<std::string> ids;
    std::function<void(Widget&)> check = [&](Widget& w) {
        if (w.id.empty() || !ids.insert(w.id).second)
            throw std::runtime_error("Retained widget IDs must be unique");
        for (auto& child : w.children) {
            child->parent = &w;
            check(*child);
        }
    };
    check(root());
    impl_->arrange(root(), {0, 0, impl_->width, impl_->height},
                   {0, 0, impl_->width, impl_->height});
    if (!impl_->hovered.empty()) {
        const auto* hit = impl_->hit(root(), impl_->mouse_x, impl_->mouse_y);
        const auto current_hover = hit ? hit->id : "";
        if (current_hover != impl_->hovered) {
            impl_->hovered = current_hover;
            impl_->hover_started = std::chrono::steady_clock::now();
        }
    }
    std::erase_if(impl_->edits, [&](const auto& entry) { return !ids.contains(entry.first); });
    auto* focused = impl_->find(impl_->focused);
    if (!focused || !focusable(*focused))
        impl_->unfocus(false);
    else if (field(*focused) && impl_->ime_rectangle)
        impl_->ime_rectangle(focused->rect);
}
void Context::draw(render::Snapshot& snapshot) {
    impl_->draw_widget(root(), snapshot);
    if (impl_->payload.is_null() && impl_->captured.empty() &&
        std::chrono::steady_clock::now() - impl_->hover_started >=
            std::chrono::milliseconds(450)) {
        const auto* widget = impl_->find(impl_->hovered);
        if (widget && widget->visible && !widget->tooltip.empty() && impl_->width > 100) {
            const auto font_size = impl_->theme.font_size * impl_->scale;
            const auto padding = 8.f * impl_->scale;
            const auto max_line_width =
                std::max(40.f, std::min(480.f * impl_->scale, impl_->width - 24.f * impl_->scale) -
                                   2.f * padding);
            std::vector<std::string> lines;
            std::string line;
            for (std::size_t i = 0; i < widget->tooltip.size();) {
                const auto first = static_cast<unsigned char>(widget->tooltip[i]);
                std::size_t bytes = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
                bytes = std::min(bytes, widget->tooltip.size() - i);
                const auto character = widget->tooltip.substr(i, bytes);
                i += bytes;
                if (character == "\n") {
                    lines.push_back(line);
                    line.clear();
                    continue;
                }
                if (!line.empty() &&
                    impl_->font.measure(line + character, font_size) > max_line_width) {
                    lines.push_back(line);
                    line.clear();
                }
                line += character;
            }
            lines.push_back(line);
            float line_width = 0;
            for (const auto& value : lines)
                line_width = std::max(line_width, impl_->font.measure(value, font_size));
            const auto line_height = font_size + 5.f * impl_->scale;
            const float width = line_width + 2.f * padding;
            const float height = float(lines.size()) * line_height + 2.f * padding;
            const Rect viewport{0, 0, impl_->width, impl_->height};
            const Rect tooltip{
                std::clamp(impl_->mouse_x + 12.f * impl_->scale, 4.f,
                           std::max(4.f, impl_->width - width - 4.f)),
                std::clamp(impl_->mouse_y + 18.f * impl_->scale, 4.f,
                           std::max(4.f, impl_->height - height - 4.f)),
                width, height};
            fill(snapshot, tooltip, impl_->theme.raised, viewport);
            outline(snapshot, tooltip, impl_->theme.border, viewport);
            for (std::size_t i = 0; i < lines.size(); ++i)
                impl_->font.draw(snapshot, lines[i], tooltip.x + padding,
                                 tooltip.y + padding + float(i) * line_height, font_size,
                                 impl_->theme.text, viewport);
        }
    }
    if (!impl_->payload.is_null()) {
        const Rect viewport{0, 0, impl_->width, impl_->height};
        const auto text =
            impl_->payload.value("label", impl_->payload.value("panel", std::string("Move")));
        const auto width =
            impl_->font.measure(text, impl_->theme.font_size * impl_->scale) + 20 * impl_->scale;
        const Rect rect{impl_->mouse_x + 12 * impl_->scale, impl_->mouse_y + 12 * impl_->scale,
                        width, 28 * impl_->scale};
        fill(snapshot, rect, impl_->theme.raised, viewport);
        outline(snapshot, rect, impl_->theme.accent, viewport);
        impl_->font.draw(snapshot, text, rect.x + 8 * impl_->scale, rect.y + 3 * impl_->scale,
                         impl_->theme.font_size * impl_->scale, impl_->theme.text, viewport);
    }
}
bool Context::update_text(const std::string& id, const std::string& value, bool force) {
    auto* w = find(id);
    if (!w)
        return false;
    if (!TextBuffer::valid_utf8(value))
        return false;
    if (impl_->focused == id && impl_->edits.contains(id)) {
        auto& edit = impl_->edits[id];
        if (!force && (edit.buffer.text() != edit.original || !edit.composition.empty()))
            return false;
        const auto cursor = edit.buffer.cursor();
        edit.buffer.reset(value);
        edit.buffer.set_cursor(cursor);
        edit.original = value;
        edit.composition.clear();
    }
    w->text = value;
    return true;
}
bool Context::update_number(const std::string& id, double value, bool force) {
    auto* widget = find(id);
    if (!widget || widget->kind != Kind::NumberField || !std::isfinite(value))
        return false;
    if (!update_text(id, number(value, widget->precision), force))
        return false;
    widget->value = value;
    if (impl_->focused == id && impl_->edits.contains(id))
        impl_->edits[id].original_value = value;
    return true;
}
bool Context::focus(const std::string& id) {
    return impl_->focus(id);
}
const std::string& Context::focused_id() const {
    return impl_->focused;
}
void Context::clear_focus(bool commit) {
    impl_->unfocus(commit);
}
bool Context::editing() const {
    const auto* w = impl_->find(impl_->focused);
    return w && field(*w);
}
void Context::set_clipboard(std::function<std::string()> read,
                            std::function<void(const std::string&)> write) {
    impl_->clipboard_read = std::move(read);
    impl_->clipboard_write = std::move(write);
}
void Context::set_ime(std::function<void(bool)> enabled, std::function<void(Rect)> rectangle) {
    impl_->ime_enabled = std::move(enabled);
    impl_->ime_rectangle = std::move(rectangle);
}
void Context::set_docking(DockLayout* docking, std::function<void()> changed) {
    impl_->docking = docking;
    impl_->docking_changed = std::move(changed);
}

bool Context::handle(const render::Event& event) {
    auto& p = *impl_;
    using Type = render::Event::Type;
    if (event.type == Type::FocusLost) {
        p.cancel_capture();
        p.hovered.clear();
        if (auto found = p.edits.find(p.focused); found != p.edits.end())
            found->second.composition.clear();
        p.unfocus(true);
        return false;
    }
    if (event.type == Type::MouseMove || event.type == Type::MouseDown ||
        event.type == Type::MouseUp) {
        p.mouse_x = event.x;
        p.mouse_y = event.y;
        auto* hit = p.hit(p.root, event.x, event.y);
        p.hovered = hit ? hit->id : "";
        p.hover_started = std::chrono::steady_clock::now();
    }
    if (event.type == Type::Wheel) {
        auto* w = p.hit(p.root, p.mouse_x, p.mouse_y);
        for (; w; w = w->parent)
            if (w->layout.scroll) {
                w->scroll_y =
                    std::clamp(w->scroll_y - event.y * p.theme.row_height * 3 * p.scale, 0.f,
                               std::max(0.f, w->content_height - w->rect.height +
                                                 2 * w->layout.padding * p.scale));
                layout(p.width, p.height, p.scale);
                p.hover_started = std::chrono::steady_clock::now();
                return true;
            }
        return false;
    }
    if (event.type == Type::MouseDown && event.button == 1) {
        auto* w = p.hit(p.root, event.x, event.y);
        if (!w || w->kind == Kind::Viewport) {
            p.unfocus(true);
            return false;
        }
        if (!w->enabled)
            return true;
        if (focusable(*w)) {
            if (!p.focus(w->id))
                return true;
        } else {
            if (!p.commit())
                return true;
            p.unfocus(false);
        }
        p.captured = w->id;
        p.down_x = event.x;
        p.down_y = event.y;
        p.down_value = w->value;
        p.dragging = false;
        p.payload = nullptr;
        if (w->kind == Kind::TextField) {
            auto& edit = p.edits[w->id];
            edit.buffer.set_cursor(p.text_position(*w, edit, event.x), event.shift);
        }
        if (w->kind == Kind::Divider) {
            auto [before, after] = p.neighbors(*w);
            if (before && after) {
                const bool horizontal = w->parent->kind == Kind::Row;
                p.before_size = (horizontal ? before->rect.width : before->rect.height) / p.scale;
                p.after_size = (horizontal ? after->rect.width : after->rect.height) / p.scale;
                p.before_layout = before->layout;
                p.after_layout = after->layout;
            }
        }
        return true;
    }
    if (event.type == Type::MouseMove && !p.captured.empty()) {
        auto* w = p.find(p.captured);
        if (!w) {
            p.cancel_capture();
            return false;
        }
        const auto distance = std::hypot(event.x - p.down_x, event.y - p.down_y);
        if (w->kind == Kind::NumberField && distance > 3 * p.scale) {
            p.dragging = true;
            w->value =
                p.down_value + (event.x - p.down_x) / p.scale * w->step * (event.shift ? .1 : 1.0);
            auto& edit = p.edits[w->id];
            edit.buffer.reset(number(w->value, w->precision));
            w->text = edit.buffer.text();
            auto callback = w->on_preview;
            if (callback)
                callback(*w);
            return true;
        }
        if (w->kind == Kind::TextField) {
            auto& edit = p.edits[w->id];
            edit.buffer.set_cursor(p.text_position(*w, edit, event.x), true);
            return true;
        }
        if (w->kind == Kind::Divider) {
            auto [before, after] = p.neighbors(*w);
            if (before && after) {
                const bool horizontal = w->parent->kind == Kind::Row;
                const auto delta = (horizontal ? event.x - p.down_x : event.y - p.down_y) / p.scale;
                const auto minimum_before = std::max(24.f, horizontal ? before->layout.min_width
                                                                      : before->layout.min_height),
                           minimum_after = std::max(24.f, horizontal ? after->layout.min_width
                                                                     : after->layout.min_height);
                const auto low = minimum_before - p.before_size,
                           high = p.after_size - minimum_after;
                const auto amount = low <= high ? std::clamp(delta, low, high) : 0.f;
                if (before->layout.flex <= 0) {
                    if (horizontal)
                        before->layout.width = p.before_size + amount;
                    else
                        before->layout.height = p.before_size + amount;
                }
                if (after->layout.flex <= 0) {
                    if (horizontal)
                        after->layout.width = p.after_size - amount;
                    else
                        after->layout.height = p.after_size - amount;
                }
                if (before->layout.flex > 0 && after->layout.flex > 0) {
                    before->layout.flex = p.before_size + amount;
                    after->layout.flex = p.after_size - amount;
                }
                p.dragging = std::abs(amount) > .01f;
                w->value = p.before_size + amount;
                auto callback = w->on_preview;
                if (callback)
                    callback(*w);
                layout(p.width, p.height, p.scale);
            }
            return true;
        }
        if (distance > 5 * p.scale) {
            if (!w->dock_panel.empty()) {
                p.payload = {{"kind", "dock_panel"}, {"panel", w->dock_panel}, {"label", w->text}};
                p.dragging = true;
            } else if (!w->drag_payload.is_null()) {
                p.payload = w->drag_payload;
                p.dragging = true;
            }
        }
        return true;
    }
    if (event.type == Type::MouseUp && event.button == 1 && !p.captured.empty()) {
        const auto id = p.captured;
        auto* w = p.find(id);
        const auto dragging = p.dragging;
        auto payload = p.payload;
        p.captured.clear();
        p.payload = nullptr;
        p.dragging = false;
        if (!w)
            return true;
        if (!payload.is_null()) {
            auto* target = p.hit(p.root, event.x, event.y);
            for (; target; target = target->parent) {
                if (p.docking && payload.value("kind", std::string()) == "dock_panel" &&
                    !target->dock_area.empty()) {
                    auto order = p.docking->panels(target->dock_area);
                    auto found = std::find(order.begin(), order.end(), target->dock_panel);
                    p.docking->move(payload.at("panel"), target->dock_area,
                                    static_cast<std::size_t>(found - order.begin()));
                    if (p.docking_changed)
                        p.docking_changed();
                    break;
                }
                if (target->on_drop) {
                    auto callback = target->on_drop;
                    callback(*target, payload);
                    break;
                }
            }
            return true;
        }
        if (w->kind == Kind::NumberField && dragging) {
            p.commit();
            return true;
        }
        if (w->kind == Kind::Divider && dragging) {
            auto callback = w->on_commit;
            if (callback)
                callback(*w);
            return true;
        }
        if (!w->rect.contains(event.x, event.y))
            return true;
        if (w->kind == Kind::Checkbox) {
            w->checked = !w->checked;
            auto callback = w->on_commit;
            if (callback)
                callback(*w);
            w = p.find(id);
            if (!w)
                return true;
        }
        if (w->kind == Kind::Button || w->kind == Kind::Tab || w->kind == Kind::TreeRow ||
            w->kind == Kind::Checkbox) {
            auto callback = w->on_click;
            if (callback)
                callback(*w);
        }
        return true;
    }
    if (event.type == Type::KeyDown && event.key == "Escape") {
        if (!p.captured.empty()) {
            p.cancel_capture();
            return true;
        }
        if (auto* w = p.find(p.focused); w && field(*w)) {
            auto& edit = p.edits[w->id];
            w->text = edit.original;
            w->value = edit.original_value;
            w->error.clear();
            edit.buffer.reset(edit.original);
            edit.composition.clear();
            auto callback = w->on_cancel;
            if (callback)
                callback(*w);
            p.unfocus(false);
            return true;
        }
        return false;
    }
    if (event.type == Type::KeyDown && event.key == "Tab") {
        std::vector<std::string> ids;
        collect_focus(p.root, ids);
        if (ids.empty())
            return false;
        auto current = std::find(ids.begin(), ids.end(), p.focused);
        std::size_t index =
            current == ids.end() ? 0 : static_cast<std::size_t>(current - ids.begin());
        if (current != ids.end())
            index = event.shift ? (index + ids.size() - 1) % ids.size() : (index + 1) % ids.size();
        return p.focus(ids[index]);
    }
    auto* w = p.find(p.focused);
    if (!w)
        return false;
    if (field(*w)) {
        auto& edit = p.edits[w->id];
        if (event.type == Type::TextEditing) {
            if (TextBuffer::valid_utf8(event.text)) {
                edit.composition = event.text;
                edit.composition_start = event.edit_start;
                edit.composition_length = event.edit_length;
            }
            return true;
        }
        if (event.type == Type::TextInput) {
            auto text = event.text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            std::replace(text.begin(), text.end(), '\r', ' ');
            edit.composition.clear();
            if (edit.buffer.insert(text))
                p.preview();
            return true;
        }
        if (event.type != Type::KeyDown)
            return false;
        if (event.control && (event.key == "A" || event.key == "a")) {
            edit.buffer.select_all();
            return true;
        }
        if (event.control &&
            (event.key == "C" || event.key == "c" || event.key == "X" || event.key == "x")) {
            if (p.clipboard_write && edit.buffer.has_selection())
                p.clipboard_write(edit.buffer.selected_text());
            if ((event.key == "X" || event.key == "x") && edit.buffer.has_selection()) {
                edit.buffer.insert("");
                p.preview();
            }
            return true;
        }
        if (event.control && (event.key == "V" || event.key == "v")) {
            if (p.clipboard_read) {
                auto text = p.clipboard_read();
                std::replace(text.begin(), text.end(), '\n', ' ');
                std::replace(text.begin(), text.end(), '\r', ' ');
                if (edit.buffer.insert(text))
                    p.preview();
            }
            return true;
        }
        if (event.control && (event.key == "Z" || event.key == "z")) {
            const auto changed = event.shift ? edit.buffer.redo() : edit.buffer.undo();
            if (changed)
                p.preview();
            return changed;
        }
        if (event.control && (event.key == "Y" || event.key == "y")) {
            const auto changed = edit.buffer.redo();
            if (changed)
                p.preview();
            return changed;
        }
        if (event.key == "Left") {
            edit.buffer.left(event.shift, event.control);
            return true;
        }
        if (event.key == "Right") {
            edit.buffer.right(event.shift, event.control);
            return true;
        }
        if (event.key == "Home") {
            edit.buffer.home(event.shift);
            return true;
        }
        if (event.key == "End") {
            edit.buffer.end(event.shift);
            return true;
        }
        if (event.key == "Backspace") {
            if (edit.buffer.backspace())
                p.preview();
            return true;
        }
        if (event.key == "Delete") {
            if (edit.buffer.delete_forward())
                p.preview();
            return true;
        }
        if (event.key == "Return" || event.key == "Enter") {
            if (p.commit()) {
                auto* current = p.find(p.focused);
                if (current && current->kind == Kind::NumberField)
                    p.edits[current->id].buffer.select_all();
            }
            return true;
        }
        return false;
    }
    if (event.type == Type::KeyDown && (event.key == "Return" || event.key == "Space")) {
        const auto id = w->id;
        if (w->kind == Kind::Checkbox) {
            w->checked = !w->checked;
            auto callback = w->on_commit;
            if (callback)
                callback(*w);
            w = p.find(id);
        }
        if (w) {
            auto callback = w->on_click;
            if (callback)
                callback(*w);
        }
        return true;
    }
    return false;
}
} // namespace faset::ui
