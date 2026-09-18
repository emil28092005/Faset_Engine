#include <faset/authoring/templates.hpp>
#include <faset/authoring/service.hpp>
#include <faset/core/hash.hpp>
#include <algorithm>
#include <set>

namespace faset::authoring {
namespace {
std::string scoped_id(const std::string& root,const Json& path,const std::string& source) {
    const auto digest=sha256(Json::array({root,path,source}).dump());
    return digest.substr(0,8)+"-"+digest.substr(8,4)+"-5"+digest.substr(13,3)+"-a"+digest.substr(17,3)+"-"+digest.substr(20,12);
}
struct Resolver {
    const SchemaRegistry& schemas;
    const SceneLoader& loader;
    std::string root;
    Json conflicts=Json::array();
    std::set<std::string> sources;
    void conflict(const Json& path,std::string code,const Json& record) {conflicts.push_back({{"instance_path",path},{"code",std::move(code)},{"record",record}});}
    Json* target(Json& entities,const Json& path,const Json& address) {
        Json full=path;for(const auto& entry:address.value("path",Json::array()))full.push_back(entry);
        for(auto& item:entities)if(item.at("origin").at("path")==full&&item.at("origin").at("object")==address.at("object"))return &item;
        return nullptr;
    }
    Json expand(const Json& scene,const Json& path) {
        require(path.size()<=32,"template.depth","Maximum template nesting depth exceeded");
        validate_scene(scene,schemas);
        Json output=Json::array();std::map<std::string,std::string> ids;
        for(const auto& item:scene["entities"]) {
            const auto id=item.at("id").get<std::string>();ids[id]=path.empty()?id:scoped_id(root,path,id);
            for(const auto& component:item["components"]) {const auto cid=component.at("id").get<std::string>();ids[cid]=path.empty()?cid:scoped_id(root,path,cid);}
        }
        for(const auto& source:scene["entities"]) {
            Json item=source;item["id"]=ids.at(source.at("id").get<std::string>());
            item["origin"]={{"path",path},{"object",source.at("id")},{"scene",scene.at("id")}};
            if(source.contains("parent")&&!source["parent"].is_null())item["parent"]=ids.at(source["parent"].get<std::string>());
            for(auto& component:item["components"]) {
                const auto source_id=component.at("id").get<std::string>();component["id"]=ids.at(source_id);component["source_id"]=source_id;
                const auto type=component.at("type").get<std::string>();if(!schemas.contains(type))continue;
                const auto metadata=schemas.schema(type);
                for(auto& [field,value]:component["fields"].items())if(metadata["fields"].contains(field)&&metadata["fields"][field].value("type",std::string())=="entity_ref"&&value.is_string()&&ids.contains(value.get<std::string>()))value=ids.at(value.get<std::string>());
            }
            output.push_back(std::move(item));
        }
        for(const auto& instance:scene.value("instances",Json::array())) {
            Json nested_path=path;nested_path.push_back(instance.at("id"));const auto source_name=instance.at("source").get<std::string>();
            Json expanded=Json::array();std::string source_id;
            try {
                const auto source=loader(source_name);source_id=source.at("id").get<std::string>();
                require(sources.insert(source_id).second,"template.cycle","Template source cycle detected");
                expanded=expand(source,nested_path);sources.erase(source_id);
            } catch(const std::exception& error) {
                if(!source_id.empty())sources.erase(source_id);
                conflict(nested_path,"template.source_unavailable",{{"source",source_name},{"message",error.what()}});continue;
            }
            for(const auto& addition:instance.value("additions",Json::array())) {
                Json item=addition;const auto id=item.at("id").get<std::string>();item["id"]=scoped_id(root,nested_path,id);
                item["origin"]={{"path",nested_path},{"object",id},{"local",true}};
                if(item.contains("parent")&&!item["parent"].is_null())item["parent"]=scoped_id(root,nested_path,item["parent"].get<std::string>());
                for(auto& component:item["components"]) {const auto cid=component.at("id").get<std::string>();component["source_id"]=cid;component["id"]=scoped_id(root,nested_path,cid);}
                expanded.push_back(std::move(item));
            }
            for(const auto& change:instance.value("overrides",Json::array())) {
                const auto& address=change.at("address");auto* item=target(expanded,nested_path,address);
                if(!item){conflict(nested_path,"override.object_missing",change);continue;}
                auto found=std::find_if((*item)["components"].begin(),(*item)["components"].end(),[&](const Json& value){return value.at("source_id")==address.at("component");});
                if(found==(*item)["components"].end()){conflict(nested_path,"override.component_missing",change);continue;}
                const auto type=found->at("type").get<std::string>();const auto field=address.at("field").get<std::string>();
                if(!schemas.contains(type)||!schemas.schema(type)["fields"].contains(field)){conflict(nested_path,"override.field_unavailable",change);continue;}
                try {validate_field(change.at("value"),schemas.schema(type)["fields"][field]);(*found)["fields"][field]=change.at("value");}
                catch(const std::exception& error){conflict(nested_path,"override.invalid",{{"change",change},{"message",error.what()}});}
            }
            std::set<std::string> suppressed;
            for(const auto& address:instance.value("suppressed",Json::array())) {
                auto* item=target(expanded,nested_path,address);if(item)suppressed.insert(item->at("id").get<std::string>());else conflict(nested_path,"suppression.object_missing",address);
            }
            bool changed=true;
            while(changed) {changed=false;for(const auto& item:expanded)if(item.contains("parent")&&item["parent"].is_string()&&suppressed.contains(item["parent"].get<std::string>()))changed=suppressed.insert(item.at("id").get<std::string>()).second||changed;}
            expanded.erase(std::remove_if(expanded.begin(),expanded.end(),[&](const Json& item){return suppressed.contains(item.at("id").get<std::string>());}),expanded.end());
            for(const auto& reparent:instance.value("reparents",Json::array())) {
                auto* item=target(expanded,nested_path,reparent.at("object"));
                auto* parent=reparent.at("parent").is_null()?nullptr:target(expanded,nested_path,reparent.at("parent"));
                if(!item||(!reparent.at("parent").is_null()&&!parent)){conflict(nested_path,"reparent.target_missing",reparent);continue;}
                if(reparent.value("keep_world",false)){conflict(nested_path,"reparent.world_transform_required",reparent);continue;}
                (*item)["parent"]=parent?parent->at("id"):Json(nullptr);
            }
            for(auto& item:expanded)output.push_back(std::move(item));
            require(output.size()<=100000,"template.size","Resolved scene exceeds object limit");
        }
        return output;
    }
};
}
ResolvedScene resolve_templates(const Json& scene,const SchemaRegistry& schemas,const SceneLoader& loader) {
    Resolver resolver{schemas,loader,scene.at("id").get<std::string>()};resolver.sources.insert(scene.at("id").get<std::string>());
    Json output=scene;output["entities"]=resolver.expand(scene,Json::array());output["instances"]=Json::array();
    validate_scene(output,schemas);return {output,resolver.conflicts};
}
}
