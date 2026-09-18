#pragma once
#include <faset/authoring/service.hpp>
#include <functional>
#include <map>
#include <string>

namespace faset::editor {
class Commands {
  public:
    using Handler = std::function<Json(const Json&)>;
    explicit Commands(authoring::AuthoringService& authoring);
    void add(std::string name, std::string description, Json input_schema, Handler handler,
             bool read_only = false);
    Json list() const;
    void remove(const std::string& name);
    Json call(const std::string& name, const Json& arguments);
    Json resolved_scene(const std::string& document) const;
    authoring::AuthoringService& authoring() {
        return authoring_;
    }
    static Json object_schema(Json properties, Json required = Json::array());

  private:
    struct Command {
        Json descriptor;
        Handler handler;
    };
    authoring::AuthoringService& authoring_;
    std::map<std::string, Command> commands_;
};
} // namespace faset::editor
