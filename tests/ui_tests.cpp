#include <algorithm>
#include <chrono>
#include <cmath>
#include <faset/core/io.hpp>
#include <faset/ui/ui.hpp>
#include <iostream>
#include <stdexcept>
using namespace faset;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
render::Event key(std::string name, bool control = false, bool shift = false) {
    render::Event e;
    e.type = render::Event::Type::KeyDown;
    e.key = std::move(name);
    e.control = control;
    e.shift = shift;
    return e;
}
render::Event text(std::string value) {
    render::Event e;
    e.type = render::Event::Type::TextInput;
    e.text = std::move(value);
    return e;
}
render::Event mouse(render::Event::Type type, float x, float y) {
    render::Event e;
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = 1;
    return e;
}
void click(ui::Context& context, const ui::Widget& widget) {
    const auto r = widget.rect;
    context.handle(mouse(render::Event::Type::MouseDown, r.x + 5, r.y + 5));
    context.handle(mouse(render::Event::Type::MouseUp, r.x + 5, r.y + 5));
}
void display_scale_contract() {
    ui::Context context(path_from_utf8(FASET_TEST_FONT));
    auto& field = context.root().add(ui::Kind::TextField, "dpi-field", "Ж");
    field.layout.width = 180;
    field.layout.height = 32;
    auto& number = context.root().add(ui::Kind::NumberField, "dpi-number");
    number.layout.height = 32;
    number.value = 4;
    number.step = 1;
    auto& button = context.root().add(ui::Kind::Button, "dpi-action", "Open");
    button.layout.width = 180;
    button.layout.height = 32;
    int clicks = 0, commits = 0, number_commits = 0;
    button.on_click = [&](ui::Widget&) { ++clicks; };
    field.on_commit = [&](ui::Widget&) { ++commits; };
    number.on_commit = [&](ui::Widget&) { ++number_commits; };
    ui::Rect ime_area;
    context.set_ime([](bool) {}, [&](ui::Rect rect) { ime_area = rect; });
    context.layout(320, 240, 1);
    const auto initial = field.rect;
    render::Snapshot one;
    context.draw(one);
    const auto first_glyph = std::find_if(one.ui_quads.begin(), one.ui_quads.end(),
                                          [](const auto& q) { return bool(q.texture); });
    check(first_glyph != one.ui_quads.end(), "1x rasterized text missing");
    const auto glyph_width = first_glyph->width, glyph_height = first_glyph->height;
    const auto revision = context.font().texture()->revision;
    check(context.focus("dpi-field"), "DPI field focus");
    context.handle(key("A", true));
    context.handle(text("Живая сцена"));
    context.layout(640, 480, 2);
    check(field.rect.width == initial.width * 2 && field.rect.height == initial.height * 2 &&
              field.clip.width == initial.width * 2 && ime_area.width == field.rect.width,
          "2x layout, clipping and IME rectangle use drawable pixels");
    check(context.focused_id() == "dpi-field" && field.text == "Живая сцена" && commits == 0,
          "Live display scale preserves focused edit without committing");
    context.handle(key("Escape"));
    render::Snapshot two;
    context.draw(two);
    const auto second_glyph = std::find_if(two.ui_quads.begin(), two.ui_quads.end(),
                                           [](const auto& q) { return bool(q.texture); });
    check(second_glyph != two.ui_quads.end() && second_glyph->width >= glyph_width * 1.8f &&
              second_glyph->height >= glyph_height * 1.8f &&
              context.font().texture()->revision > revision,
          "2x display requests larger glyph rasterization");
    check(std::abs(second_glyph->width - (second_glyph->uv_rect[2] - second_glyph->uv_rect[0]) *
                                             second_glyph->texture->width) < .1f,
          "High-DPI glyphs use one atlas texel per pixel, not stretched 1x bitmaps");
    context.handle(mouse(render::Event::Type::MouseDown, button.rect.x + 300, button.rect.y + 40));
    context.handle(mouse(render::Event::Type::MouseUp, button.rect.x + 300, button.rect.y + 40));
    check(clicks == 1, "Hit testing reaches the second half of a scaled control");
    const auto r = number.rect;
    context.handle(mouse(render::Event::Type::MouseDown, r.x + 20, r.y + 20));
    context.handle(mouse(render::Event::Type::MouseMove, r.x + 40, r.y + 20));
    check(number.value == 14, "Numeric drag uses logical distance at 2x");
    context.layout(320, 240, 1);
    context.handle(mouse(render::Event::Type::MouseUp, 40, 52));
    check(number.value == 4 && number_commits == 0,
          "Monitor scale change cancels pointer drag without an accidental commit");
    check(field.rect.width == initial.width && button.layout.width == 180,
          "Returning to 1x preserves logical layout metrics");
    ui::Context scroll(path_from_utf8(FASET_TEST_FONT));
    auto& panel = scroll.root().add(ui::Kind::Column, "scroll");
    panel.layout.height = 80;
    panel.layout.scroll = true;
    panel.layout.gap = 0;
    for (int i = 0; i < 10; ++i)
        panel.add(ui::Kind::Button, "row-" + std::to_string(i), "Row").layout.height = 30;
    scroll.layout(200, 100, 1);
    panel.scroll_y = 30;
    scroll.layout(200, 100, 1);
    scroll.layout(400, 200, 2);
    check(panel.scroll_y == 60 && panel.children[1]->rect.y == panel.rect.y,
          "DPI changes retain the logical scroll position");
}
void keyboard_scroll_contract() {
    for (float scale : {1.f, 2.f}) {
        ui::Context context(path_from_utf8(FASET_TEST_FONT));
        auto& columns = context.root().add(ui::Kind::Row, "panels");
        columns.layout.height = 150;
        auto& inspector = columns.add(ui::Kind::Column, "inspector");
        inspector.layout.width = 200;
        inspector.layout.padding = 8;
        inspector.layout.gap = 4;
        inspector.layout.scroll = true;
        auto& assets = columns.add(ui::Kind::Column, "assets");
        assets.layout.width = 200;
        assets.layout.padding = 8;
        assets.layout.gap = 3;
        assets.layout.scroll = true;
        int commits = 0, activations = 0;
        for (int i = 0; i < 30; ++i) {
            auto& field = inspector.add(ui::Kind::TextField, "property-" + std::to_string(i),
                                        "Property " + std::to_string(i));
            field.layout.height = 30;
            field.on_commit = [&](ui::Widget&) { ++commits; };
            auto& asset = assets.add(ui::Kind::Button, "asset-" + std::to_string(i), "Model.glb");
            asset.layout.height = 28;
            asset.on_click = [&](ui::Widget&) { ++activations; };
        }
        ui::Rect ime;
        context.set_ime([](bool) {}, [&](ui::Rect rect) { ime = rect; });
        context.layout(440 * scale, 200 * scale, scale);
        check(context.find("property-29")->clip.height == 0 &&
                  context.find("asset-29")->clip.height == 0,
              "Long Inspector and Assets start with offscreen controls");
        auto visible = [&](const std::string& id) {
            const auto* widget = context.find(id);
            const auto r = widget->rect.intersection(widget->clip);
            check(context.focused_id() == id && std::abs(r.height - widget->rect.height) < .01f &&
                      std::abs(r.width - widget->rect.width) < .01f,
                  "Keyboard focus must reveal the entire control in its clipped panel");
        };
        for (int i = 0; i < 30; ++i) {
            context.handle(key("Tab"));
            visible("property-" + std::to_string(i));
            check(ime.y == context.find(context.focused_id())->rect.y,
                  "IME rectangle follows the newly scrolled field");
        }
        context.handle(key("A", true));
        context.handle(text("Изменено"));
        const float inspector_scroll = inspector.scroll_y;
        for (int i = 0; i < 30; ++i) {
            context.handle(key("Tab"));
            visible("asset-" + std::to_string(i));
            context.handle(key("Return"));
        }
        check(commits == 1 && activations == 30 && inspector.scroll_y == inspector_scroll,
              "Focus scroll commits one draft, activates assets and leaves unrelated panel alone");
        for (int i = 28; i >= 0; --i) {
            context.handle(key("Tab", false, true));
            visible("asset-" + std::to_string(i));
        }
        for (int i = 29; i >= 0; --i) {
            context.handle(key("Tab", false, true));
            visible("property-" + std::to_string(i));
        }
        check(inspector.scroll_y == 0 && assets.scroll_y == 0 && commits == 1,
              "Reverse keyboard traversal returns both panels to the top without edits");
        context.handle(text(" draft"));
        inspector.scroll_y = inspector_scroll;
        context.layout(440 * scale, 200 * scale, scale);
        check(context.find("property-0")->clip.height == 0,
              "A manual scroll may move an active field out of view");
        check(context.focus("property-0"), "Refocus active field");
        visible("property-0");
        check(context.find("property-0")->text == "Property 0 draft" && commits == 1,
              "Revealing the current focus preserves its uncommitted edit");
    }
    ui::Context nested(path_from_utf8(FASET_TEST_FONT));
    auto& outer = nested.root().add(ui::Kind::Column, "outer");
    outer.layout.height = 120;
    outer.layout.padding = 6;
    outer.layout.scroll = true;
    outer.add(ui::Kind::Label, "spacer").layout.height = 240;
    auto& inner = outer.add(ui::Kind::Column, "inner");
    inner.layout.height = 85;
    inner.layout.padding = 5;
    inner.layout.scroll = true;
    for (int i = 0; i < 15; ++i)
        inner.add(ui::Kind::TextField, "nested-" + std::to_string(i)).layout.height = 30;
    nested.layout(260, 160);
    check(nested.focus("nested-14"), "Focus deeply clipped control");
    auto* target = nested.find("nested-14");
    check(outer.scroll_y > 0 && inner.scroll_y > 0 && target->clip.height == target->rect.height,
          "Focus reveals a control through both initially clipped scroll ancestors");
    auto& invalid = inner.add(ui::Kind::NumberField, "invalid-number");
    invalid.layout.height = 30;
    nested.layout(260, 160);
    nested.focus("invalid-number");
    nested.handle(text("bad number"));
    const auto old_outer = outer.scroll_y, old_inner = inner.scroll_y;
    nested.handle(key("Tab"));
    check(nested.focused_id() == "invalid-number" && !invalid.error.empty() &&
              outer.scroll_y == old_outer && inner.scroll_y == old_inner,
          "Rejected numeric commit retains focus and scroll instead of hiding the error");
}
void right_click_context_contract() {
    ui::Context context(path_from_utf8(FASET_TEST_FONT));
    auto& surface = context.root().add(ui::Kind::Column, "context-surface");
    surface.layout.width = 180;
    surface.layout.height = 96;
    surface.layout.gap = 0;
    auto& direct = surface.add(ui::Kind::Button, "context-direct", "Direct");
    direct.layout.height = 24;
    auto& nested = surface.add(ui::Kind::Label, "context-nested", "Nested");
    nested.layout.height = 24;
    auto& disabled = surface.add(ui::Kind::Button, "context-disabled", "Disabled");
    disabled.layout.height = 24;
    disabled.enabled = false;
    auto& clipped = surface.add(ui::Kind::Button, "context-clipped", "Clipped");
    clipped.layout.absolute = true;
    clipped.layout.y = 120;
    clipped.layout.height = 24;

    int direct_context = 0, parent_context = 0, disabled_context = 0, clipped_context = 0;
    int ordinary_clicks = 0;
    float delivered_x = -1, delivered_y = -1;
    surface.on_context = [&](ui::Widget& target, float, float) {
        check(&target == &surface, "Context action must identify the ancestor that handles it");
        ++parent_context;
    };
    direct.on_context = [&](ui::Widget& target, float x, float y) {
        check(&target == &direct, "Context action must identify the nearest handler");
        ++direct_context;
        delivered_x = x;
        delivered_y = y;
    };
    direct.on_click = [&](ui::Widget&) { ++ordinary_clicks; };
    disabled.on_context = [&](ui::Widget&, float, float) { ++disabled_context; };
    disabled.on_click = [&](ui::Widget&) { ++ordinary_clicks; };
    clipped.on_context = [&](ui::Widget&, float, float) { ++clipped_context; };
    context.layout(240, 180);

    auto right_down = [&](const ui::Widget& widget) {
        auto event = mouse(render::Event::Type::MouseDown, widget.rect.x + 7,
                           widget.rect.y + 9);
        event.button = 3;
        return context.handle(event);
    };
    check(right_down(direct) && direct_context == 1 && parent_context == 0 &&
              delivered_x == direct.rect.x + 7 && delivered_y == direct.rect.y + 9,
          "Right click dispatches nearest handler with drawable coordinates");
    context.handle(mouse(render::Event::Type::MouseUp, direct.rect.x + 7, direct.rect.y + 9));
    check(ordinary_clicks == 0, "Right click must not start a left-button capture or click");

    check(right_down(nested) && parent_context == 1 && direct_context == 1,
          "Right click bubbles to the nearest ancestor with a context handler");
    right_down(disabled);
    check(disabled_context == 0 && parent_context == 1 && ordinary_clicks == 0,
          "Disabled widgets must not dispatch context actions");
    check(clipped.clip.height == 0, "Clipped context fixture must be outside its parent");
    right_down(clipped);
    check(clipped_context == 0 && parent_context == 1,
          "Clipped widgets must not receive context actions");
}
} // namespace
int main() {
    try {
        display_scale_contract();
        keyboard_scroll_contract();
        right_click_context_contract();
        ui::TextBuffer buffer("Привет");
        check(buffer.backspace() && buffer.text() == "Приве", "UTF-8 backspace split codepoint");
        check(buffer.undo() && buffer.text() == "Привет", "text undo");
        buffer.select_all();
        buffer.insert("Дверь");
        buffer.left(true);
        check(buffer.selected_text() == "ь", "UTF-8 selection");
        buffer.insert("ца");
        check(buffer.text() == "Дверца", "selection replacement");
        check(!buffer.insert(std::string("\xc0\x80", 2)), "overlong UTF-8 accepted");
        buffer.home();
        buffer.delete_forward();
        check(buffer.text() == "верца", "UTF-8 delete");
        buffer.undo();
        check(buffer.text() == "Дверца", "undo delete");
        const auto missing_font = std::filesystem::temp_directory_path() /
                                  path_from_utf8("Недоступный шрифт-" + new_id() + ".ttf");
        bool unicode_font_diagnostic = false;
        try {
            ui::FontAtlas missing(missing_font);
        } catch (const std::exception& error) {
            unicode_font_diagnostic =
                std::string(error.what()).find(path_to_utf8(missing_font)) != std::string::npos;
        }
        check(unicode_font_diagnostic, "Font errors preserve the UTF-8 path");
        const auto unicode_font_directory =
            std::filesystem::temp_directory_path() / path_from_utf8("Faset шрифты-" + new_id()) /
            std::string(80, 'a') / std::string(80, 'b') / std::string(80, 'c');
        std::filesystem::create_directories(unicode_font_directory);
        const auto unicode_font_path =
            unicode_font_directory / path_from_utf8("Основной шрифт.ttf");
        std::filesystem::copy_file(path_from_utf8(FASET_TEST_FONT), unicode_font_path);
        {
            ui::FontAtlas unicode_font(unicode_font_path);
            check(std::filesystem::remove(unicode_font_path),
                  "Unicode font file must be closed after loading");
            check(unicode_font.measure("Живая сцена", 18) > 0,
                  "Shaping uses retained font bytes after Unicode source file is removed");
            render::Snapshot glyphs;
            unicode_font.draw(glyphs, "Живая сцена", 0, 0, 18, {1, 1, 1, 1}, {0, 0, 400, 40});
            check(!glyphs.ui_quads.empty() && unicode_font.texture()->revision > 1,
                  "Rasterization remains valid after Unicode font source disappears");
        }
        std::filesystem::remove_all(
            unicode_font_directory.parent_path().parent_path().parent_path());
        ui::Context context(path_from_utf8(FASET_TEST_FONT));
        auto& root = context.root();
        root.layout.gap = 4;
        auto& name = root.add(ui::Kind::TextField, "name", "Door");
        name.layout.height = 32;
        int commits = 0;
        name.on_commit = [&](ui::Widget&) { ++commits; };
        auto& number = root.add(ui::Kind::NumberField, "number");
        number.value = 4;
        number.step = .5;
        number.layout.height = 32;
        int number_commits = 0, previews = 0;
        number.on_commit = [&](ui::Widget&) { ++number_commits; };
        number.on_preview = [&](ui::Widget&) { ++previews; };
        auto& checkbox = root.add(ui::Kind::Checkbox, "enabled", "Enabled");
        int checks = 0;
        checkbox.on_commit = [&](ui::Widget&) { ++checks; };
        auto& button = root.add(ui::Kind::Button, "save", "Save");
        int clicks = 0;
        button.on_click = [&](ui::Widget&) { ++clicks; };
        context.layout(320, 240);
        std::string clipboard;
        context.set_clipboard([&] { return clipboard; },
                              [&](const std::string& s) { clipboard = s; });
        bool ime = false;
        int ime_rectangles = 0;
        context.set_ime([&](bool enabled) { ime = enabled; },
                        [&](ui::Rect r) {
                            check(r.width > 0, "IME area");
                            ++ime_rectangles;
                        });
        check(context.focus("name") && ime, "field focus enables IME");
        context.handle(key("A", true));
        context.handle(text("Привет"));
        check(!context.update_text("name", "External"), "document refresh erased dirty edit");
        context.handle(key("C", true));
        context.handle(key("A", true));
        context.handle(key("C", true));
        check(clipboard == "Привет", "clipboard copied wrong selection");
        clipboard = "Дверь";
        context.handle(key("V", true));
        check(name.text == "Дверь", "UTF-8 paste");
        context.handle(key("Z", true));
        check(name.text == "Привет", "local Ctrl-Z");
        context.handle(key("Return"));
        check(commits == 1 && name.text == "Привет", "text commits once");
        context.handle(key("Return"));
        check(commits == 1, "unchanged text recommitted");
        context.handle(key("End"));
        render::Event composition;
        composition.type = render::Event::Type::TextEditing;
        composition.text = "й";
        composition.edit_length = 1;
        context.handle(composition);
        check(name.text == "Привет", "IME preedit changed document field");
        context.handle(text("й"));
        context.handle(key("Return"));
        check(name.text == "Приветй" && commits == 2, "IME commit");
        context.focus("number");
        const auto r = number.rect;
        context.handle(mouse(render::Event::Type::MouseDown, r.x + 20, r.y + 12));
        context.handle(mouse(render::Event::Type::MouseMove, r.x + 40, r.y + 12));
        context.handle(mouse(render::Event::Type::MouseMove, r.x + 50, r.y + 12));
        context.handle(mouse(render::Event::Type::MouseUp, r.x + 50, r.y + 12));
        check(number.value == 19 && number_commits == 1 && previews == 2,
              "numeric drag must commit once at release");
        context.focus("save");
        check(number_commits == 1, "blur duplicated drag commit");
        context.focus("number");
        context.handle(key("A", true));
        context.handle(text("NaN"));
        context.handle(key("Return"));
        check(number_commits == 1 && !number.error.empty(), "invalid numeric input committed");
        context.handle(key("Escape"));
        check(number.value == 19, "cancel numeric input changed value");
        click(context, checkbox);
        check(checkbox.checked && checks == 1, "checkbox commit");
        click(context, button);
        check(clicks == 1, "button click");
        context.handle(key("Return"));
        check(clicks == 2, "keyboard button activation");
        context.handle(key("Tab"));
        check(context.focused_id() == "name", "focus wraps predictably");
        check(ime_rectangles > 0, "IME rectangle never sent");
        ui::Context split(path_from_utf8(FASET_TEST_FONT));
        auto& row = split.root().add(ui::Kind::Row, "row");
        row.layout.flex = 1;
        row.layout.gap = 0;
        auto& left = row.add(ui::Kind::Panel, "left");
        left.layout.width = 100;
        left.layout.min_width = 50;
        auto& divider = row.add(ui::Kind::Divider, "divider");
        divider.layout.width = 5;
        auto& right = row.add(ui::Kind::Panel, "right");
        right.layout.flex = 1;
        right.layout.min_width = 50;
        int resize_commit = 0;
        divider.on_commit = [&](ui::Widget&) { ++resize_commit; };
        split.layout(300, 100);
        const auto d = divider.rect;
        split.handle(mouse(render::Event::Type::MouseDown, d.x + 2, 20));
        split.handle(mouse(render::Event::Type::MouseMove, d.x + 32, 20));
        split.handle(mouse(render::Event::Type::MouseUp, d.x + 32, 20));
        check(left.layout.width == 130 && right.rect.width == 165 && resize_commit == 1,
              "divider resize");
        const auto moved_divider = divider.rect;
        split.handle(mouse(render::Event::Type::MouseDown, moved_divider.x + 2, 20));
        split.handle(mouse(render::Event::Type::MouseMove, moved_divider.x + 42, 20));
        split.handle(key("Escape"));
        check(left.layout.width == 130 && resize_commit == 1,
              "Escape must restore divider without committing");
        int cancellations = 0;
        number.on_cancel = [&](ui::Widget&) { ++cancellations; };
        context.handle(mouse(render::Event::Type::MouseDown, r.x + 10, r.y + 12));
        context.handle(mouse(render::Event::Type::MouseMove, r.x + 50, r.y + 12));
        context.handle(key("Escape"));
        check(number.value == 19 && number_commits == 1 && cancellations == 1,
              "Escape must cancel numeric drag");
        ui::Context scrolling(path_from_utf8(FASET_TEST_FONT));
        auto& list = scrolling.root().add(ui::Kind::Panel, "list");
        list.layout.height = 80;
        list.layout.scroll = true;
        list.layout.gap = 0;
        for (int i = 0; i < 10; ++i)
            list.add(ui::Kind::Label, "row" + std::to_string(i), "Объект " + std::to_string(i));
        scrolling.layout(300, 200);
        scrolling.handle(mouse(render::Event::Type::MouseMove, 30, 30));
        render::Event wheel;
        wheel.type = render::Event::Type::Wheel;
        wheel.y = -2;
        check(scrolling.handle(wheel) && list.scroll_y > 0, "scroll event");
        render::Snapshot frame;
        scrolling.draw(frame);
        check(!frame.ui_quads.empty(), "retained UI emitted no quads");
        for (const auto& q : frame.ui_quads)
            check(q.x >= 0 && q.y >= 0 && q.x + q.width <= 300.01f && q.y + q.height <= 80.01f,
                  "CPU clipping escaped scroll panel");
        check(scrolling.font().measure("Привет", 14) > 20, "Cyrillic shaping failed");
        ui::DockLayout docks;
        docks.move("Scene", "left", 0);
        docks.move("Assets", "left", 1);
        docks.move("Assets", "left", 0);
        docks.set_size("Scene", 224);
        check(docks.panels("left") == std::vector<std::string>({"Assets", "Scene"}),
              "dock reordering");
        const auto state = docks.to_json();
        ui::DockLayout restored;
        restored.from_json(state);
        check(restored.to_json() == state, "dock serialization");
        auto invalid = state;
        invalid["areas"]["right"] = {"Scene"};
        bool rejected = false;
        try {
            restored.from_json(invalid);
        } catch (...) {
            rejected = true;
        }
        check(rejected && restored.to_json() == state, "invalid dock load mutated existing layout");
        ui::Context declarative(path_from_utf8(FASET_TEST_FONT));
        declarative.apply_layout({{"id", "root"},
                                  {"kind", "column"},
                                  {"children", ui::Json::array({{{"id", "run"},
                                                                 {"kind", "button"},
                                                                 {"text", "Play"},
                                                                 {"layout", {{"height", 30}}}}})}});
        int action = 0;
        declarative.find("run")->on_click = [&](ui::Widget&) { ++action; };
        declarative.apply_layout({{"id", "root"},
                                  {"kind", "column"},
                                  {"children", ui::Json::array({{{"id", "run"},
                                                                 {"kind", "button"},
                                                                 {"text", "Run"},
                                                                 {"layout", {{"height", 34}}}}})}});
        declarative.layout(200, 100);
        click(declarative, *declarative.find("run"));
        check(action == 1 && declarative.find("run")->text == "Run", "layout reload lost callback");
        const auto initial_height = declarative.find("run")->layout.height;
        const auto invalid_patch =
            ui::Json{{"id", "root"},
                     {"children",
                      ui::Json::array({{{"id", "run"}, {"layout", {{"height", 99}}}},
                                       {{"id", "bad-label"}, {"kind", "label"}, {"text", 123}}})}};
        rejected = false;
        try {
            declarative.apply_layout(invalid_patch);
        } catch (...) {
            rejected = true;
        }
        check(rejected && declarative.find("run")->layout.height == initial_height &&
                  !declarative.find("bad-label"),
              "Layout validation must reject all changes before mutation");
        rejected = false;
        try {
            declarative.apply_layout(
                {{"id", "root"},
                 {"children",
                  ui::Json::array({{{"id", "new-parent"},
                                    {"kind", "column"},
                                    {"children", ui::Json::array({{{"id", "run"}}})}}})}});
        } catch (...) {
            rejected = true;
        }
        check(rejected && !declarative.find("new-parent"),
              "Hot layout must not duplicate/reparent a retained widget ID");
        std::cout << "UI: UTF-8, shaping, text/IME/clipboard, focus, transactions, "
                     "layout, clipping, docking OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
