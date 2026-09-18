#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace faset {
struct ProcessOptions {
    // First element is the executable. Arguments are passed directly, never through a shell.
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::map<std::string, std::string> environment;
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
} // namespace faset
