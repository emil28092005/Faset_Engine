#pragma once
#include <faset/authoring/schema.hpp>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace faset::authoring {
Json make_scene(std::string name, int dimension = 3);
Json default_simulation_settings();
Json make_entity(const SchemaRegistry& schemas, std::string name, const std::string& parent = "");
void validate_scene(const Json& scene, const SchemaRegistry& schemas);

class AuthoringService {
  public:
    explicit AuthoringService(std::filesystem::path project_root,
                              SchemaRegistry schemas = builtin_schemas());
    Json create(std::string name, int dimension = 3);
    Json open(const std::filesystem::path& relative, bool recover = false);
    Json query(const std::string& document) const;
    Json documents() const;
    Json transact(const std::string& document, std::uint64_t expected_revision,
                  const Json& operations, const std::string& idempotency_key = "");
    Json undo(const std::string& document, std::uint64_t expected_revision);
    Json redo(const std::string& document, std::uint64_t expected_revision);
    Json save(const std::string& document, const std::filesystem::path& relative = {});
    Json recovery_documents() const;
    Json recover(const std::string& document,
                 std::optional<std::uint64_t> expected_revision = std::nullopt);
    const SchemaRegistry& schemas() const {
        return schemas_;
    }
    void register_schemas(const Json& manifest);
    void replace_external_schemas(const Json& manifest);
    const std::filesystem::path& root() const {
        return root_;
    }

  private:
    struct State {
        Json data;
        std::uint64_t revision = 0;
        std::filesystem::path path;
        std::string saved_hash, disk_hash;
        std::vector<Json> undo, redo;
        std::map<std::string, std::pair<std::string, Json>> requests;
    };
    Json summary(const State& state, bool include_data = true) const;
    State& state(const std::string& document);
    const State& state(const std::string& document) const;
    void journal(const State& state) const;
    void apply(Json& scene, const Json& operation);
    Json history(const std::string& document, std::uint64_t revision, bool redo);
    std::filesystem::path root_;
    SchemaRegistry schemas_;
    std::map<std::string, State> documents_;
    mutable std::recursive_mutex mutex_;
};
} // namespace faset::authoring
