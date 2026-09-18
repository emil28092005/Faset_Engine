#include <faset/authoring/service.hpp>
#include <faset/authoring/templates.hpp>
#include <faset/authoring/transforms.hpp>
#include <faset/core/io.hpp>
#include <iostream>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Check failed at line " + std::to_string(__LINE__) +          \
                                     ": " #x);                                                     \
    } while (false)
template <class Fn> void fails(Fn fn, const std::string& code) {
    try {
        fn();
    } catch (const faset::Error& error) {
        CHECK(error.code() == code);
        return;
    }
    throw std::runtime_error("Expected error " + code);
}
int main() {
    using namespace faset;
    using namespace faset::authoring;
    const auto root = std::filesystem::temp_directory_path() / ("faset-authoring-" + new_id());
    try {
        auto schemas = builtin_schemas();
        AuthoringService service(root, schemas);
        auto created = service.create("Courtyard", 3);
        const std::string id = created["id"];
        auto first = make_entity(schemas, "Door");
        const std::string entity_id = first["id"], transform_id = first["components"][0]["id"];
        Json commands = Json::array({{{"op", "entity.create"}, {"entity", first}}});
        const auto after = service.transact(id, 0, commands, "request-1");
        CHECK(after["revision"] == 1);
        CHECK(after["scene"]["entities"].size() == 1);
        CHECK(service.transact(id, 0, commands, "request-1") == after);
        fails([&] { service.transact(id, 0, commands); }, "revision.conflict");
        fails([&] { service.transact(id, 1, commands, "request-1"); }, "idempotency.conflict");
        auto bad = Json::array(
            {{{"op", "entity.rename"}, {"entity", entity_id}, {"name", "Should not survive"}},
             {{"op", "component.set"},
              {"entity", entity_id},
              {"component", transform_id},
              {"field", "position"},
              {"value", "not a vector"}}});
        fails([&] { service.transact(id, 1, bad); }, "validation.field_type");
        CHECK(service.query(id) == after);
        auto edited = service.transact(id, 1,
                                       Json::array({{{"op", "component.set"},
                                                     {"entity", entity_id},
                                                     {"component", transform_id},
                                                     {"field", "position"},
                                                     {"value", {4, 0, 2}}}}));
        CHECK(edited["revision"] == 2);
        CHECK(service.undo(id, 2)["scene"] == after["scene"]);
        CHECK(service.redo(id, 3)["scene"] == edited["scene"]);
        CHECK(service.save(id, "Scenes/courtyard.scene.json")["dirty"] == false);
        service.transact(
            id, 4,
            Json::array(
                {{{"op", "entity.rename"}, {"entity", entity_id}, {"name", "Дверь 世界"}}}));
        AuthoringService restarted(root, schemas);
        const auto recovered = restarted.open("Scenes/courtyard.scene.json", true);
        CHECK(recovered["scene"]["entities"][0]["name"] == "Дверь 世界");
        CHECK(recovered["dirty"] == true);
        AuthoringService opened_then_recovered(root, schemas);
        opened_then_recovered.open("Scenes/courtyard.scene.json");
        fails([&] { opened_then_recovered.recover(id); }, "recovery.revision_required");
        CHECK(opened_then_recovered.recover(id, 0)["scene"]["entities"][0]["name"] == "Дверь 世界");
        auto fresh = service.create("Never saved", 2);
        const std::string fresh_id = fresh.at("id");
        service.transact(fresh_id, 0,
                         Json::array({{{"op", "entity.create"}, {"name", "Recovered sprite"}}}));
        AuthoringService recover_new(root, schemas);
        const auto restored_new = recover_new.recover(fresh_id);
        CHECK(restored_new["dirty"] == true && restored_new["path"] == "");
        CHECK(restored_new["scene"]["entities"].size() == 1);
        fails([&] { recover_new.recover("../../outside"); }, "id.invalid");
        atomic_write(root / "Scenes/courtyard.scene.json",
                     read_text(root / "Scenes/courtyard.scene.json") + "\n");
        fails([&] { restarted.save(id); }, "save.disk_conflict");
        const auto configured = service.create("Configured scene");
        const std::string configured_id = configured.at("id");
        const auto setting = service.transact(
            configured_id, 0,
            Json::array({{{"op", "scene.simulation"},
                          {"value", {{"fixed_delta", 0.02}, {"gravity", {0, -10, 0}}}}}}));
        CHECK(setting["scene"]["simulation"]["fixed_delta"] == 0.02);
        fails(
            [&] {
                service.transact(configured_id, 1,
                                 Json::array({{{"op", "scene.simulation"},
                                               {"value", {{"max_catch_up_ticks", 1.5}}}}}));
            },
            "simulation.integer");
        fails(
            [&] {
                service.transact(
                    configured_id, 1,
                    Json::array({{{"op", "scene.simulation"}, {"value", {{"fixed_delta", 0}}}}}));
            },
            "simulation.fixed_delta");
        CHECK(!service.undo(configured_id, 1)["scene"].contains("simulation"));
        // Parent cycles are rejected atomically; names never provide identity.
        fails(
            [&] {
                service.transact(id, 5,
                                 Json::array({{{"op", "entity.reparent"},
                                               {"entity", entity_id},
                                               {"parent", entity_id}}}));
            },
            "entity.cycle");
        CHECK(service.query(id)["revision"] == 5);
        // A failed recovery publication rejects the entire mutation and its Undo record.
        const auto fault = service.create("Journal failure");
        const std::string fault_id = fault["id"];
        const auto journal = root / ".faset/recovery" / (fault_id + ".json");
        const auto saved_journal = read_text(journal);
        std::filesystem::remove(journal);
        std::filesystem::create_directory(journal);
        bool journal_failed = false;
        try {
            service.transact(
                fault_id, 0,
                Json::array({{{"op", "entity.create"}, {"name", "Must not publish"}}}));
        } catch (const std::exception&) {
            journal_failed = true;
        }
        CHECK(journal_failed);
        CHECK(service.query(fault_id) == fault);
        std::filesystem::remove(journal);
        atomic_write(journal, saved_journal);
        // Unavailable extension data is retained, including fields unknown to this SDK.
        auto unknown = make_entity(schemas, "Plugin object");
        unknown["components"].push_back({{"id", new_id()},
                                         {"type", "plugin.future"},
                                         {"version", 5},
                                         {"fields", {{"unknown", Json::array({1, 2, 3})}}}});
        service.transact(id, 5, Json::array({{{"op", "entity.create"}, {"entity", unknown}}}));
        CHECK(service.query(id)["scene"]["entities"][1] == unknown);
        // Nested template addresses remain valid after source rename and source reparent.
        auto source = make_scene("Door template");
        source["entities"].push_back(first);
        auto middle = make_scene("Nested");
        middle["instances"].push_back({{"id", "nested"}, {"source", "door"}});
        auto outer = make_scene("Level");
        Json address = {{"path", Json::array({"nested"})},
                        {"object", entity_id},
                        {"component", transform_id},
                        {"field", "position"}};
        outer["instances"].push_back(
            {{"id", "one"},
             {"source", "middle"},
             {"overrides", Json::array({{{"address", address}, {"value", {8, 0, 0}}}})}});
        outer["instances"].push_back({{"id", "two"}, {"source", "middle"}});
        auto loader = [&](const std::string& name) { return name == "door" ? source : middle; };
        auto resolved = resolve_templates(outer, schemas, loader);
        CHECK(resolved.conflicts.empty());
        CHECK(resolved.scene["entities"].size() == 2);
        CHECK(resolved.scene["entities"][0]["components"][0]["fields"]["position"] ==
              Json::array({8, 0, 0}));
        CHECK(resolved.scene["entities"][1]["components"][0]["fields"]["position"] ==
              Json::array({0, 0, 0}));
        const auto stable = resolved.scene["entities"][0]["id"];
        source["entities"][0]["name"] = "Renamed";
        CHECK(resolve_templates(outer, schemas, loader).scene["entities"][0]["id"] == stable);
        source["entities"] = Json::array();
        CHECK(resolve_templates(outer, schemas, loader).conflicts.size() == 1);
        CHECK(outer["instances"][0]["overrides"].size() == 1);
        // Template deletion, suppression restore and source repair are normal Undo transactions.
        auto template_doc = service.create("Template management");
        const std::string template_id = template_doc["id"];
        service.transact(
            template_id, 0,
            Json::array({{{"op", "template.instance"},
                          {"instance", {{"id", "inst"}, {"source", "Scenes/absent.json"}}}}}));
        const Json suppressed = {{"path", Json::array()}, {"object", entity_id}};
        service.transact(
            template_id, 1,
            Json::array(
                {{{"op", "template.suppress"}, {"instance", "inst"}, {"value", suppressed}}}));
        auto restored = service.transact(
            template_id, 2,
            Json::array(
                {{{"op", "template.restore"}, {"instance", "inst"}, {"address", suppressed}},
                 {{"op", "template.source_set"},
                  {"instance", "inst"},
                  {"source", "Scenes/courtyard.scene.json"}}}));
        CHECK(restored["scene"]["instances"][0]["suppressed"].empty());
        CHECK(service
                  .transact(template_id, 3,
                            Json::array({{{"op", "template.remove"},
                                          {"instance", "inst"}}}))["scene"]["instances"]
                  .empty());
        CHECK(service.undo(template_id, 4)["scene"]["instances"].size() == 1);
        const auto added = make_entity(schemas, "Local object");
        service.transact(
            template_id, 5,
            Json::array({{{"op", "template.add"}, {"instance", "inst"}, {"value", added}}}));
        auto replacement = added;
        replacement["name"] = "Edited local object";
        replacement["components"].push_back({{"id", new_id()},
                                             {"type", "faset.mesh"},
                                             {"version", 1},
                                             {"fields", schemas.default_fields("faset.mesh")}});
        const auto replaced = service.transact(
            template_id, 6,
            Json::array(
                {{{"op", "template.addition_set"}, {"instance", "inst"}, {"value", replacement}}}));
        CHECK(replaced["scene"]["instances"][0]["additions"][0]["components"].size() == 2);
        auto malformed = replacement;
        malformed["components"][1]["fields"]["primitive"] = "unsupported";
        fails(
            [&] {
                service.transact(template_id, 7,
                                 Json::array({{{"op", "template.addition_set"},
                                               {"instance", "inst"},
                                               {"value", malformed}}}));
            },
            "validation.enum");
        // Malformed address records never reach journals; valid unresolved targets stay
        // recoverable.
        for (const auto& broken : Json::array(
                 {{{"id", "bad"},
                   {"source", "missing"},
                   {"overrides", Json::array({{{"address", "oops"}, {"value", 42}}})}},
                  {{"id", "bad"},
                   {"source", "missing"},
                   {"suppressed", Json::array({{{"object", "ok"}, {"path", 42}}})}},
                  {{"id", "bad"},
                   {"source", "missing"},
                   {"reparents",
                    Json::array({{{"object", {{"object", "ok"}}}, {"parent", false}}})}}})) {
            const auto before = service.query(template_id);
            bool rejected = false;
            try {
                service.transact(
                    template_id, before["revision"],
                    Json::array({{{"op", "template.instance"}, {"instance", broken}}}));
            } catch (const Error&) {
                rejected = true;
            }
            CHECK(rejected);
            CHECK(service.query(template_id) == before);
        }
        // Stable FieldId survives a label rename; incompatible migrations require an explicit
        // decision.
        SchemaRegistry newer;
        newer.register_schema(
            {{"id", "sample.type"},
             {"name", "Sample"},
             {"version", 2},
             {"fields",
              {{"speed",
                {{"id", "speed"}, {"name", "Movement speed"}, {"type", "number"}, {"default", 2}}},
               {"enabled", {{"type", "boolean"}, {"default", true}}}}}});
        newer.add_migration("sample.type", 1, {{"speed", {{"scale", 0.01}}}});
        const auto migrated =
            newer.migrate_component({{"id", new_id()},
                                     {"type", "sample.type"},
                                     {"version", 1},
                                     {"fields", {{"speed", 300}, {"unrecognized", "preserve"}}}});
        CHECK(migrated["fields"]["speed"] == 3.0);
        CHECK(migrated["fields"]["enabled"] == true);
        CHECK(migrated["fields"]["unrecognized"] == "preserve");
        // Local additions and overrides remap references within their own instance; future schemas
        // stay opaque.
        auto refs = builtin_schemas();
        refs.register_schema({{"id", "ref"},
                              {"version", 1},
                              {"fields", {{"target", {{"type", "entity_ref"}, {"default", ""}}}}}});
        auto ref_entity = [&](const std::string& name, const std::string& target, int version = 1) {
            return Json{
                {"id", name},
                {"name", name},
                {"parent", nullptr},
                {"components",
                 Json::array({{{"id", name + "-ref"},
                               {"type", "ref"},
                               {"version", version},
                               {"fields", {{"target", target}, {"opaque", {{"saved", true}}}}}}})}};
        };
        auto ref_source = make_scene("References");
        ref_source["entities"] =
            Json::array({ref_entity("a", "b"), ref_entity("b", "a"), ref_entity("future", "a", 2)});
        auto ref_outer = make_scene("Instances");
        ref_outer["instances"] = Json::array(
            {{{"id", "ref-instance"},
              {"source", "ref-source"},
              {"additions", Json::array({ref_entity("local", "b")})},
              {"overrides",
               Json::array(
                   {{{"address", {{"object", "a"}, {"component", "a-ref"}, {"field", "target"}}},
                     {"value", "local"}}})}}});
        const auto ref_result =
            resolve_templates(ref_outer, refs, [&](const std::string&) { return ref_source; });
        CHECK(ref_result.conflicts.empty());
        const auto& re = ref_result.scene["entities"];
        CHECK(re[0]["components"][0]["fields"]["target"] == re[3]["id"]);
        CHECK(re[3]["components"][0]["fields"]["target"] == re[1]["id"]);
        CHECK(re[2]["components"][0]["fields"] ==
              ref_source["entities"][2]["components"][0]["fields"]);
        AuthoringService opaque(root / "opaque", refs);
        const auto opaque_doc = opaque.create("Future");
        const std::string opaque_id = opaque_doc["id"];
        opaque.transact(
            opaque_id, 0,
            Json::array({{{"op", "entity.create"}, {"entity", ref_entity("a", "a", 2)}}}));
        const auto duplicated = opaque.transact(
            opaque_id, 1, Json::array({{{"op", "entity.duplicate"}, {"entity", "a"}}}));
        CHECK(duplicated["scene"]["entities"][1]["components"][0]["fields"] ==
              duplicated["scene"]["entities"][0]["components"][0]["fields"]);
        // Full TRS reparent, including parent rotation/scale, preserves world placement.
        auto hierarchy = make_scene("Transforms");
        auto parent = make_entity(schemas, "Parent");
        auto child = make_entity(schemas, "Child");
        parent["components"][0]["fields"]["position"] = Json::array({10, 0, 0});
        parent["components"][0]["fields"]["rotation"] = Json::array({0, 0, 1.5707963267948966});
        parent["components"][0]["fields"]["scale"] = Json::array({2, 2, 2});
        child["components"][0]["fields"]["position"] = Json::array({10, 4, 0});
        hierarchy["entities"] = Json::array({parent, child});
        reparent_entity(hierarchy, child["id"], parent["id"], true);
        auto position = hierarchy["entities"][1]["components"][0]["fields"]["position"];
        CHECK(std::abs(position[0].get<double>() - 2.0) < 1e-6);
        CHECK(std::abs(position[1].get<double>()) < 1e-6);
        reparent_entity(hierarchy, child["id"], nullptr, true);
        position = hierarchy["entities"][1]["components"][0]["fields"]["position"];
        CHECK(std::abs(position[0].get<double>() - 10) < 1e-6);
        CHECK(std::abs(position[1].get<double>() - 4) < 1e-6);
        std::filesystem::remove_all(root);
        std::cout << "Authoring transactions, conflict/retry, Undo, recovery, unknown fields, "
                     "nested IDs and migrations passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
