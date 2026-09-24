#include <faset/core/io.hpp>
#include <faset/editor/editor_ui.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace faset;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

render::Event mouse(render::Event::Type type, float x, float y, int button) {
    render::Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.button = button;
    return event;
}

render::Event key(std::string name) {
    render::Event event;
    event.type = render::Event::Type::KeyDown;
    event.key = std::move(name);
    return event;
}

ui::Rect visible_rect(editor::EditorUI& editor, const std::string& id) {
    const auto* widget = editor.widgets().find(id);
    check(widget && widget->visible, "Missing visible widget: " + id);
    const auto visible = widget->rect.intersection(widget->clip);
    check(visible.width > 2 && visible.height > 2, "Clipped widget: " + id);
    return visible;
}

void click(editor::EditorUI& editor, const std::string& id) {
    const auto rect = visible_rect(editor, id);
    const auto x = rect.x + rect.width * .5f;
    const auto y = rect.y + rect.height * .5f;
    editor.frame({mouse(render::Event::Type::MouseDown, x, y, 1),
                  mouse(render::Event::Type::MouseUp, x, y, 1)});
}

void right_click(editor::EditorUI& editor, float x, float y) {
    editor.frame({mouse(render::Event::Type::MouseDown, x, y, 3),
                  mouse(render::Event::Type::MouseUp, x, y, 3)});
}

void right_click(editor::EditorUI& editor, const std::string& id) {
    const auto rect = visible_rect(editor, id);
    right_click(editor, rect.x + rect.width * .5f, rect.y + rect.height * .5f);
}

void escape(editor::EditorUI& editor) {
    render::Event event;
    event.type = render::Event::Type::KeyDown;
    event.key = "Escape";
    editor.frame({event});
}

bool popup_open(editor::EditorUI& editor) {
    const auto* popup = editor.widgets().find("context-popup");
    return popup && popup->visible;
}

bool action_available(editor::EditorUI& editor, const std::string& id) {
    const auto* action = editor.widgets().find(id);
    return action && action->visible && action->enabled;
}

void object_actions(editor::EditorUI& editor, editor::Session& session) {
    click(editor, "add-cube");
    auto state = session.authoring().query(editor.current_document());
    check(state["scene"]["entities"].size() == 1, "Fixture cube must exist");
    const auto original = state["scene"]["entities"][0]["id"].get<std::string>();
    click(editor, "scene-root");
    check(editor.selected_entity().empty(), "Fixture scene root deselects cube");

    const auto entity_rect = visible_rect(editor, "entity-" + original);
    const float entity_x = entity_rect.x + entity_rect.width * .5f;
    const float entity_y = entity_rect.y + entity_rect.height * .5f;
    editor.frame({mouse(render::Event::Type::MouseDown, entity_x, entity_y, 3)});
    check(editor.selected_entity() == original, "Right MouseDown selects target entity");
    check(popup_open(editor), "Right MouseDown on entity opens context popup");
    editor.frame({mouse(render::Event::Type::MouseUp, entity_x, entity_y, 3)});
    check(action_available(editor, "context-duplicate") &&
              action_available(editor, "context-delete"),
          "Entity menu offers Duplicate and Delete");
    const auto before_duplicate = state.at("revision").get<std::uint64_t>();
    click(editor, "context-duplicate");
    state = session.authoring().query(editor.current_document());
    check(state["scene"]["entities"].size() == 2 &&
              state.at("revision") == before_duplicate + 1,
          "Duplicate authors exactly one new object in one transaction");
    check(!popup_open(editor), "Choosing an action closes context popup");
    click(editor, "undo");
    state = session.authoring().query(editor.current_document());
    check(state["scene"]["entities"].size() == 1 &&
              state["scene"]["entities"][0]["id"] == original,
          "One Undo reverses context Duplicate");

    right_click(editor, "entity-" + original);
    click(editor, "context-delete");
    check(session.authoring().query(editor.current_document())["scene"]["entities"].empty(),
          "Context Delete removes the selected object");
    click(editor, "undo");
    check(session.authoring().query(editor.current_document())["scene"]["entities"].size() ==
              1,
          "One Undo reverses context Delete");

    right_click(editor, "entity-" + original);
    escape(editor);
    check(!popup_open(editor), "Escape closes context popup");
    right_click(editor, "entity-" + original);
    const auto outside = visible_rect(editor, "statusbar");
    const float x = outside.x + outside.width * .5f;
    const float y = outside.y + outside.height * .5f;
    editor.frame({mouse(render::Event::Type::MouseDown, x, y, 1),
                  mouse(render::Event::Type::MouseUp, x, y, 1)});
    check(!popup_open(editor), "Clicking outside closes context popup");

    right_click(editor, "scene-root");
    check(popup_open(editor) && action_available(editor, "context-create-cube"),
          "Scene root offers creation actions");
    click(editor, "context-create-cube");
    state = session.authoring().query(editor.current_document());
    check(state["scene"]["entities"].size() == 2,
          "Scene root context action creates a cube");
    const auto created_revision = state.at("revision").get<std::uint64_t>();
    editor.frame({key("Return"), key("Space")});
    state = session.authoring().query(editor.current_document());
    check(!popup_open(editor) && state.at("revision") == created_revision &&
              state["scene"]["entities"].size() == 2,
          "Return and Space cannot reactivate a hidden context action");
    click(editor, "undo");
    check(session.authoring().query(editor.current_document())["scene"]["entities"].size() ==
              1,
          "Scene root creation is undoable");

    const auto tree = visible_rect(editor, "scene-tree");
    const auto row = visible_rect(editor, "entity-" + original);
    const float blank_y = tree.y + tree.height - 18;
    check(blank_y > row.y + row.height + 4, "Fixture scene tree needs blank space");
    right_click(editor, tree.x + tree.width * .5f, blank_y);
    check(popup_open(editor) && action_available(editor, "context-create-cube"),
          "Blank scene tree offers creation actions");
    escape(editor);
}

void inherited_child_actions(editor::EditorUI& editor, editor::Session& session,
                             const std::filesystem::path& root) {
    const auto source = session.authoring().query(editor.current_document());
    check(source["scene"]["entities"].size() == 1,
          "Template fixture starts with one local source object");
    const auto source_object = source["scene"]["entities"][0]["id"].get<std::string>();
    click(editor, "entity-" + source_object);
    click(editor, "menu-Scene");
    click(editor, "save-template");
    check(std::filesystem::exists(root / "Assets/Templates/Template.scene.json"),
          "Template fixture saves the selected object through the Scene menu");
    click(editor, "menu-File");
    click(editor, "new-3d");
    click(editor, "menu-Scene");
    click(editor, "instance-template");
    const auto document = editor.current_document();
    auto authored = session.authoring().query(document);
    check(authored["scene"]["instances"].size() == 1,
          "Template fixture instances the source into a new scene");
    const auto before = session.commands().resolved_scene(document);
    std::string inherited_id, inherited_source_id;
    for (const auto& object : before.at("scene").at("entities")) {
        const auto& origin = object.at("origin");
        if (origin.at("path").size() == 1 && !origin.value("local", false)) {
            inherited_id = object.at("id").get<std::string>();
            inherited_source_id = origin.at("object").get<std::string>();
        }
    }
    check(!inherited_id.empty() && inherited_source_id == source_object &&
              inherited_id != inherited_source_id,
          "Template fixture has an inherited entity with a scoped resolved ID");

    right_click(editor, "entity-" + inherited_id);
    check(action_available(editor, "context-add-child"),
          "Inherited object menu offers Add local child");
    click(editor, "context-add-child");
    authored = session.authoring().query(document);
    const auto& additions = authored["scene"]["instances"][0]["additions"];
    check(additions.size() == 1 && additions[0]["parent"] == inherited_source_id,
          "Local child stores its inherited parent's source ID");

    const auto after = session.commands().resolved_scene(document);
    for (const auto& conflict : after.at("conflicts"))
        check(conflict.at("code") != "addition.parent_missing",
              "Local child must not produce addition.parent_missing");
    const auto addition_source_id = additions[0]["id"].get<std::string>();
    bool resolved_child = false;
    for (const auto& object : after.at("scene").at("entities")) {
        const auto& origin = object.at("origin");
        if (origin.value("local", false) && origin.at("path").size() == 1 &&
            origin.at("object") == addition_source_id) {
            resolved_child = object.at("parent") == inherited_id;
            break;
        }
    }
    check(resolved_child, "Local child resolves under the clicked inherited entity");
}

void file_actions(editor::EditorUI& editor) {
    click(editor, "tab-assets");
    click(editor, "asset-refresh");
    right_click(editor, "file-Scripts/example.lua");
    check(popup_open(editor) && action_available(editor, "context-open-file"),
          "Lua file context menu offers Open");
    check(editor.widgets().find("file-Scripts/example.lua")->selected,
          "Right-click selects Lua file");
    escape(editor);
    right_click(editor, "file-Assets/model.gltf");
    check(popup_open(editor) && action_available(editor, "context-import"),
          "Importable source context menu offers Import");
    check(editor.widgets().find("file-Assets/model.gltf")->selected,
          "Right-click selects importable source");
    escape(editor);
}

void unavailable_asset_actions(editor::EditorUI& editor, editor::Session& session,
                               const std::filesystem::path& root) {
    atomic_write_json(root / "Assets/unavailable.gltf",
                      {{"asset", {{"version", "2.0"}}},
                       {"scene", 0},
                       {"scenes", Json::array({{{"nodes", {0}}}})},
                       {"nodes", Json::array({{{"name", "Imported fixture"}}})}});
    const auto job = session.commands()
                         .call("faset_import", {{"path", "Assets/unavailable.gltf"}})
                         .at("job")
                         .get<std::string>();
    Json imported;
    for (int attempt = 0; attempt < 500; ++attempt) {
        session.poll();
        imported = session.commands().call("faset_job", {{"id", job}});
        if (imported.at("state") != "queued" && imported.at("state") != "running")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(imported.at("state") == "succeeded", "Unavailable asset fixture first imports");
    const auto asset_id = imported.at("result").at("asset_id").get<std::string>();
    check(std::filesystem::remove(root / ".faset/cache/assets" / asset_id / "current.json"),
          "Unavailable asset fixture removes the active manifest pointer");
    click(editor, "asset-refresh");
    editor.frame({});
    const auto* unavailable = editor.widgets().find("asset-" + asset_id);
    check(unavailable && unavailable->text.starts_with("Unavailable"),
          "Asset browser retains an unavailable imported asset row");

    right_click(editor, "file-Assets/model.gltf");
    escape(editor);
    check(editor.widgets().find("file-Assets/model.gltf")->selected &&
              editor.widgets().find("asset-import")->enabled,
          "Previous importable file is selected before switching to unavailable asset");
    const auto previous_jobs =
        session.commands().call("faset_jobs", Json::object()).at("jobs").size();
    const auto document = editor.current_document();
    right_click(editor, "asset-" + asset_id);
    check(popup_open(editor) && action_available(editor, "context-copy-id") &&
              !action_available(editor, "context-import"),
          "Unavailable asset offers only safe context actions");
    check(editor.widgets().find("asset-" + asset_id)->selected &&
              !editor.widgets().find("file-Assets/model.gltf")->selected &&
              !editor.widgets().find("asset-import")->enabled &&
              !editor.widgets().find("asset-open")->enabled,
          "Right-click selects unavailable asset and clears the prior file toolbar actions");
    escape(editor);
    click(editor, "asset-import");
    click(editor, "asset-open");
    check(session.commands().call("faset_jobs", Json::object()).at("jobs").size() ==
                  previous_jobs &&
              editor.current_document() == document,
          "Disabled toolbar cannot act on the previously selected source file");
}

void viewport_actions(editor::EditorUI& editor) {
    const auto rect = editor.snapshot().scene_rect;
    const float x = rect[0] + rect[2] * .72f;
    const float y = rect[1] + rect[3] * .72f;
    right_click(editor, x, y);
    check(popup_open(editor), "Short viewport right-click opens context popup");
    escape(editor);
    const auto camera_before = editor.snapshot().view_projection;
    editor.frame({mouse(render::Event::Type::MouseDown, x, y, 3)});
    editor.frame({mouse(render::Event::Type::MouseMove, x + 48, y + 24, 0)});
    editor.frame({mouse(render::Event::Type::MouseUp, x + 48, y + 24, 3)});
    check(!popup_open(editor), "Right drag does not open context popup");
    check(editor.snapshot().view_projection != camera_before,
          "Right drag continues to orbit the viewport camera");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("faset-context-ui-" + new_id());
    try {
        atomic_write(root / "Scripts/example.lua", "function Update(dt) end\n");
        atomic_write_json(root / "Assets/model.gltf", {{"asset", {{"version", "2.0"}}}});
        editor::Session session({root, path_from_utf8(FASET_TEST_ENGINE), root});
        render::Renderer renderer({1280, 900, "Context menu acceptance", true, true});
        editor::EditorUI editor(session, renderer,
                                path_from_utf8(FASET_TEST_ENGINE) / "assets/fonts/NotoSans.ttf",
                                path_from_utf8(FASET_TEST_ENGINE) / "assets/ui/dark.json");
        editor.frame({});
        object_actions(editor, session);
        inherited_child_actions(editor, session, root);
        file_actions(editor);
        unavailable_asset_actions(editor, session, root);
        viewport_actions(editor);
        if (std::getenv("FASET_CONTEXT_CAPTURE"))
            right_click(editor, "scene-root");
        renderer.render(editor.snapshot());
        if (const auto* capture = std::getenv("FASET_CONTEXT_CAPTURE"))
            renderer.capture(path_from_utf8(capture));
        check(renderer.stats().validation_errors == 0, "Vulkan validation errors");
        std::cout << "Scene, asset and viewport right-click workflows passed\n";
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nRetained: " << path_to_utf8(root) << '\n';
        return 1;
    }
}
