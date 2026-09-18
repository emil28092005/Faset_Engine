#include <array>
#include <cmath>
#include <faset/authoring/schema.hpp>
#include <limits>
#include <set>

namespace faset::authoring {
void validate_field(const Json& value, const Json& descriptor) {
    const auto kind = descriptor.value("type", std::string("any"));
    bool valid = true;
    if (kind == "number" || kind == "float")
        valid = value.is_number() && std::isfinite(value.get<double>());
    else if (kind == "integer" || kind == "int")
        valid = value.is_number_integer();
    else if (kind == "boolean" || kind == "bool")
        valid = value.is_boolean();
    else if (kind == "string" || kind == "asset_ref" || kind == "entity_ref")
        valid = value.is_string();
    else if (kind == "vec2" || kind == "vec3" || kind == "vec4" || kind == "color") {
        const auto size = kind == "vec2" ? 2u : (kind == "vec3" ? 3u : 4u);
        valid = value.is_array() && value.size() == size;
        if (valid)
            for (const auto& entry : value)
                valid = valid && entry.is_number() && std::isfinite(entry.get<double>());
    } else if (kind == "array")
        valid = value.is_array();
    else if (kind == "object")
        valid = value.is_object();
    else
        require(kind == "any", "schema.field_type", "Unsupported schema field type: " + kind);
    require(valid, "validation.field_type",
            "Invalid value for field " + descriptor.value("id", std::string("?")) + " (expected " +
                kind + ")");
    if (value.is_number()) {
        if (descriptor.contains("min"))
            require(value.get<double>() >= descriptor["min"].get<double>(), "validation.minimum",
                    "Field is below its minimum");
        if (descriptor.contains("max"))
            require(value.get<double>() <= descriptor["max"].get<double>(), "validation.maximum",
                    "Field exceeds its maximum");
    }
    if (descriptor.contains("enum")) {
        bool found = false;
        for (const auto& option : descriptor["enum"])
            found = found || option == value;
        require(found, "validation.enum", "Field value is not an allowed choice");
    }
}
void SchemaRegistry::register_schema(const Json& value) {
    require(value.is_object() && value.contains("id") && value["id"].is_string() &&
                value.contains("fields") && value["fields"].is_object(),
            "schema.invalid", "Invalid component schema");
    Json normalized = value;
    const auto id = value.at("id").get<std::string>();
    require(!id.empty(), "schema.invalid", "TypeId cannot be empty");
    if (value.contains("version")) {
        const auto& version = value.at("version");
        require(version.is_number_integer() && version > 0 &&
                    version <= std::numeric_limits<int>::max(),
                "schema.invalid", "Schema version must be a positive supported integer");
    }
    for (auto& [key, field] : normalized["fields"].items()) {
        require(field.is_object() && field.contains("default"), "schema.invalid",
                "Each field requires a typed default");
        require(field.value("id", key) == key, "schema.field_id",
                "Field map keys must be stable FieldIds");
        field["id"] = key;
        for (const auto* limit : {"min", "max"})
            if (field.contains(limit))
                require(field.at(limit).is_number() && std::isfinite(field.at(limit).get<double>()),
                        "schema.invalid", "Field limits must be finite numbers");
        if (field.contains("enum"))
            require(field.at("enum").is_array(), "schema.invalid",
                    "Field enum choices must be an array");
        validate_field(field["default"], field);
    }
    if (auto found = schemas_.find(id); found != schemas_.end())
        require(found->second == normalized, "schema.duplicate_type",
                "A different schema is already registered for " + id);
    schemas_[id] = std::move(normalized);
}
void SchemaRegistry::register_schemas(const Json& values) {
    const auto& array = values.is_array() ? values : values.at("types");
    auto candidate = *this;
    for (const auto& schema : array)
        candidate.register_schema(schema);
    *this = std::move(candidate);
}
SchemaRegistry gameplay_schemas(const Json& manifest) {
    require(manifest.is_array() || (manifest.is_object() && manifest.contains("types") &&
                                    manifest.at("types").is_array()),
            "schema.invalid", "Gameplay schemas require an array or a types manifest");
    const auto& types = manifest.is_array() ? manifest : manifest.at("types");
    auto result = builtin_schemas();
    for (const auto& schema : types) {
        require(schema.is_object() && schema.contains("id") && schema.at("id").is_string(),
                "schema.invalid", "Gameplay schema requires a TypeId");
        const auto id = schema.at("id").get<std::string>();
        require(!result.contains(id), "schema.duplicate_type",
                "Gameplay schema duplicates a builtin or gameplay TypeId: " + id);
        result.register_schema(schema);
    }
    return result;
}
bool SchemaRegistry::contains(const std::string& type) const {
    return schemas_.contains(type);
}
Json SchemaRegistry::schema(const std::string& type) const {
    const auto found = schemas_.find(type);
    require(found != schemas_.end(), "schema.missing", "Component schema unavailable: " + type);
    return found->second;
}
Json SchemaRegistry::manifest() const {
    Json types = Json::array();
    for (const auto& [id, type] : schemas_)
        types.push_back(type);
    return {{"format", "faset.schema"}, {"version", 1}, {"types", types}};
}
Json SchemaRegistry::default_fields(const std::string& type) const {
    Json fields = Json::object();
    const auto metadata = schema(type);
    for (const auto& [id, field] : metadata["fields"].items())
        fields[id] = field["default"];
    return fields;
}
void SchemaRegistry::validate_component(const Json& component) const {
    require(component.is_object() && component.contains("type") && component["type"].is_string() &&
                component.contains("fields") && component["fields"].is_object(),
            "component.invalid", "Invalid component record");
    const auto type = component.at("type").get<std::string>();
    if (!contains(type))
        return;
    const auto metadata = schema(type);
    // Future or missing-module schemas are preserved, not interpreted with the wrong version.
    if (component.value("version", 1) != metadata.value("version", 1))
        return;
    for (const auto& [id, value] : component["fields"].items())
        if (metadata["fields"].contains(id))
            validate_field(value, metadata["fields"][id]);
}
void SchemaRegistry::add_migration(const std::string& type, int from_version, Json rules) {
    require(from_version > 0 && rules.is_object(), "migration.invalid", "Invalid migration");
    require(!migrations_.contains({type, from_version}), "migration.duplicate",
            "Migration already exists");
    migrations_[{type, from_version}] = std::move(rules);
}
Json SchemaRegistry::migrate_component(const Json& source) const {
    Json result = source;
    const auto type = result.at("type").get<std::string>();
    if (!contains(type))
        return result;
    const auto current = schema(type).value("version", 1);
    auto version = result.value("version", 1);
    if (version > current)
        return result;
    while (version < current) {
        const auto found = migrations_.find({type, version});
        require(found != migrations_.end(), "migration.required",
                "Explicit migration required for " + type);
        for (const auto& [field, rule] : found->second.items()) {
            if (rule.contains("default") && !result["fields"].contains(field))
                result["fields"][field] = rule["default"];
            if (rule.contains("scale") && result["fields"].contains(field)) {
                require(result["fields"][field].is_number(), "migration.type",
                        "Cannot scale a nonnumeric field");
                result["fields"][field] =
                    result["fields"][field].get<double>() * rule["scale"].get<double>();
            }
            if (rule.value("require_manual", false) && result["fields"].contains(field))
                throw Error("migration.manual", "Field requires explicit manual migration",
                            {{"type", type}, {"field", field}});
        }
        result["version"] = ++version;
    }
    const auto metadata = schema(type);
    for (const auto& [field, descriptor] : metadata["fields"].items())
        if (!result["fields"].contains(field))
            result["fields"][field] = descriptor["default"];
    validate_component(result);
    return result;
}
SchemaRegistry builtin_schemas() {
    SchemaRegistry registry;
    struct Transform {
        std::array<float, 3> position, rotation, scale;
    };
    TypeRegistration<Transform>(registry, "faset.transform", "Transform")
        .field("position", "Position", &Transform::position, std::array<float, 3>{0, 0, 0}, "vec3")
        .field("rotation", "Rotation", &Transform::rotation, std::array<float, 3>{0, 0, 0}, "vec3",
               {{"unit", "radians"}})
        .field("scale", "Scale", &Transform::scale, std::array<float, 3>{1, 1, 1}, "vec3")
        .commit();
    auto add = [&](std::string id, std::string name, Json fields) {
        registry.register_schema({{"id", id}, {"name", name}, {"version", 1}, {"fields", fields}});
    };
    auto field = [](std::string type, Json value) {
        return Json{{"type", type}, {"default", value}};
    };
    add("faset.sprite", "Sprite",
        {{"color", field("color", {0.65, 0.6, 0.85, 1.0})},
         {"size", field("vec2", {1, 1})},
         {"texture", field("asset_ref", "")},
         {"layer", field("integer", 0)}});
    add("faset.mesh", "Mesh",
        {{"asset", field("asset_ref", "")},
         {"color", field("color", {0.65, 0.65, 0.68, 1.0})},
         {"primitive",
          Json{{"type", "string"}, {"default", "cube"}, {"enum", {"cube", "plane", "asset"}}}}});
    add("faset.camera", "Camera",
        {{"fov", Json{{"type", "number"}, {"default", 60.0}, {"min", 1.0}, {"max", 179.0}}},
         {"near", Json{{"type", "number"}, {"default", 0.1}, {"min", 0.001}}},
         {"far", Json{{"type", "number"}, {"default", 1000.0}, {"min", 0.01}}}});
    add("faset.light", "Directional Light",
        {{"color", field("color", {1, 1, 1, 1})},
         {"intensity", Json{{"type", "number"}, {"default", 1.0}, {"min", 0.0}}}});
    for (int dimension : {2, 3}) {
        Json vector = dimension == 2 ? Json{0, 0} : Json{0, 0, 0};
        Json extents = dimension == 2 ? Json{0.5, 0.5} : Json{0.5, 0.5, 0.5};
        add("faset.rigid_body_" + std::to_string(dimension) + "d",
            "Rigid Body " + std::to_string(dimension) + "D",
            {{"body_type", Json{{"type", "string"},
                                {"default", "dynamic"},
                                {"enum", {"static", "dynamic", "kinematic"}}}},
             {"half_extents", field(dimension == 2 ? "vec2" : "vec3", extents)},
             {"linear_velocity", field(dimension == 2 ? "vec2" : "vec3", vector)},
             {"density", Json{{"type", "number"}, {"default", 1.0}, {"min", 0.001}}},
             {"friction", Json{{"type", "number"}, {"default", 0.5}, {"min", 0.0}}},
             {"restitution",
              Json{{"type", "number"}, {"default", 0.0}, {"min", 0.0}, {"max", 1.0}}},
             {"gravity_scale", field("number", 1.0)},
             {"category_bits", Json{{"type", "integer"}, {"default", 1}, {"min", 0}}},
             {"mask_bits", Json{{"type", "integer"}, {"default", 65535}, {"min", 0}}}});
    }
    return registry;
}
} // namespace faset::authoring
