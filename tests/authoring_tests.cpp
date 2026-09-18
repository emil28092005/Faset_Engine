#include <faset/authoring/service.hpp>
#include <faset/authoring/templates.hpp>
#include <faset/core/io.hpp>
#include <iostream>

#define CHECK(x) do {if(!(x))throw std::runtime_error("Check failed at line "+std::to_string(__LINE__)+": " #x);}while(false)
template<class Fn> void fails(Fn fn,const std::string& code) {try{fn();}catch(const faset::Error& error){CHECK(error.code()==code);return;}throw std::runtime_error("Expected error "+code);}
int main() {
    using namespace faset;using namespace faset::authoring;
    const auto root=std::filesystem::temp_directory_path()/("faset-authoring-"+new_id());
    try {
        auto schemas=builtin_schemas();AuthoringService service(root,schemas);
        auto created=service.create("Courtyard",3);const std::string id=created["id"];
        auto first=make_entity(schemas,"Door");const std::string entity_id=first["id"],transform_id=first["components"][0]["id"];
        Json commands=Json::array({{{"op","entity.create"},{"entity",first}}});
        const auto after=service.transact(id,0,commands,"request-1");CHECK(after["revision"]==1);CHECK(after["scene"]["entities"].size()==1);
        CHECK(service.transact(id,0,commands,"request-1")==after);
        fails([&]{service.transact(id,0,commands);},"revision.conflict");
        fails([&]{service.transact(id,1,commands,"request-1");},"idempotency.conflict");
        auto bad=Json::array({{{"op","entity.rename"},{"entity",entity_id},{"name","Should not survive"}},{{"op","component.set"},{"entity",entity_id},{"component",transform_id},{"field","position"},{"value","not a vector"}}});
        fails([&]{service.transact(id,1,bad);},"validation.field_type");CHECK(service.query(id)==after);
        auto edited=service.transact(id,1,Json::array({{{"op","component.set"},{"entity",entity_id},{"component",transform_id},{"field","position"},{"value",{4,0,2}}}}));
        CHECK(edited["revision"]==2);CHECK(service.undo(id,2)["scene"]==after["scene"]);CHECK(service.redo(id,3)["scene"]==edited["scene"]);
        CHECK(service.save(id,"Scenes/courtyard.scene.json")["dirty"]==false);
        service.transact(id,4,Json::array({{{"op","entity.rename"},{"entity",entity_id},{"name","Дверь 世界"}}}));
        AuthoringService restarted(root,schemas);const auto recovered=restarted.open("Scenes/courtyard.scene.json",true);CHECK(recovered["scene"]["entities"][0]["name"]=="Дверь 世界");CHECK(recovered["dirty"]==true);
        atomic_write(root/"Scenes/courtyard.scene.json",read_text(root/"Scenes/courtyard.scene.json")+"\n");
        fails([&]{restarted.save(id);},"save.disk_conflict");
        // Parent cycles are rejected atomically; names never provide identity.
        fails([&]{service.transact(id,5,Json::array({{{"op","entity.reparent"},{"entity",entity_id},{"parent",entity_id}}}));},"entity.cycle");
        CHECK(service.query(id)["revision"]==5);
        // Unavailable extension data is retained, including fields unknown to this SDK.
        auto unknown=make_entity(schemas,"Plugin object");unknown["components"].push_back({{"id",new_id()},{"type","plugin.future"},{"version",5},{"fields",{{"unknown",Json::array({1,2,3})}}}});
        service.transact(id,5,Json::array({{{"op","entity.create"},{"entity",unknown}}}));CHECK(service.query(id)["scene"]["entities"][1]==unknown);
        // Nested template addresses remain valid after source rename and source reparent.
        auto source=make_scene("Door template");source["entities"].push_back(first);
        auto middle=make_scene("Nested");middle["instances"].push_back({{"id","nested"},{"source","door"}});
        auto outer=make_scene("Level");
        Json address={{"path",Json::array({"nested"})},{"object",entity_id},{"component",transform_id},{"field","position"}};
        outer["instances"].push_back({{"id","one"},{"source","middle"},{"overrides",Json::array({{{"address",address},{"value",{8,0,0}}}})}});
        outer["instances"].push_back({{"id","two"},{"source","middle"}});
        auto loader=[&](const std::string& name){return name=="door"?source:middle;};
        auto resolved=resolve_templates(outer,schemas,loader);CHECK(resolved.conflicts.empty());CHECK(resolved.scene["entities"].size()==2);
        CHECK(resolved.scene["entities"][0]["components"][0]["fields"]["position"]==Json::array({8,0,0}));
        CHECK(resolved.scene["entities"][1]["components"][0]["fields"]["position"]==Json::array({0,0,0}));
        const auto stable=resolved.scene["entities"][0]["id"];source["entities"][0]["name"]="Renamed";
        CHECK(resolve_templates(outer,schemas,loader).scene["entities"][0]["id"]==stable);
        source["entities"]=Json::array();CHECK(resolve_templates(outer,schemas,loader).conflicts.size()==1);CHECK(outer["instances"][0]["overrides"].size()==1);
        // Stable FieldId survives a label rename; incompatible migrations require an explicit decision.
        SchemaRegistry newer;newer.register_schema({{"id","sample.type"},{"name","Sample"},{"version",2},{"fields",{{"speed",{{"id","speed"},{"name","Movement speed"},{"type","number"},{"default",2}}},{"enabled",{{"type","boolean"},{"default",true}}}}}});
        newer.add_migration("sample.type",1,{{"speed",{{"scale",0.01}}}});
        const auto migrated=newer.migrate_component({{"id",new_id()},{"type","sample.type"},{"version",1},{"fields",{{"speed",300},{"unrecognized","preserve"}}}});
        CHECK(migrated["fields"]["speed"]==3.0);CHECK(migrated["fields"]["enabled"]==true);CHECK(migrated["fields"]["unrecognized"]=="preserve");
        std::filesystem::remove_all(root);std::cout<<"Authoring transactions, conflict/retry, Undo, recovery, unknown fields, nested IDs and migrations passed\n";return 0;
    } catch(const std::exception& error) {std::filesystem::remove_all(root);std::cerr<<error.what()<<'\n';return 1;}
}
