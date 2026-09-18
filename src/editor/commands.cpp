#include <algorithm>
#include <faset/authoring/templates.hpp>
#include <faset/core/io.hpp>
#include <faset/editor/commands.hpp>
#include <set>

namespace faset::editor {
Json Commands::object_schema(Json properties, Json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}
void Commands::add(std::string name, std::string description, Json schema, Handler handler,
                   bool read_only) {
    require(!commands_.contains(name), "command.duplicate", "Command already registered: " + name);
    Json descriptor = {{"name", name},
                       {"description", std::move(description)},
                       {"inputSchema", std::move(schema)},
                       {"annotations", {{"readOnlyHint", read_only}, {"openWorldHint", false}}}};
    commands_.emplace(std::move(name), Command{std::move(descriptor), std::move(handler)});
}
void Commands::remove(const std::string& name) {
    commands_.erase(name);
}
Json Commands::list() const {
    Json result = Json::array();
    for (const auto& [name, command] : commands_)
        result.push_back(command.descriptor);
    return result;
}
Json Commands::call(const std::string& name, const Json& arguments) {
    const auto found = commands_.find(name);
    require(found != commands_.end(), "command.unknown", "Unknown editor command: " + name);
    require(arguments.is_object(), "arguments.object", "Tool arguments must be an object");
    const auto& schema = found->second.descriptor["inputSchema"];
    for (const auto& key : schema.value("required", Json::array()))
        require(arguments.contains(key.get<std::string>()), "arguments.required",
                "Missing argument: " + key.get<std::string>());
    for (const auto& [key, value] : arguments.items()) {
        require(schema["properties"].contains(key), "arguments.unknown",
                "Unknown argument: " + key);
        const auto& property = schema["properties"][key];
        const auto type = property.value("type", std::string());
        const bool valid =
            type.empty() || (type == "string" && value.is_string()) ||
            (type == "boolean" && value.is_boolean()) || (type == "number" && value.is_number()) ||
            (type == "integer" && value.is_number_integer()) ||
            (type == "object" && value.is_object()) || (type == "array" && value.is_array());
        require(valid, "arguments.type", "Invalid type for argument: " + key);
        if (property.contains("enum"))
            require(std::find(property["enum"].begin(), property["enum"].end(), value) !=
                        property["enum"].end(),
                    "arguments.enum", "Unsupported value for argument: " + key);
        if (property.contains("maximum") && value.is_number())
            require(value.get<double>() <= property["maximum"].get<double>(), "arguments.maximum",
                    "Argument exceeds its maximum: " + key);
        if (property.contains("minimum") && value.is_number())
            require(value.get<double>() >= property["minimum"].get<double>(), "arguments.minimum",
                    "Argument is below its minimum: " + key);
    }
    try {
        return found->second.handler(arguments);
    } catch (const Json::exception& error) {
        throw Error("arguments.invalid", "Invalid command data", {{"reason", error.what()}});
    }
}
Json Commands::resolved_scene(const std::string& id) const {
    const auto result = authoring::resolve_templates(
        authoring_.query(id).at("scene"), authoring_.schemas(), [&](const std::string& path) {
            const auto relative = path_from_utf8(path).lexically_normal();
            for (const auto& document : authoring_.documents())
                if (document.at("path") == generic_path_to_utf8(relative))
                    return authoring_.query(document.at("id")).at("scene");
            return read_json(project_path(authoring_.root(), relative));
        });
    return {{"scene", result.scene}, {"conflicts", result.conflicts}};
}
Commands::Commands(authoring::AuthoringService& authoring) : authoring_(authoring) {
    const Json text = {{"type", "string"}}, integer = {{"type", "integer"}, {"minimum", 0}},
               boolean = {{"type", "boolean"}};
    add(
        "faset_documents",
        "List open authoring documents and their revisions. This does not inspect a running game.",
        object_schema(Json::object()),
        [&](const Json&) { return Json{{"documents", authoring_.documents()}}; }, true);
    add("faset_document_create",
        "Create an unsaved 2D or 3D scene. Returns its persistent document ID and revision.",
        object_schema({{"name", text}, {"dimension", integer}}, {"name", "dimension"}),
        [&](const Json& args) { return authoring_.create(args.at("name"), args.at("dimension")); });
    add("faset_document_open",
        "Open a scene relative to the project. Set recover=true to load its saved recovery "
        "journal.",
        object_schema({{"path", text}, {"recover", boolean}}, {"path"}), [&](const Json& args) {
            return authoring_.open(path_from_utf8(args.at("path").get<std::string>()),
                                   args.value("recover", false));
        });
    add(
        "faset_document_query",
        "Read an authoring scene, persistent IDs, dirty state and revision. No Player or runtime "
        "state is exposed.",
        object_schema({{"document", text}}, {"document"}),
        [&](const Json& args) { return authoring_.query(args.at("document")); }, true);
    add("faset_document_save",
        "Atomically save an authoring document. Refuses to overwrite an externally modified file.",
        object_schema({{"document", text}, {"path", text}}, {"document"}), [&](const Json& args) {
            return authoring_.save(args.at("document"),
                                   path_from_utf8(args.value("path", std::string())));
        });
    add(
        "faset_schema",
        "Inspect registered component TypeIds, stable FieldIds, defaults and constraints.",
        object_schema(Json::object()), [&](const Json&) { return authoring_.schemas().manifest(); },
        true);
    add("faset_scene_edit",
        "Apply one atomic authoring batch with optimistic revision checking and one Undo step. "
        "Operations: entity.create/rename/delete/duplicate/reparent; "
        "component.add/remove/set/migrate; "
        "scene.rename/simulation; "
        "template.instance/override/revert/suppress/restore/add/addition_set/reparent/remove/"
        "source_set. Use "
        "persistent IDs "
        "from document_query and schema. An idempotency_key retries the same payload in this "
        "session.",
        object_schema({{"document", text},
                       {"revision", integer},
                       {"operations", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                       {"idempotency_key", text}},
                      {"document", "revision", "operations"}),
        [&](const Json& args) {
            return authoring_.transact(args.at("document"), args.at("revision"),
                                       args.at("operations"),
                                       args.value("idempotency_key", std::string()));
        });
    add("faset_undo", "Undo one authoring transaction. Requires the current document revision.",
        object_schema({{"document", text}, {"revision", integer}}, {"document", "revision"}),
        [&](const Json& args) {
            return authoring_.undo(args.at("document"), args.at("revision"));
        });
    add("faset_redo", "Redo one authoring transaction. Requires the current document revision.",
        object_schema({{"document", text}, {"revision", integer}}, {"document", "revision"}),
        [&](const Json& args) {
            return authoring_.redo(args.at("document"), args.at("revision"));
        });
    add(
        "faset_template_preview",
        "Resolve authoring templates and report conflicts without changing source documents. This "
        "is not a live game query.",
        object_schema({{"document", text}}, {"document"}),
        [&](const Json& args) { return resolved_scene(args.at("document")); }, true);
    add(
        "faset_simulation_get",
        "Read scene simulation settings with defaults: fixed_delta seconds, max_catch_up_ticks, "
        "physics_substeps and gravity.",
        object_schema({{"document", text}}, {"document"}),
        [&](const Json& args) {
            const auto document = authoring_.query(args.at("document"));
            auto settings = authoring::default_simulation_settings();
            settings.update(document.at("scene").value("simulation", Json::object()));
            return Json{{"document", document.at("id")},
                        {"revision", document.at("revision")},
                        {"settings", settings}};
        },
        true);
    add("faset_simulation_set",
        "Update scene simulation settings as one Undo transaction. Values apply when the Player "
        "next starts.",
        object_schema(
            {{"document", text}, {"revision", integer}, {"settings", {{"type", "object"}}}},
            {"document", "revision", "settings"}),
        [&](const Json& args) {
            return authoring_.transact(
                args.at("document"), args.at("revision"),
                Json::array({{{"op", "scene.simulation"}, {"value", args.at("settings")}}}));
        });
    add("faset_recovery_restore",
        "Restore a recovery journal, including an unsaved new scene. If the document is already "
        "open, pass its current revision. External file changes are never overwritten.",
        object_schema({{"document", text}, {"revision", integer}}, {"document"}),
        [&](const Json& args) {
            return authoring_.recover(
                args.at("document"),
                args.contains("revision")
                    ? std::optional<std::uint64_t>(args.at("revision").get<std::uint64_t>())
                    : std::nullopt);
        });
    add(
        "faset_recovery_list", "List document recovery records in this project.",
        object_schema(Json::object()),
        [&](const Json&) { return Json{{"recovery", authoring_.recovery_documents()}}; }, true);
}
} // namespace faset::editor
