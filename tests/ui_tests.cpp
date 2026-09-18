#include <chrono>
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
} // namespace
int main() {
    try {
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
        ui::Context context(FASET_TEST_FONT);
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
        ui::Context split(FASET_TEST_FONT);
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
        ui::Context scrolling(FASET_TEST_FONT);
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
        ui::Context declarative(FASET_TEST_FONT);
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
