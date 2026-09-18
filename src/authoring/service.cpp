#include <algorithm>
#include <cmath>
#include <faset/authoring/service.hpp>
#include <faset/authoring/transforms.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <set>

namespace faset::authoring {
namespace {
Json& entity(Json& scene, const std::string& id) {
    for (auto& item : scene["entities"])
        if (item.at("id") == id)
            return item;
    throw Error("entity.missing", "Entity does not exist", {{"entity", id}});
}
Json& component(Json& item, const std::string& id) {
    for (auto& value : item["components"])
        if (value.at("id") == id)
            return value;
    throw Error("component.missing", "Component does not exist", {{"component", id}});
}
std::string parent_id(const Json& item) {
    return item.contains("parent") && !item["parent"].is_null() ? item["parent"].get<std::string>()
                                                                : "";
}
void check_revision(std::uint64_t current, std::uint64_t expected) {
    if (current != expected)
        throw Error("revision.conflict", "Document changed since it was read",
                    {{"expected", expected}, {"current", current}});
}
bool finite_json(const Json& value) {
    if (value.is_number_float())
        return std::isfinite(value.get<double>());
    if (value.is_structured())
        for (const auto& child : value)
            if (!finite_json(child))
                return false;
    return true;
}
bool valid_id(const Json& value) {
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 128 &&
           text.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") ==
               std::string::npos;
}
} // namespace
Json default_simulation_settings() {
    return {{"fixed_delta", 1.0 / 60.0},
            {"max_catch_up_ticks", 4},
            {"physics_substeps", 4},
            {"gravity", {0, -9.81, 0}}};
}
Json make_scene(std::string name, int dimension) {
    require(dimension == 2 || dimension == 3, "scene.dimension", "Scene dimension must be 2 or 3");
    return {{"format", "faset.scene"},   {"version", 1},           {"id", new_id()},
            {"name", std::move(name)},   {"dimension", dimension}, {"entities", Json::array()},
            {"instances", Json::array()}};
}
Json make_entity(const SchemaRegistry& schemas, std::string name, const std::string& parent) {
    Json transform = {{"id", new_id()},
                      {"type", "faset.transform"},
                      {"version", 1},
                      {"fields", schemas.default_fields("faset.transform")}};
    return {{"id", new_id()},
            {"name", std::move(name)},
            {"parent", parent.empty() ? Json(nullptr) : Json(parent)},
            {"components", Json::array({transform})}};
}
void validate_scene(const Json& scene, const SchemaRegistry& schemas) {
    require(scene.is_object() && scene.value("format", std::string()) == "faset.scene",
            "scene.format", "Expected a Faset scene");
    require(scene.value("version", 0) == 1, "scene.version", "Unsupported scene format version");
    require(scene.contains("id") && valid_id(scene["id"]), "scene.id",
            "Scene requires a safe stable ID");
    require(scene.contains("name") && scene["name"].is_string(), "scene.name",
            "Scene name must be text");
    require(scene.value("dimension", 0) == 2 || scene.value("dimension", 0) == 3, "scene.dimension",
            "Scene dimension must be 2 or 3");
    require(scene.contains("entities") && scene["entities"].is_array(), "scene.entities",
            "Scene entities must be an array");
    require(finite_json(scene), "validation.finite", "Scene contains a non-finite number");
    if (scene.contains("simulation")) {
        const auto& settings = scene.at("simulation");
        require(settings.is_object(), "simulation.object", "Simulation settings must be an object");
        if (settings.contains("fixed_delta"))
            require(settings["fixed_delta"].is_number() &&
                        settings["fixed_delta"].get<double>() > 0 &&
                        settings["fixed_delta"].get<double>() <= 1,
                    "simulation.fixed_delta",
                    "Fixed delta must be greater than zero and at most one second");
        for (const auto& [name, maximum] :
             std::map<std::string, int>{{"max_catch_up_ticks", 1024}, {"physics_substeps", 128}})
            if (settings.contains(name)) {
                const auto& value = settings[name];
                require(value.is_number_integer() && value.get<double>() >= 1 &&
                            value.get<double>() <= maximum,
                        "simulation.integer", "Invalid simulation setting: " + name);
            }
        if (settings.contains("gravity"))
            validate_field(settings["gravity"], {{"type", "vec3"}});
    }
    std::set<std::string> ids;
    std::map<std::string, std::string> parents;
    auto insert_id = [&](const Json& value) {
        require(valid_id(value), "id.invalid",
                "ID must contain at most 128 ASCII identifier characters");
        require(ids.insert(value.get<std::string>()).second, "id.duplicate",
                "Duplicate document ID");
    };
    for (const auto& item : scene["entities"]) {
        require(item.is_object() && item.contains("id") && item.contains("name") &&
                    item["name"].is_string(),
                "entity.invalid", "Invalid entity record");
        insert_id(item["id"]);
        parents[item["id"].get<std::string>()] = parent_id(item);
        require(item.contains("components") && item["components"].is_array(), "entity.components",
                "Entity components must be an array");
        std::set<std::string> types;
        for (const auto& value : item["components"]) {
            require(value.contains("id"), "component.id", "Component requires stable ID");
            insert_id(value["id"]);
            schemas.validate_component(value);
            require(types.insert(value.at("type").get<std::string>()).second,
                    "component.duplicate_type",
                    "One component of each type is supported per entity");
        }
    }
    for (const auto& [id, parent] : parents) {
        std::set<std::string> visited{id};
        auto current = parent;
        while (!current.empty()) {
            require(parents.contains(current), "entity.parent_missing", "Parent entity is missing");
            require(visited.insert(current).second, "entity.cycle", "Hierarchy contains a cycle");
            current = parents.at(current);
        }
    }
    if (scene.contains("instances")) {
        require(scene["instances"].is_array(), "template.instances",
                "Template instances must be an array");
        for (const auto& instance : scene["instances"]) {
            require(instance.contains("id") && instance.contains("source") &&
                        instance["source"].is_string(),
                    "template.instance", "Invalid template instance");
            insert_id(instance["id"]);
            require(!instance["source"].get_ref<const std::string&>().empty(), "template.source",
                    "Template source cannot be empty");
            auto address = [&](const Json& value, bool field) {
                require(value.is_object() && value.contains("object") && valid_id(value["object"]),
                        "template.address", "Template address needs a stable object ID");
                if (value.contains("path")) {
                    require(value["path"].is_array(), "template.address",
                            "Instance path must be an array");
                    for (const auto& part : value["path"])
                        require(valid_id(part), "template.address",
                                "Instance path needs stable IDs");
                }
                if (field)
                    require(value.contains("component") && valid_id(value["component"]) &&
                                value.contains("field") && value["field"].is_string() &&
                                !value["field"].get_ref<const std::string&>().empty(),
                            "template.address",
                            "Field address needs stable component and field IDs");
            };
            for (const auto* collection : {"overrides", "suppressed", "reparents"})
                if (instance.contains(collection))
                    require(instance[collection].is_array(), "template.records",
                            "Template records must be an array");
            for (const auto& record : instance.value("overrides", Json::array())) {
                require(record.is_object() && record.contains("address") &&
                            record.contains("value"),
                        "template.override", "Override needs an address and value");
                address(record["address"], true);
            }
            for (const auto& record : instance.value("suppressed", Json::array()))
                address(record, false);
            for (const auto& record : instance.value("reparents", Json::array())) {
                require(record.is_object() && record.contains("object") &&
                            record.contains("parent"),
                        "template.reparent", "Reparent needs object and parent addresses");
                address(record["object"], false);
                if (!record["parent"].is_null())
                    address(record["parent"], false);
                if (record.contains("keep_world"))
                    require(record["keep_world"].is_boolean(), "template.reparent",
                            "keep_world must be a boolean");
            }

            if (instance.contains("additions")) {
                require(instance["additions"].is_array(), "template.additions",
                        "Local additions must be an array");
                auto additions = make_scene("Local additions");
                additions["entities"] = instance["additions"];
                // Local cycles are rejected here; parents in the source are checked during
                // resolution.
                std::set<std::string> local_ids;
                for (const auto& item : additions["entities"])
                    if (item.contains("id") && item["id"].is_string())
                        local_ids.insert(item["id"].get<std::string>());
                for (auto& item : additions["entities"]) {
                    if (item.contains("parent") && !item["parent"].is_null())
                        require(valid_id(item["parent"]), "template.addition_parent",
                                "Local parent must be a stable object ID or null");
                    if (!item.contains("parent") || item["parent"].is_null() ||
                        !local_ids.contains(item["parent"].get<std::string>()))
                        item["parent"] = nullptr;
                }
                validate_scene(additions, schemas);
            }
        }
    }
}
AuthoringService::AuthoringService(std::filesystem::path root, SchemaRegistry schemas)
    : root_(std::filesystem::absolute(std::move(root)).lexically_normal()),
      schemas_(std::move(schemas)) {
    std::filesystem::create_directories(root_);
}
AuthoringService::State& AuthoringService::state(const std::string& id) {
    auto found = documents_.find(id);
    require(found != documents_.end(), "document.missing", "Document is not open");
    return found->second;
}
const AuthoringService::State& AuthoringService::state(const std::string& id) const {
    auto found = documents_.find(id);
    require(found != documents_.end(), "document.missing", "Document is not open");
    return found->second;
}
Json AuthoringService::summary(const State& value, bool include_data) const {
    Json result = {{"id", value.data.at("id")},
                   {"name", value.data.at("name")},
                   {"revision", value.revision},
                   {"dirty", sha256(value.data.dump()) != value.saved_hash},
                   {"path", value.path.generic_string()},
                   {"can_undo", !value.undo.empty()},
                   {"can_redo", !value.redo.empty()}};
    if (include_data)
        result["scene"] = value.data;
    return result;
}
void AuthoringService::journal(const State& value) const {
    atomic_write_json(project_path(root_, std::filesystem::path(".faset/recovery") /
                                              (value.data.at("id").get<std::string>() + ".json")),
                      {{"format", "faset.recovery"},
                       {"version", 1},
                       {"path", value.path.generic_string()},
                       {"revision", value.revision},
                       {"saved_hash", value.saved_hash},
                       {"disk_hash", value.disk_hash},
                       {"scene", value.data}});
}
Json AuthoringService::create(std::string name, int dimension) {
    std::lock_guard lock(mutex_);
    State value;
    value.data = make_scene(std::move(name), dimension);
    journal(value);
    const auto id = value.data["id"].get<std::string>();
    documents_.emplace(id, std::move(value));
    return summary(state(id));
}
Json AuthoringService::open(const std::filesystem::path& relative, bool recover) {
    std::lock_guard lock(mutex_);
    auto path = project_path(root_, relative);
    Json data = read_json(path);
    validate_scene(data, schemas_);
    const auto id = data.at("id").get<std::string>();
    if (documents_.contains(id)) {
        require(state(id).path == relative.lexically_normal(), "document.id_collision",
                "Another open file has the same document ID");
        return recover ? this->recover(id, state(id).revision) : summary(state(id));
    }
    State value;
    value.data = data;
    value.path = relative.lexically_normal();
    value.saved_hash = sha256(data.dump());
    value.disk_hash = sha256_file(path);
    const auto recovery =
        project_path(root_, std::filesystem::path(".faset/recovery") / (id + ".json"));
    if (recover && std::filesystem::exists(recovery)) {
        const auto recovered = read_json(recovery);
        require(recovered.value("disk_hash", std::string()) == value.disk_hash,
                "recovery.disk_conflict", "Scene file changed since recovery was written");
        validate_scene(recovered.at("scene"), schemas_);
        require(recovered.at("scene").at("id") == id, "recovery.id",
                "Recovery ID does not match its document");
        value.data = recovered.at("scene");
        value.revision = recovered.value("revision", 0u);
    }
    for (auto& item : value.data["entities"])
        for (auto& component : item["components"])
            component = schemas_.migrate_component(component);
    documents_.emplace(id, std::move(value));
    return summary(state(id));
}
Json AuthoringService::query(const std::string& id) const {
    std::lock_guard lock(mutex_);
    return summary(state(id));
}
Json AuthoringService::documents() const {
    std::lock_guard lock(mutex_);
    Json result = Json::array();
    for (const auto& [id, value] : documents_)
        result.push_back(summary(value, false));
    return result;
}
void AuthoringService::register_schemas(const Json& manifest) {
    std::lock_guard lock(mutex_);
    schemas_.register_schemas(manifest);
}
void AuthoringService::replace_external_schemas(const Json& manifest) {
    std::lock_guard lock(mutex_);
    auto candidate = builtin_schemas();
    candidate.register_schemas(manifest);
    schemas_ = std::move(candidate);
}
void AuthoringService::apply(Json& scene, const Json& command) {
    require(command.is_object() && command.contains("op") && command["op"].is_string(),
            "command.invalid", "Command requires an operation name");
    const auto op = command.at("op").get<std::string>();
    if (op == "entity.create") {
        Json value = command.contains("entity") && command["entity"].is_object()
                         ? command["entity"]
                         : make_entity(schemas_, command.value("name", std::string("Object")),
                                       command.value("parent", std::string()));
        if (!value.contains("id"))
            value["id"] = new_id();
        scene["entities"].push_back(std::move(value));
    } else if (op == "entity.rename") {
        entity(scene, command.at("entity").get<std::string>())["name"] = command.at("name");
    } else if (op == "scene.simulation") {
        const auto& value = command.at("value");
        require(value.is_object(), "simulation.object", "Simulation settings must be an object");
        const auto defaults = default_simulation_settings();
        for (const auto& [key, setting] : value.items())
            require(defaults.contains(key), "simulation.setting",
                    "Unknown simulation setting: " + key);
        if (!scene.contains("simulation"))
            scene["simulation"] = defaults;
        scene["simulation"].update(value);
    } else if (op == "entity.delete") {
        const auto id = command.at("entity").get<std::string>();
        entity(scene, id);
        std::set<std::string> removed{id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& item : scene["entities"])
                if (removed.contains(parent_id(item)))
                    changed = removed.insert(item.at("id").get<std::string>()).second || changed;
        }
        auto& values = scene["entities"];
        values.erase(std::remove_if(values.begin(), values.end(),
                                    [&](const Json& value) {
                                        return removed.contains(value.at("id").get<std::string>());
                                    }),
                     values.end());
    } else if (op == "entity.reparent") {
        reparent_entity(scene, command.at("entity").get<std::string>(),
                        command.value("parent", Json(nullptr)), command.value("keep_world", false));
    } else if (op == "component.add") {
        auto& value = entity(scene, command.at("entity").get<std::string>());
        const auto type = command.at("type").get<std::string>();
        const auto metadata = schemas_.schema(type);
        Json fields = schemas_.default_fields(type);
        if (command.contains("fields"))
            fields.update(command["fields"]);
        value["components"].push_back({{"id", command.value("id", new_id())},
                                       {"type", type},
                                       {"version", metadata.value("version", 1)},
                                       {"fields", fields}});
    } else if (op == "component.remove") {
        auto& values = entity(scene, command.at("entity").get<std::string>())["components"];
        const auto id = command.at("component").get<std::string>();
        auto found = std::find_if(values.begin(), values.end(),
                                  [&](const Json& value) { return value.at("id") == id; });
        require(found != values.end(), "component.missing", "Component does not exist");
        values.erase(found);
    } else if (op == "component.set") {
        auto& value = component(entity(scene, command.at("entity").get<std::string>()),
                                command.at("component").get<std::string>());
        const auto field = command.at("field").get<std::string>();
        require(!field.empty(), "field.invalid", "FieldId cannot be empty");
        value["fields"][field] = command.at("value");
    } else if (op == "entity.duplicate") {
        const auto id = command.at("entity").get<std::string>();
        entity(scene, id);
        std::set<std::string> subtree{id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& item : scene["entities"])
                if (subtree.contains(parent_id(item)))
                    changed = subtree.insert(item.at("id").get<std::string>()).second || changed;
        }
        std::map<std::string, std::string> mapping;
        for (const auto& item : scene["entities"])
            if (subtree.contains(item.at("id").get<std::string>())) {
                mapping[item.at("id")] = new_id();
                for (const auto& component : item["components"])
                    mapping[component.at("id")] = new_id();
            }
        Json duplicates = Json::array();
        for (const auto& item : scene["entities"])
            if (subtree.contains(item.at("id").get<std::string>())) {
                auto copy = item;
                copy["id"] = mapping.at(item.at("id").get<std::string>());
                const auto parent = parent_id(item);
                if (mapping.contains(parent))
                    copy["parent"] = mapping.at(parent);
                if (item.at("id") == id)
                    copy["name"] = item.at("name").get<std::string>() + " Copy";
                for (auto& component : copy["components"]) {
                    component["id"] = mapping.at(component.at("id").get<std::string>());
                    const auto type = component.at("type").get<std::string>();
                    if (!schemas_.contains(type))
                        continue;
                    const auto metadata = schemas_.schema(type);
                    if (component.value("version", 1) != metadata.value("version", 1))
                        continue;
                    for (auto& [field, value] : component["fields"].items())
                        if (metadata["fields"].contains(field) &&
                            metadata["fields"][field].value("type", std::string()) ==
                                "entity_ref" &&
                            value.is_string() && mapping.contains(value.get<std::string>()))
                            value = mapping.at(value.get<std::string>());
                }
                duplicates.push_back(std::move(copy));
            }
        for (auto& value : duplicates)
            scene["entities"].push_back(std::move(value));
    } else if (op == "scene.rename")
        scene["name"] = command.at("name");
    else if (op == "template.instance") {
        Json value = command.at("instance");
        if (!value.contains("id"))
            value["id"] = new_id();
        if (!scene.contains("instances"))
            scene["instances"] = Json::array();
        scene["instances"].push_back(std::move(value));
    } else if (op == "template.override" || op == "template.revert" || op == "template.suppress" ||
               op == "template.add" || op == "template.reparent" || op == "template.remove" ||
               op == "template.restore" || op == "template.source_set" ||
               op == "template.addition_set") {
        auto& instances = scene["instances"];
        const auto id = command.at("instance").get<std::string>();
        auto found = std::find_if(instances.begin(), instances.end(),
                                  [&](const Json& value) { return value.at("id") == id; });
        require(found != instances.end(), "template.missing", "Instance not found");
        if (op == "template.addition_set") {
            const auto& replacement = command.at("value");
            const auto addition_id = replacement.at("id");
            auto& additions = (*found)["additions"];
            require(additions.is_array(), "template.addition_missing",
                    "Instance has no local additions");
            auto addition =
                std::find_if(additions.begin(), additions.end(),
                             [&](const Json& value) { return value.at("id") == addition_id; });
            require(addition != additions.end(), "template.addition_missing",
                    "Local addition not found");
            *addition = replacement;
            return;
        }
        if (op == "template.remove") {
            instances.erase(found);
            return;
        }
        if (op == "template.source_set") {
            const auto source = command.at("source").get<std::string>();
            project_path(root_, source);
            (*found)["source"] = source;
            return;
        }
        if (op == "template.restore") {
            auto& records = (*found)["suppressed"];
            require(records.is_array(), "template.suppression_missing",
                    "Instance has no suppressed objects");
            const auto address = command.at("address");
            const auto before = records.size();
            records.erase(std::remove(records.begin(), records.end(), address), records.end());
            require(records.size() != before, "template.suppression_missing",
                    "Suppressed object address not found");
            return;
        }
        const std::string key = op == "template.suppress"   ? "suppressed"
                                : op == "template.add"      ? "additions"
                                : op == "template.reparent" ? "reparents"
                                                            : "overrides";
        if (!found->contains(key))
            (*found)[key] = Json::array();
        auto& records = (*found)[key];
        if (key == "overrides") {
            const auto address = command.at("address");
            auto old = std::find_if(records.begin(), records.end(), [&](const Json& value) {
                return value.at("address") == address;
            });
            if (old != records.end())
                records.erase(old);
            if (op != "template.revert")
                records.push_back({{"address", address}, {"value", command.at("value")}});
        } else
            records.push_back(command.at("value"));
    } else
        throw Error("command.unknown", "Unknown authoring command: " + op);
}
Json AuthoringService::transact(const std::string& id, std::uint64_t revision,
                                const Json& operations, const std::string& key) {
    std::lock_guard lock(mutex_);
    auto& current = state(id);
    require(operations.is_array() && !operations.empty(), "transaction.empty",
            "Transaction requires an array of operations");
    const auto fingerprint =
        sha256(Json{{"revision", revision}, {"operations", operations}}.dump());
    if (!key.empty() && current.requests.contains(key)) {
        const auto& request = current.requests.at(key);
        require(request.first == fingerprint, "idempotency.conflict",
                "Idempotency key was used with another payload");
        return request.second;
    }
    check_revision(current.revision, revision);
    State candidate = current;
    for (const auto& operation : operations)
        apply(candidate.data, operation);
    validate_scene(candidate.data, schemas_);
    candidate.undo.push_back(current.data);
    if (candidate.undo.size() > 100)
        candidate.undo.erase(candidate.undo.begin());
    candidate.redo.clear();
    ++candidate.revision;
    journal(candidate);
    auto result = summary(candidate);
    if (!key.empty()) {
        if (candidate.requests.size() >= 256)
            candidate.requests.erase(candidate.requests.begin());
        candidate.requests[key] = {fingerprint, result};
    }
    current = std::move(candidate);
    return result;
}
Json AuthoringService::history(const std::string& id, std::uint64_t revision, bool forward) {
    std::lock_guard lock(mutex_);
    auto& current = state(id);
    check_revision(current.revision, revision);
    State candidate = current;
    auto& source = forward ? candidate.redo : candidate.undo;
    auto& target = forward ? candidate.undo : candidate.redo;
    require(!source.empty(), "history.empty", forward ? "Nothing to redo" : "Nothing to undo");
    target.push_back(candidate.data);
    candidate.data = source.back();
    source.pop_back();
    ++candidate.revision;
    journal(candidate);
    current = std::move(candidate);
    return summary(current);
}
Json AuthoringService::undo(const std::string& id, std::uint64_t revision) {
    return history(id, revision, false);
}
Json AuthoringService::redo(const std::string& id, std::uint64_t revision) {
    return history(id, revision, true);
}
Json AuthoringService::save(const std::string& id, const std::filesystem::path& relative) {
    std::lock_guard lock(mutex_);
    auto& current = state(id);
    const auto selected = relative.empty() ? current.path : relative.lexically_normal();
    require(!selected.empty(), "save.path", "Choose a scene path before saving");
    const auto path = project_path(root_, selected);
    if (std::filesystem::exists(path)) {
        require(selected == current.path && !current.disk_hash.empty(), "save.exists",
                "Save As will not overwrite another file");
        require(sha256_file(path) == current.disk_hash, "save.disk_conflict",
                "File changed outside the Editor; reload or save to another path");
    }
    atomic_write_json(path, current.data);
    current.path = selected;
    current.saved_hash = sha256(current.data.dump());
    current.disk_hash = sha256_file(path);
    journal(current);
    return summary(current);
}
Json AuthoringService::recover(const std::string& id,
                               std::optional<std::uint64_t> expected_revision) {
    std::lock_guard lock(mutex_);
    require(valid_id(Json(id)), "id.invalid", "Invalid recovery document ID");
    const auto record =
        read_json(project_path(root_, std::filesystem::path(".faset/recovery") / (id + ".json")));
    require(record.value("format", std::string()) == "faset.recovery" &&
                record.value("version", 0) == 1,
            "recovery.format", "Unsupported recovery record");
    State candidate;
    candidate.data = record.at("scene");
    validate_scene(candidate.data, schemas_);
    require(candidate.data.at("id") == id, "recovery.id",
            "Recovery ID does not match its document");
    candidate.path = record.at("path").get<std::string>();
    candidate.saved_hash = record.value("saved_hash", std::string());
    candidate.disk_hash = record.value("disk_hash", std::string());
    candidate.revision = record.value("revision", std::uint64_t(0));
    if (!candidate.path.empty()) {
        const auto disk = project_path(root_, candidate.path);
        require(std::filesystem::exists(disk) && sha256_file(disk) == candidate.disk_hash,
                "recovery.disk_conflict",
                "Scene file changed or disappeared since recovery was written");
    }
    for (auto& item : candidate.data["entities"])
        for (auto& component : item["components"])
            component = schemas_.migrate_component(component);
    validate_scene(candidate.data, schemas_);
    if (documents_.contains(id)) {
        const auto& current = state(id);
        require(expected_revision.has_value(), "recovery.revision_required",
                "Recovering an open document requires its current revision");
        check_revision(current.revision, *expected_revision);
        require(current.path == candidate.path, "document.id_collision",
                "Recovery path differs from the open document");
        if (candidate.data == current.data)
            return summary(current);
        candidate.undo = current.undo;
        candidate.undo.push_back(current.data);
        candidate.revision = std::max(current.revision, candidate.revision) + 1;
    } else if (!candidate.path.empty())
        candidate.undo.push_back(read_json(project_path(root_, candidate.path)));
    journal(candidate);
    documents_[id] = std::move(candidate);
    return summary(state(id));
}
Json AuthoringService::recovery_documents() const {
    std::lock_guard lock(mutex_);
    Json result = Json::array();
    const auto path = project_path(root_, ".faset/recovery");
    if (!std::filesystem::exists(path))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(path))
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            try {
                const auto value = read_json(entry.path());
                result.push_back({{"id", value.at("scene").at("id")},
                                  {"name", value.at("scene").at("name")},
                                  {"path", value.at("path")},
                                  {"dirty", sha256(value.at("scene").dump()) !=
                                                value.value("saved_hash", std::string())}});
            } catch (const std::exception&) {
                result.push_back({{"error", "Invalid recovery record"},
                                  {"file", entry.path().filename().string()}});
            }
        }
    return result;
}
} // namespace faset::authoring
