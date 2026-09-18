#include <algorithm>
#include <chrono>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/editor/session.hpp>

namespace faset::editor {
namespace {
std::string executable_name(const std::string& name) {
#ifdef _WIN32
    return name + ".exe";
#else
    return name;
#endif
}
BuildConfig build_config(const SessionConfig& config) {
    BuildConfig result;
    result.project_root = config.project_root;
    result.engine_root = config.engine_root;
    result.build_directory = config.project_root / ".faset/build";
    result.cache_root = config.project_root / ".faset/cache";
    return result;
}
Json resolved_or_throw(Commands& commands, const std::string& id) {
    const auto resolved = commands.resolved_scene(id);
    if (!resolved.at("conflicts").empty())
        throw Error("template.conflicts", "Resolve template conflicts before Play or export",
                    {{"conflicts", resolved.at("conflicts")}});
    return resolved.at("scene");
}
} // namespace
struct Session::ImportTask {
    std::string id;
    std::shared_ptr<assets::ImportJob> job = std::make_shared<assets::ImportJob>();
    mutable std::mutex mutex;
    std::string state = "queued", error;
    Json result = Json::object();
    Json json() const {
        std::lock_guard lock(mutex);
        const auto progress = job->progress();
        return {{"id", id},
                {"kind", "import"},
                {"state", state},
                {"stage", progress.stage},
                {"progress", progress.fraction},
                {"error", error},
                {"result", result}};
    }
};
Session::Session(SessionConfig config)
    : config_(std::move(config)), authoring_(config_.project_root), commands_(authoring_),
      assets_(config_.project_root / ".faset/cache"), builds_(build_config(config_)) {
    register_commands();
    plugins_ = std::make_unique<PluginManager>(
        commands_, [this](std::string message) { log(std::move(message)); });
    plugins_->load(config_.project_root / "Plugins");
    const auto schema = config_.project_root / ".faset/schema.json";
    if (std::filesystem::exists(schema))
        try {
            load_schema(schema);
        } catch (const std::exception& error) {
            log(std::string("Schema load failed: ") + error.what());
        }
}
Session::~Session() {
    for (const auto& [id, task] : imports_)
        task->job->cancel();
    workers_.clear();
    stop_player();
}
void Session::log(std::string value) {
    if (value.empty())
        return;
    logs_.push_back(std::move(value));
    if (logs_.size() > 1000)
        logs_.erase(logs_.begin(), logs_.begin() + 100);
}
Json Session::project() const {
    const auto path = config_.project_root / "project.faset.json";
    if (std::filesystem::exists(path))
        return read_json(path);
    return {{"format", "faset.project"},
            {"version", 1},
            {"name", config_.project_root.filename().string()},
            {"dimension", 3}};
}
void Session::scaffold(const std::string& name, int dimension) {
    builds_.scaffold(name, dimension);
    log("Created project: " + name);
}
Json Session::assets_list() const {
    Json list = Json::array();
    const auto directory = assets_.cache_root() / "assets";
    if (std::filesystem::exists(directory))
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_directory()) {
                try {
                    const auto id = entry.path().filename().string();
                    const auto manifest = assets_.current_manifest(id);
                    list.push_back({{"id", id}, {"manifest", manifest}});
                } catch (const std::exception& error) {
                    list.push_back(
                        {{"id", entry.path().filename().string()}, {"error", error.what()}});
                }
            }
    return {{"assets", list}};
}
Json Session::jobs() const {
    Json list = Json::array();
    for (const auto& item : builds_.jobs())
        list.push_back(item.json());
    for (const auto& [id, task] : imports_)
        list.push_back(task->json());
    return {{"jobs", list}};
}
Json Session::job(const std::string& id) const {
    if (imports_.contains(id))
        return imports_.at(id)->json();
    return builds_.job(id).json();
}
void Session::load_schema(const std::filesystem::path& path) {
    const auto value = read_json(path);
    authoring_.replace_external_schemas(value);
    const auto output = config_.project_root / ".faset/schema.json";
    if (std::filesystem::weakly_canonical(path) != std::filesystem::weakly_canonical(output))
        atomic_write_json(output, value);
    log("Gameplay schema loaded");
}
void Session::launch_player(Json scene, const std::filesystem::path& executable) {
    require(std::filesystem::is_regular_file(executable), "play.missing_player",
            "Build the Player before starting Play");
    const auto directory = config_.project_root / ".faset/play" / new_id();
    std::filesystem::create_directories(directory);
    const auto snapshot = directory / "scene.fscene";
    write_cooked_scene(snapshot, scene);
    control_path_ = directory / "control.json";
    control_sequence_ = 0;
    ProcessOptions options;
    options.arguments = {
        executable.string(),           "--scene",   snapshot.string(),     "--assets",
        assets_.cache_root().string(), "--control", control_path_.string()};
    options.working_directory = config_.project_root;
    player_ = std::make_unique<Process>(options);
    log("Play started in a separate Player process");
}
void Session::stop_player() {
    if (!pending_play_job_.empty()) {
        builds_.cancel(pending_play_job_);
        pending_play_job_.clear();
        pending_play_scene_ = nullptr;
    }
    if (player_) {
        player_->cancel();
        const auto result = player_->poll();
        log(result.output);
        player_.reset();
        log("Play stopped; authoring scene unchanged");
    }
}
void Session::poll() {
    if (player_) {
        const auto result = player_->poll();
        log(result.output);
        if (!result.running) {
            log("Player exited with code " + std::to_string(result.exit_code.value_or(-1)));
            player_.reset();
        }
    }
    for (const auto& value : builds_.jobs()) {
        if (observed_jobs_[value.id] == value.state)
            continue;
        observed_jobs_[value.id] = value.state;
        if (value.state == "failed")
            log(value.kind + " failed: " + value.error);
        if (value.state == "succeeded") {
            log(value.kind + " completed");
            if (value.result.contains("schema"))
                try {
                    load_schema(value.result.at("schema").get<std::string>());
                } catch (const std::exception& error) {
                    log(std::string("Schema update failed: ") + error.what());
                }
        }
        if (value.id == pending_play_job_ && value.finished()) {
            pending_play_job_.clear();
            if (value.state == "succeeded")
                try {
                    const auto executable =
                        value.result.value("player", (builds_.config().build_directory /
                                                      executable_name("faset_player"))
                                                         .string());
                    launch_player(pending_play_scene_, executable);
                } catch (const std::exception& error) {
                    log(std::string("Play failed: ") + error.what());
                }
            pending_play_scene_ = nullptr;
        }
    }
    for (const auto& [id, task] : imports_) {
        const auto value = task->json();
        const auto status = value.at("state").get<std::string>();
        if (observed_jobs_[id] == status)
            continue;
        observed_jobs_[id] = status;
        if (status == "succeeded")
            log("Asset import completed: " + value["result"].value("asset_id", std::string()));
        if (status == "failed" || status == "conflict")
            log("Asset import " + status + ": " + value.value("error", std::string()));
    }
}
void Session::register_commands() {
    const Json text = {{"type", "string"}}, boolean = {{"type", "boolean"}};
    auto schema = [](Json properties, Json required = Json::array()) {
        return Commands::object_schema(std::move(properties), std::move(required));
    };
    commands_.add(
        "faset_capabilities", "Inspect available Editor services and rendering capabilities.",
        schema(Json::object()),
        [&](const Json&) {
            bool screenshot = false;
            for (const auto& command : commands_.list())
                if (command.at("name") == "faset_editor_capture")
                    screenshot = true;
            return Json{{"authoring", true},
                        {"build", true},
                        {"import", true},
                        {"plugins", true},
                        {"viewport_capture", screenshot},
                        {"runtime_entity_access", false},
                        {"protocol", "2025-06-18"}};
        },
        true);
    commands_.add(
        "faset_plugins", "Inspect startup-loaded Editor plugins and exact SDK compatibility.",
        schema(Json::object()),
        [&](const Json&) {
            return Json{{"sdk_fingerprint", PluginManager::fingerprint()},
                        {"plugins", plugins_->status()},
                        {"panels", plugins_->panels()}};
        },
        true);
    commands_.add(
        "faset_project", "Read the authoring project's settings.", schema(Json::object()),
        [&](const Json&) { return project(); }, true);
    commands_.add(
        "faset_assets", "List imported asset manifests and resource identities.",
        schema(Json::object()), [&](const Json&) { return assets_list(); }, true);
    commands_.add("faset_import",
                  "Import GLB/glTF or a Blender export manifest relative to this project. Returns "
                  "a cancellable job ID; failure retains the last successful generation.",
                  schema({{"path", text},
                          {"settings", {{"type", "object"}}},
                          {"allow_removed_outputs", boolean}},
                         {"path"}),
                  [&](const Json& args) {
                      assets::ImportRequest request;
                      request.source =
                          project_path(config_.project_root, args.at("path").get<std::string>());
                      request.settings = args.value("settings", Json(nullptr));
                      request.allow_removed_outputs = args.value("allow_removed_outputs", false);
                      auto task = std::make_shared<ImportTask>();
                      task->id = "import-" + new_id();
                      imports_[task->id] = task;
                      workers_.emplace_back([this, task, request] {
                          {
                              std::lock_guard lock(task->mutex);
                              task->state = "running";
                          }
                          try {
                              const auto result = assets_.import_asset(request, *task->job);
                              std::lock_guard lock(task->mutex);
                              task->state =
                                  result.status == assets::ImportStatus::succeeded   ? "succeeded"
                                  : result.status == assets::ImportStatus::cancelled ? "cancelled"
                                  : result.status == assets::ImportStatus::conflict  ? "conflict"
                                                                                     : "failed";
                              task->result = {{"asset_id", result.asset_id},
                                              {"generation", result.generation},
                                              {"diagnostics", result.diagnostics},
                                              {"cache_hit", result.cache_hit},
                                              {"manifest", result.manifest}};
                              for (const auto& message : result.diagnostics)
                                  task->error += message + "\n";
                          } catch (const std::exception& error) {
                              std::lock_guard lock(task->mutex);
                              task->state = "failed";
                              task->error = error.what();
                          }
                      });
                      return Json{{"job", task->id}};
                  });
    commands_.add("faset_build",
                  "Incrementally compile C++ gameplay and export its metadata in separate native "
                  "processes. Returns a job ID.",
                  schema(Json::object()),
                  [&](const Json&) { return Json{{"job", builds_.start_build()}}; });
    commands_.add("faset_export",
                  "Build, validate and export a resolved authoring snapshot to a project-relative "
                  "output directory. Returns a job ID.",
                  schema({{"document", text}, {"output", text}}, {"document", "output"}),
                  [&](const Json& args) {
                      return Json{{"job", builds_.start_export(
                                              resolved_or_throw(commands_, args.at("document")),
                                              project_path(config_.project_root,
                                                           args.at("output").get<std::string>()))}};
                  });
    commands_.add(
        "faset_jobs", "List editor import/build/export jobs and their progress.",
        schema(Json::object()), [&](const Json&) { return jobs(); }, true);
    commands_.add(
        "faset_job", "Read an editor job's progress, result and diagnostics.",
        schema({{"id", text}}, {"id"}), [&](const Json& args) { return job(args.at("id")); }, true);
    commands_.add("faset_job_cancel",
                  "Cancel an editor job. Cancellation is separate from authoring Undo.",
                  schema({{"id", text}}, {"id"}), [&](const Json& args) {
                      const auto id = args.at("id").get<std::string>();
                      if (imports_.contains(id))
                          imports_.at(id)->job->cancel();
                      else
                          builds_.cancel(id);
                      return Json{{"cancel_requested", true}};
                  });
    commands_.add("faset_play",
                  "Build gameplay, then start a separate Player from an immutable snapshot of the "
                  "current authoring document. Returns the build job ID.",
                  schema({{"document", text}}, {"document"}), [&](const Json& args) {
                      stop_player();
                      pending_play_scene_ = resolved_or_throw(commands_, args.at("document"));
                      pending_play_job_ = builds_.start_build();
                      return Json{{"job", pending_play_job_}, {"play_pending", true}};
                  });
    commands_.add(
        "faset_stop",
        "Stop editor Play or cancel its pending build. Does not modify the authoring scene.",
        schema(Json::object()), [&](const Json&) {
            stop_player();
            return Json{{"stopped", true}};
        });
    commands_.add("faset_play_control",
                  "Pause, resume, or single-step the Editor's Player session. No game entities or "
                  "state are exposed.",
                  schema({{"command", {{"type", "string"}, {"enum", {"pause", "resume", "step"}}}}},
                         {"command"}),
                  [&](const Json& args) {
                      require(bool(player_), "play.not_running", "Player is not running");
                      const auto command = args.at("command").get<std::string>();
                      require(command == "pause" || command == "resume" || command == "step",
                              "play.command", "Unknown Play control");
                      atomic_write_json(control_path_,
                                        {{"sequence", ++control_sequence_}, {"command", command}});
                      return Json{{"queued", true}};
                  });
    commands_.add(
        "faset_editor_logs",
        "Read compiler, importer and process diagnostics collected by this Editor session.",
        schema(Json::object()), [&](const Json&) { return Json{{"logs", logs_}}; }, true);
}
} // namespace faset::editor
