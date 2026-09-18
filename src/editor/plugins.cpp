#include <algorithm>
#include <cctype>
#include <faset/core/io.hpp>
#include <faset/editor/plugin_api.h>
#include <faset/editor/plugins.hpp>
#include <set>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace faset::editor {
namespace {
struct ResponseBuffer {
    std::string bytes;
    bool failed = false;
};
void append(void* context, const char* bytes, std::uint64_t size) noexcept {
    auto& output = *static_cast<ResponseBuffer*>(context);
    if (output.failed)
        return;
    if ((!bytes && size) || size > 8 * 1024 * 1024 ||
        output.bytes.size() + size > 8 * 1024 * 1024) {
        output.failed = true;
        return;
    }
    try {
        if (size)
            output.bytes.append(bytes, static_cast<std::size_t>(size));
    } catch (...) {
        output.failed = true;
    }
}
Json response(const std::string& bytes) {
    auto value = Json::parse(bytes);
    require(value.is_object(), "plugin.response", "Plugin response must be a JSON object");
    return value;
}
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return c < 128 && (std::isalnum(c) || c == '.' || c == '_' || c == '-');
           });
}
std::string prefix(std::string id) {
    for (auto& c : id)
        if (c == '.' || c == '-')
            c = '_';
    return "plugin_" + id + "_";
}
} // namespace
struct PluginManager::Impl {
    struct Registration {
        Json descriptor;
        FasetCommand callback = nullptr;
        void* user = nullptr;
    };
    struct Module {
        Impl* owner = nullptr;
        Json manifest;
        std::filesystem::path directory;
        std::vector<Registration> commands;
        Json panels = Json::array();
        std::vector<std::string> installed;
        FasetEditorHost host{};
        FasetEditorPlugin plugin{};
        void* library = nullptr;
        bool ready = false;
        std::string failure;
        ~Module() {
            ready = false;
            for (const auto& name : installed)
                owner->commands.remove(name);
            if (plugin.shutdown)
                try {
                    plugin.shutdown(plugin.user);
                } catch (...) { /* Plugin contract forbids exceptions. */
                }
#ifdef _WIN32
            if (library)
                FreeLibrary(static_cast<HMODULE>(library));
#else
            if (library)
                dlclose(library);
#endif
        }
        std::string id() const {
            return manifest.at("id");
        }
        void check_thread() const {
            require(std::this_thread::get_id() == owner->thread, "plugin.thread",
                    "Editor SDK calls must run on the Editor thread");
        }
    };
    Commands& commands;
    Logger logger;
    std::thread::id thread = std::this_thread::get_id();
    std::vector<std::unique_ptr<Module>> modules;
    Json records = Json::array();
    bool attempted = false;
    unsigned call_depth = 0;
    Impl(Commands& value, Logger output) : commands(value), logger(std::move(output)) {}
    ~Impl() {
        while (!modules.empty())
            modules.pop_back();
    }
    static int command(void* context, const char* descriptor, FasetCommand callback, void* user) {
        auto& module = *static_cast<Module*>(context);
        try {
            module.check_thread();
            require(!module.ready, "plugin.registration_closed", "Registrations are startup-only");
            require(callback, "plugin.callback", "Command callback is missing");
            const auto value = Json::parse(descriptor);
            const auto name = value.at("name").get<std::string>();
            require(name.starts_with(prefix(module.id())) && identifier(name),
                    "plugin.command_owner", "Plugin command must use its module prefix");
            require(value.at("description").is_string() &&
                        value.at("inputSchema").at("type") == "object" &&
                        value.at("inputSchema").at("properties").is_object(),
                    "plugin.command_schema", "Command requires an object input schema");
            for (const auto& prior : module.commands)
                require(prior.descriptor.at("name") != name, "plugin.command_duplicate",
                        "Duplicate plugin command");
            module.commands.push_back({value, callback, user});
            return 0;
        } catch (const std::exception& error) {
            module.failure = error.what();
            return 1;
        }
    }
    static int panel(void* context, const char* descriptor) {
        auto& module = *static_cast<Module*>(context);
        try {
            module.check_thread();
            require(!module.ready, "plugin.registration_closed", "Registrations are startup-only");
            auto value = Json::parse(descriptor);
            require(value.at("id").get<std::string>().starts_with(module.id() + "."),
                    "plugin.panel_owner", "Panel ID must belong to its module");
            require(value.at("title").is_string() && value.at("command").is_string(),
                    "plugin.panel", "Panel needs a title and command");
            if (!value.contains("arguments"))
                value["arguments"] = Json::object();
            require(value["arguments"].is_object(), "plugin.panel",
                    "Panel arguments must be an object");
            for (const auto& previous : module.panels)
                require(previous["id"] != value["id"], "plugin.panel_duplicate",
                        "Duplicate panel ID");
            value["owner"] = module.id();
            module.panels.push_back(std::move(value));
            return 0;
        } catch (const std::exception& error) {
            module.failure = error.what();
            return 1;
        }
    }
    static int invoke(void* context, const char* name, const char* arguments, FasetWrite write,
                      void* receiver) {
        auto& module = *static_cast<Module*>(context);
        bool entered = false;
        try {
            module.check_thread();
            require(module.ready, "plugin.not_ready",
                    "Editor commands become available after plugin startup");
            require(module.owner->call_depth < 32, "plugin.recursion",
                    "Plugin command recursion limit exceeded");
            ++module.owner->call_depth;
            entered = true;
            const auto output = module.owner->commands.call(name, Json::parse(arguments)).dump();
            --module.owner->call_depth;
            entered = false;
            write(receiver, output.data(), output.size());
            return 0;
        } catch (const std::exception& error) {
            if (entered)
                --module.owner->call_depth;
            const auto* known = dynamic_cast<const Error*>(&error);
            const auto output =
                (known ? known->json()
                       : Json{{"code", "plugin.command"}, {"message", error.what()}})
                    .dump();
            if (write)
                try {
                    write(receiver, output.data(), output.size());
                } catch (...) {
                }
            return 1;
        }
    }
    static void log(void* context, const char* text) {
        auto& module = *static_cast<Module*>(context);
        try {
            module.check_thread();
            module.owner->logger(module.id() + ": " + text);
        } catch (...) {
        }
    }
    void activate(const Json& manifest, const std::filesystem::path& directory) {
        auto module = std::make_unique<Module>();
        module->owner = this;
        module->manifest = manifest;
        module->directory = directory;
        const auto path =
            project_path(directory, path_from_utf8(manifest.at("library").get<std::string>()));
        require(std::filesystem::is_regular_file(path), "plugin.library",
                "Plugin library is missing");
#ifdef _WIN32
        module->library =
            LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto entry = module->library
                         ? reinterpret_cast<FasetPluginEntry>(GetProcAddress(
                               static_cast<HMODULE>(module->library), "faset_editor_plugin"))
                         : nullptr;
#else
        module->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        const auto error = module->library ? nullptr : dlerror();
        require(module->library, "plugin.load", error ? error : "Cannot load plugin library");
        auto entry =
            reinterpret_cast<FasetPluginEntry>(dlsym(module->library, "faset_editor_plugin"));
#endif
        require(module->library && entry, "plugin.entry",
                "Library does not export faset_editor_plugin");
        module->host = {FASET_EDITOR_API_VERSION,
                        sizeof(FasetEditorHost),
                        FASET_EDITOR_SDK_FINGERPRINT,
                        module.get(),
                        command,
                        panel,
                        invoke,
                        log};
        require(entry(&module->host, &module->plugin) == 0, "plugin.startup",
                "Plugin startup failed");
        require(module->failure.empty(), "plugin.registration", module->failure);
        require(module->plugin.api_version == FASET_EDITOR_API_VERSION &&
                    module->plugin.struct_size == sizeof(FasetEditorPlugin),
                "plugin.api", "Plugin returned an incompatible API");
        require(module->plugin.build_fingerprint &&
                    std::string(module->plugin.build_fingerprint) == FASET_EDITOR_SDK_FINGERPRINT,
                "plugin.build", "Plugin binary does not match this Editor SDK build");
        for (const auto& value : module->panels) {
            const auto name = value.at("command").get<std::string>();
            require(std::any_of(module->commands.begin(), module->commands.end(),
                                [&](const auto& reg) { return reg.descriptor.at("name") == name; }),
                    "plugin.panel_command", "Panel command must be registered by its owner");
        }
        for (const auto& registration : module->commands) {
            const auto& descriptor = registration.descriptor;
            const auto name = descriptor.at("name").get<std::string>();
            commands.add(
                name, descriptor.at("description"), descriptor.at("inputSchema"),
                [registration](const Json& arguments) {
                    ResponseBuffer output;
                    const auto input = arguments.dump();
                    const int result =
                        registration.callback(registration.user, input.c_str(), append, &output);
                    require(!output.failed, "plugin.output_limit",
                            "Cannot collect plugin response (maximum 8 MiB)");
                    const auto value = response(output.bytes);
                    if (result != 0)
                        throw Error(value.value("code", std::string("plugin.failed")),
                                    value.value("message", std::string("Plugin command failed")),
                                    value);
                    return value;
                },
                descriptor.value("read_only", false));
            module->installed.push_back(name);
        }
        module->ready = true;
        logger("Loaded editor plugin: " + module->id());
        modules.push_back(std::move(module));
    }
};
PluginManager::PluginManager(Commands& commands, Logger logger)
    : impl_(std::make_unique<Impl>(commands, std::move(logger))) {}
PluginManager::~PluginManager() = default;
std::string PluginManager::fingerprint() {
    return FASET_EDITOR_SDK_FINGERPRINT;
}
Json PluginManager::status() const {
    return impl_->records;
}
Json PluginManager::panels() const {
    Json result = Json::array();
    for (const auto& module : impl_->modules)
        for (const auto& panel : module->panels)
            result.push_back(panel);
    return result;
}
void PluginManager::load(const std::filesystem::path& directory) {
    require(!impl_->attempted, "plugin.restart_required",
            "Plugin discovery runs once; restart the Editor after changing packages");
    impl_->attempted = true;
    if (!std::filesystem::exists(directory))
        return;
    struct Source {
        Json manifest;
        std::filesystem::path directory;
    };
    std::map<std::string, Source> sources;
    std::set<std::string> invalid;
    auto failure = [&](const std::string& id, const std::string& message) {
        invalid.insert(id);
        impl_->records.push_back({{"id", id}, {"state", "failed"}, {"message", message}});
        impl_->logger("Plugin " + id + ": " + message);
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.is_regular_file() &&
            path_to_utf8(entry.path().filename()).ends_with(".faset-plugin.json")) {
            std::string id = path_to_utf8(entry.path().filename());
            try {
                const auto manifest = read_json(entry.path());
                id = manifest.at("id");
                require(identifier(id), "plugin.id", "Invalid module ID");
                require(!sources.contains(id), "plugin.duplicate", "Duplicate module ID");
                require(manifest.at("format") == "faset.editor_plugin" &&
                            manifest.at("version") == 1 && manifest.at("kind") == "editor",
                        "plugin.manifest", "Unsupported editor plugin manifest");
                require(manifest.at("module_version").is_string() &&
                            manifest.at("dependencies").is_array(),
                        "plugin.manifest", "Plugin needs version and dependency list");
                require(manifest.at("api_version") == FASET_EDITOR_API_VERSION &&
                            manifest.at("build_fingerprint") == fingerprint(),
                        "plugin.compatibility",
                        "Plugin manifest does not match this Editor SDK; rebuild it");
                project_path(entry.path().parent_path(),
                             path_from_utf8(manifest.at("library").get<std::string>()));
                sources.emplace(id, Source{manifest, entry.path().parent_path()});
            } catch (const std::exception& error) {
                failure(id, error.what());
            }
        }
    std::map<std::string, int> colors;
    std::vector<std::string> order;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        require(sources.contains(id) && !invalid.contains(id), "plugin.dependency",
                "Missing or invalid dependency: " + id);
        require(colors[id] != 1, "plugin.cycle", "Plugin dependency cycle at " + id);
        if (colors[id] == 2)
            return;
        colors[id] = 1;
        for (const auto& dependency : sources.at(id).manifest.at("dependencies")) {
            const std::string required = dependency.at("id");
            visit(required);
            require(sources.at(required).manifest.at("module_version") == dependency.at("version"),
                    "plugin.dependency_version", "Dependency version mismatch: " + required);
        }
        colors[id] = 2;
        order.push_back(id);
    };
    for (const auto& [id, source] : sources)
        try {
            visit(id);
        } catch (const std::exception& error) {
            failure(id, error.what());
        }
    std::set<std::string> loaded;
    for (const auto& id : order)
        if (!invalid.contains(id))
            try {
                const auto& source = sources.at(id);
                for (const auto& dependency : source.manifest.at("dependencies"))
                    require(loaded.contains(dependency.at("id").get<std::string>()),
                            "plugin.dependency_failed", "A required plugin failed to load");
                impl_->activate(source.manifest, source.directory);
                loaded.insert(id);
                impl_->records.push_back({{"id", id},
                                          {"version", source.manifest.at("module_version")},
                                          {"state", "loaded"}});
            } catch (const std::exception& error) {
                failure(id, error.what());
            }
}
} // namespace faset::editor
