#include <faset/core/io.hpp>
#include <faset/editor/mcp.hpp>
#include <iostream>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Check failed at " + std::to_string(__LINE__) + ": " #x);     \
    } while (false)
int main() {
    using namespace faset;
    using namespace faset::editor;
    const auto root = std::filesystem::temp_directory_path() / ("faset-mcp-" + new_id());
    try {
        authoring::AuthoringService service(root);
        Commands commands(service);
        McpServer server(commands);
        auto request = [&](std::string method, Json params = Json::object()) {
            auto result = server.handle(
                {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
            CHECK(result.has_value());
            return *result;
        };
        CHECK(request("tools/list")["error"]["code"] == -32002);
        CHECK(request("initialize",
                      {{"protocolVersion", "2025-06-18"},
                       {"capabilities", Json::object()},
                       {"clientInfo",
                        {{"name", "test"}, {"version", "1"}}}})["result"]["protocolVersion"] ==
              "2025-06-18");
        CHECK(!server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
        const auto listed = request("tools/list")["result"]["tools"];
        CHECK(listed.size() >= 10);
        for (const auto& tool : listed) {
            const std::string name = tool["name"];
            CHECK(name.find("runtime") == std::string::npos);
            CHECK(tool["inputSchema"]["additionalProperties"] == false);
        }
        auto call = [&](std::string name, Json arguments = Json::object()) {
            return request("tools/call", {{"name", name}, {"arguments", arguments}})["result"];
        };
        const auto created =
            call("faset_document_create", {{"name", "MCP scene"}, {"dimension", 2}});
        CHECK(created["isError"] == false);
        const std::string id = created["structuredContent"]["id"];
        Json args = {{"document", id},
                     {"revision", 0},
                     {"operations", Json::array({{{"op", "entity.create"}, {"name", "Player"}}})},
                     {"idempotency_key", "first"}};
        const auto first = call("faset_scene_edit", args);
        CHECK(first["isError"] == false);
        CHECK(call("faset_scene_edit", args) == first);
        args.erase("idempotency_key");
        CHECK(call("faset_scene_edit", args)["structuredContent"]["error"]["code"] ==
              "revision.conflict");
        CHECK(service.query(id)["scene"] == first["structuredContent"]["scene"]);
        CHECK(call("faset_scene_edit", {{"document", id},
                                        {"revision", -1},
                                        {"operations", args["operations"]}})["isError"] == true);
        CHECK(call("faset_document_open",
                   {{"path", "../outside.json"}})["structuredContent"]["error"]["code"] ==
              "path.outside_project");
        CHECK(call("faset_runtime_query")["isError"] == true);
        CHECK(request("resources/read", {{"uri", "faset://schema"}}).contains("result"));
        CHECK(request("not/a/method")["error"]["code"] == -32601);
        CHECK(server.handle(Json::array())->at("error").at("code") == -32600);
        std::filesystem::remove_all(root);
        std::cout << "MCP lifecycle, shared authoring, retries/conflicts, tool schemas and "
                     "editor-only boundary passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
