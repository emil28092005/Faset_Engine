#pragma once
#include <faset/editor/commands.hpp>
#include <functional>
#include <memory>
namespace faset::editor {
class PluginManager {
  public:
    using Logger = std::function<void(std::string)>;
    explicit PluginManager(Commands&, Logger);
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    // Discover and validate the complete dependency graph before loading code.
    // Individual failures are reported; dependents are never loaded after one.
    void load(const std::filesystem::path& directory);
    Json status() const;
    Json panels() const;
    static std::string fingerprint();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace faset::editor
