#pragma once
#include <faset/editor/commands.hpp>
#include <optional>
#include <string>
#include <vector>

namespace faset::editor {
// MCP is compiled exclusively into the Editor / headless authoring executable.
class McpServer {
  public:
    explicit McpServer(Commands& commands) : commands_(commands) {}
    std::optional<Json> handle(const Json& message);
    Json parse_error() const;

  private:
    Commands& commands_;
    bool initialized_ = false, ready_ = false;
};
class StdioTransport {
  public:
    // Nonblocking: GUI and MCP can share the same authoring session and event loop.
    std::vector<std::string> poll();
    bool closed() const noexcept {
        return closed_;
    }
    void send(const Json& message);

  private:
    std::string buffer_;
    bool closed_ = false;
};
} // namespace faset::editor
