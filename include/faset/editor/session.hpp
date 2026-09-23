#pragma once
#include <faset/assets/asset_pipeline.hpp>
#include <faset/core/process.hpp>
#include <faset/editor/build_service.hpp>
#include <faset/editor/commands.hpp>
#include <faset/editor/plugins.hpp>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace faset::editor {
struct SessionConfig {
    std::filesystem::path project_root, engine_root, binary_directory;
};
class Session {
  public:
    explicit Session(SessionConfig);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    authoring::AuthoringService& authoring() {
        return authoring_;
    }
    Commands& commands() {
        return commands_;
    }
    void poll();
    const std::vector<std::string>& logs() const {
        return logs_;
    }
    Json project() const;
    Json plugin_panels() const {
        return plugins_ ? plugins_->panels() : Json::array();
    }
    void scaffold(const std::string& name, int dimension);
    void scaffold(const std::string& name, int dimension, std::string_view language);
    const SessionConfig& config() const {
        return config_;
    }
    bool playing() const {
        return bool(player_);
    }
    bool play_pending() const {
        return !pending_play_job_.empty();
    }
    void log(std::string message);

  private:
    struct ImportTask;
    void register_commands();
    Json assets_list() const;
    Json jobs() const;
    std::string source_signature() const;
    Json schema_status() const;
    Json job(const std::string& id) const;
    void load_schema(const std::filesystem::path& path);
    void launch_player(Json scene, const std::filesystem::path& executable);
    void stop_player();
    SessionConfig config_;
    authoring::AuthoringService authoring_;
    Commands commands_;
    assets::AssetPipeline assets_;
    BuildService builds_;
    std::map<std::string, std::shared_ptr<ImportTask>> imports_;
    std::vector<std::jthread> workers_;
    std::vector<std::string> logs_;
    std::map<std::string, std::string> observed_jobs_;
    std::unique_ptr<Process> player_;
    std::filesystem::path control_path_;
    std::uint64_t control_sequence_ = 0;
    std::string pending_play_job_;
    Json pending_play_scene_;
    std::unique_ptr<PluginManager> plugins_;
    std::map<std::string, std::string> submitted_sources_;
    std::string schema_source_signature_;
    bool schema_loaded_ = false;
    std::string schema_error_;
};
} // namespace faset::editor
