#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace faset {
struct ProcessOptions {
    // UTF-8 text; the first element is the executable (use path_to_utf8 for paths).
    // Arguments are passed directly, never through a shell.
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::map<std::string, std::string> environment; // UTF-8 names and values.
};
struct ProcessPoll {
    bool running{};
    std::optional<int> exit_code;
    std::string output; // Newly available stdout and stderr, combined.
};
class Process {
  public:
    explicit Process(const ProcessOptions&);
    ~Process();
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    ProcessPoll poll();
    // Terminates the process group/job, including its compiler children.
    void cancel();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::filesystem::path find_executable(const std::string& name);
// Launch an explicitly selected external application without an owning Process/job.
// UTF-8 arguments remain literal (no shell); inherited environment, discarded stdio.
// This does not grant permission to execute arbitrary project files as programs.
void launch_detached(const std::vector<std::string>& arguments,
                     const std::filesystem::path& working_directory = {});
} // namespace faset
