#include <array>
#include <faset/editor/plugin_api.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
struct State {
    const FasetEditorHost* host;
};
void collect(void* target, const char* data, uint64_t size) {
    static_cast<std::string*>(target)->append(data, size);
}
Json invoke(State& state, const char* command, const Json& arguments) {
    std::string output;
    const auto input = arguments.dump();
    const auto status =
        state.host->invoke_command(state.host->context, command, input.c_str(), collect, &output);
    auto result = Json::parse(output);
    if (status != 0)
        throw std::runtime_error(result.value("message", std::string("Editor command failed")));
    return result;
}
std::string id() {
    std::random_device random;
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
        stream << std::setw(8) << random();
    return stream.str();
}
int create(void* user, const char* input, FasetWrite write, void* receiver) {
    try {
        auto& state = *static_cast<State*>(user);
        const auto arguments = Json::parse(input);
        const auto document =
            invoke(state, "faset_document_query", {{"document", arguments.at("document")}});
        const auto schema = invoke(state, "faset_schema", Json::object());
        bool known = false;
        for (const auto& type : schema.at("types"))
            if (type.at("id") == "example.beacon")
                known = true;
        if (!known)
            throw std::runtime_error("Include Beacon.hpp in gameplay, register its schema and "
                                     "behavior, then Build before adding a Beacon");
        const auto entity = id();
        Json record = {
            {"id", entity},
            {"name", "Beacon"},
            {"parent", nullptr},
            {"components",
             Json::array(
                 {{{"id", id()},
                   {"type", "faset.transform"},
                   {"version", 1},
                   {"fields",
                    {{"position", {0, 1, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}}}},
                  {{"id", id()},
                   {"type", "faset.mesh"},
                   {"version", 1},
                   {"fields",
                    {{"asset", ""}, {"primitive", "cube"}, {"color", {0.68, 0.55, 0.95, 1}}}}},
                  {{"id", id()},
                   {"type", "example.beacon"},
                   {"version", 1},
                   {"fields", {{"speed", 1.0}}}}})}};
        const auto result =
            invoke(state, "faset_scene_edit",
                   {{"document", document.at("id")},
                    {"revision", document.at("revision")},
                    {"operations", Json::array({{{"op", "entity.create"}, {"entity", record}}})}})
                .dump();
        write(receiver, result.data(), result.size());
        return 0;
    } catch (const std::exception& error) {
        const auto output = Json{{"code", "beacon.create"}, {"message", error.what()}}.dump();
        write(receiver, output.data(), output.size());
        return 1;
    }
}
void shutdown(void* user) {
    delete static_cast<State*>(user);
}
} // namespace
extern "C" FASET_PLUGIN_EXPORT int faset_editor_plugin(const FasetEditorHost* host,
                                                       FasetEditorPlugin* plugin) {
    if (!host || !plugin || host->api_version != FASET_EDITOR_API_VERSION ||
        host->struct_size != sizeof(FasetEditorHost) ||
        std::string(host->build_fingerprint) != FASET_EDITOR_SDK_FINGERPRINT)
        return 1;
    try {
        auto* state = new State{host};
        *plugin = {FASET_EDITOR_API_VERSION, sizeof(FasetEditorPlugin),
                   FASET_EDITOR_SDK_FINGERPRINT, state, shutdown};
        const auto descriptor = Json{
            {"name", "plugin_example_beacon_create"},
            {"description", "Create a rotating Beacon using one authoring transaction."},
            {"inputSchema",
             {{"type", "object"},
              {"properties", {{"document", {{"type", "string"}}}}},
              {"required", {"document"}},
              {"additionalProperties",
               false}}}}.dump();
        if (host->register_command(host->context, descriptor.c_str(), create, state) != 0)
            return 1;
        const auto panel = Json{{"id", "example.beacon.tools"},
                                {"title", "Beacon tools"},
                                {"action", "Add Beacon"},
                                {"command", "plugin_example_beacon_create"},
                                {"arguments", {{"document", "$document"}}}}
                               .dump();
        return host->register_panel(host->context, panel.c_str());
    } catch (...) {
        return 1;
    }
}
