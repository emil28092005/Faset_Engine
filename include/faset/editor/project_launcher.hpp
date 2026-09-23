#pragma once
#include <faset/ui/ui.hpp>
#include <optional>

namespace faset::editor {
struct ProjectSelection {
    std::filesystem::path path;
    std::string name;
    int dimension = 3;
    bool create = false;
    std::string language = "cpp";
};
// Records only existing, valid projects. Call after successful Session setup.
void remember_project(const std::filesystem::path& project);
class ProjectLauncher {
  public:
    ProjectLauncher(render::Renderer&, const std::filesystem::path& engine_root,
                    const std::filesystem::path& initial_project = {},
                    const std::filesystem::path& recent_store = {},
                    const std::optional<ProjectSelection>& retry = {},
                    std::string creation_error = {});
    ~ProjectLauncher();
    void frame(const std::vector<render::Event>&);
    const render::Snapshot& snapshot() const;
    ui::Context& widgets();
    const std::optional<ProjectSelection>& selection() const;
    bool cancelled() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::optional<ProjectSelection>
run_project_launcher(const std::filesystem::path& engine_root,
                     const std::filesystem::path& initial_project = {},
                     std::uint64_t max_frames = 0, const std::filesystem::path& capture = {},
                     const std::optional<ProjectSelection>& retry = {},
                     std::string creation_error = {});
} // namespace faset::editor
