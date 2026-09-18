#pragma once
#include <faset/core/error.hpp>
#include <faset/core/json.hpp>
#include <map>
#include <string>
#include <type_traits>

namespace faset::authoring {
class SchemaRegistry {
  public:
    void register_schema(const Json& schema);
    void register_schemas(const Json& schemas);
    bool contains(const std::string& type) const;
    Json schema(const std::string& type) const;
    Json manifest() const;
    Json default_fields(const std::string& type) const;
    // Unknown fields and absent schemas survive authoring. Known fields are validated.
    void validate_component(const Json& component) const;
    Json migrate_component(const Json& component) const;
    void add_migration(const std::string& type, int from_version, Json field_rules);

  private:
    std::map<std::string, Json> schemas_;
    std::map<std::pair<std::string, int>, Json> migrations_;
};

template <class T> class TypeRegistration {
  public:
    TypeRegistration(SchemaRegistry& registry, std::string id, std::string name, int version = 1)
        : registry_(registry), schema_{{"id", std::move(id)},
                                       {"name", std::move(name)},
                                       {"version", version},
                                       {"fields", Json::object()}} {}
    template <class Value>
    TypeRegistration& field(std::string id, std::string name, Value T::* member,
                            Value default_value, std::string kind,
                            Json constraints = Json::object()) {
        static_assert(std::is_member_object_pointer_v<decltype(member)>);
        require(!schema_["fields"].contains(id), "schema.duplicate_field",
                "Duplicate stable FieldId");
        // Converting the typed default checks supported JSON serialization at compile time.
        Json descriptor = {{"id", id},
                           {"name", std::move(name)},
                           {"type", std::move(kind)},
                           {"default", Json(default_value)}};
        descriptor.update(constraints);
        schema_["fields"][id] = std::move(descriptor);
        return *this;
    }
    void commit() {
        registry_.register_schema(schema_);
    }

  private:
    SchemaRegistry& registry_;
    Json schema_;
};
SchemaRegistry builtin_schemas();
void validate_field(const Json& value, const Json& descriptor);
} // namespace faset::authoring
