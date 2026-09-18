#include <algorithm>
#include <faset/editor/mcp.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace faset::editor {
namespace {
Json rpc_error(Json id, int code, std::string message, Json data = Json::object()) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", std::move(message)}, {"data", std::move(data)}}}};
}
} // namespace
Json McpServer::parse_error() const {
    return rpc_error(nullptr, -32700, "Parse error");
}
std::optional<Json> McpServer::handle(const Json& message) {
    if (!message.is_object() || !message.contains("jsonrpc") || message["jsonrpc"] != "2.0" ||
        !message.contains("method") || !message["method"].is_string())
        return rpc_error(nullptr, -32600, "Invalid Request");
    const auto method = message.at("method").get<std::string>();
    if (!message.contains("id")) {
        if (method == "notifications/initialized" && initialized_)
            ready_ = true;
        return std::nullopt;
    }
    const Json id = message.at("id");
    if (!(id.is_string() || id.is_number_integer()))
        return rpc_error(nullptr, -32600, "Request ID must be a string or integer");
    const auto params = message.value("params", Json::object());
    if (!params.is_object())
        return rpc_error(id, -32602, "Invalid params");
    auto result = [&](Json value) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(value)}};
    };
    if (method == "ping")
        return result(Json::object());
    if (method == "initialize") {
        if (initialized_)
            return rpc_error(id, -32600, "Session already initialized");
        if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string())
            return rpc_error(id, -32602, "protocolVersion is required");
        initialized_ = true;
        return result(
            {{"protocolVersion", "2025-06-18"},
             {"serverInfo", {{"name", "faset-editor"}, {"version", FASET_VERSION}}},
             {"capabilities", {{"tools", Json::object()}, {"resources", Json::object()}}},
             {"instructions",
              "Faset tools edit project documents and manage editor jobs. Query revisions before "
              "writes. Player/runtime world access is intentionally unavailable."}});
    }
    if (!ready_)
        return rpc_error(id, -32002, "Initialize the session before using editor tools");
    if (method == "tools/list")
        return result({{"tools", commands_.list()}});
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string())
            return rpc_error(id, -32602, "Tool name is required");
        try {
            auto value =
                commands_.call(params.at("name"), params.value("arguments", Json::object()));
            Json content = Json::array();
            if (value.is_object() && value.contains("image_base64")) {
                content.push_back(
                    {{"type", "image"},
                     {"data", value.at("image_base64")},
                     {"mimeType", value.value("mimeType", std::string("image/png"))}});
                value.erase("image_base64");
            }
            content.push_back({{"type", "text"}, {"text", value.dump()}});
            return result({{"content", content}, {"structuredContent", value}, {"isError", false}});
        } catch (const Error& error) {
            const auto value = Json{{"error", error.json()}};
            return result({{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})},
                           {"structuredContent", value},
                           {"isError", true}});
        } catch (const std::exception& error) {
            const auto value =
                Json{{"error", {{"code", "editor.failure"}, {"message", error.what()}}}};
            return result({{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})},
                           {"structuredContent", value},
                           {"isError", true}});
        }
    }
    if (method == "resources/list")
        return result({{"resources", Json::array({{{"uri", "faset://schema"},
                                                   {"name", "Component schema"},
                                                   {"mimeType", "application/json"}},
                                                  {{"uri", "faset://documents"},
                                                   {"name", "Open authoring documents"},
                                                   {"mimeType", "application/json"}}})}});
    if (method == "resources/read") {
        if (!params.contains("uri") || !params["uri"].is_string())
            return rpc_error(id, -32602, "Resource URI must be a string");
        const auto uri = params.at("uri").get<std::string>();
        Json value;
        if (uri == "faset://schema")
            value = commands_.authoring().schemas().manifest();
        else if (uri == "faset://documents")
            value = commands_.authoring().documents();
        else
            return rpc_error(id, -32602, "Unknown resource URI");
        return result({{"contents", Json::array({{{"uri", uri},
                                                  {"mimeType", "application/json"},
                                                  {"text", value.dump()}}})}});
    }
    return rpc_error(id, -32601, "Method not found");
}
std::vector<std::string> StdioTransport::poll() {
    std::vector<std::string> lines;
    if (closed_)
        return lines;
    char bytes[65536];
    std::size_t count = 0;
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto type = GetFileType(input);
    DWORD available = 0;
    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            closed_ = true;
        }
    } else if (type == FILE_TYPE_DISK)
        available = sizeof(bytes);
    else
        return lines;
    if (available) {
        DWORD read = 0;
        if (!ReadFile(input, bytes, std::min<DWORD>(available, sizeof(bytes)), &read, nullptr) ||
            read == 0)
            closed_ = true;
        count = read;
    }
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    if (::poll(&input, 1, 0) > 0 && (input.revents & (POLLIN | POLLHUP))) {
        const auto read = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (read > 0)
            count = static_cast<std::size_t>(read);
        else if (read == 0)
            closed_ = true;
        else if (errno != EINTR && errno != EAGAIN)
            closed_ = true;
    }
#endif
    buffer_.append(bytes, count);
    require(buffer_.size() <= 8 * 1024 * 1024, "mcp.message_size", "MCP input exceeds 8 MiB");
    std::size_t newline = 0;
    while ((newline = buffer_.find('\n')) != std::string::npos) {
        auto line = buffer_.substr(0, newline);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
        buffer_.erase(0, newline + 1);
    }
    if (closed_ && !buffer_.empty()) {
        lines.push_back(std::move(buffer_));
        buffer_.clear();
    }
    return lines;
}
void StdioTransport::send(const Json& value) {
    const auto bytes = value.dump() + '\n';
    std::size_t offset = 0;
#ifdef _WIN32
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    while (offset < bytes.size()) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 65536));
        if (!WriteFile(output, bytes.data() + offset, count, &written, nullptr) || written == 0) {
            closed_ = true;
            return;
        }
        offset += written;
    }
#else
    // A disconnected MCP client must not terminate the GUI with SIGPIPE. Block
    // it only on this thread during the write, preserving the process signal
    // policy and any SIGPIPE that was already pending for the caller.
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
        closed_ = true;
        return;
    }
    sigpending(&pending);
    const bool alreadyPending = sigismember(&pending, SIGPIPE) == 1;
    bool brokenPipe = false;
    while (offset < bytes.size()) {
        const auto written = ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (written > 0)
            offset += static_cast<std::size_t>(written);
        else if (written < 0 && errno == EINTR)
            continue;
        else {
            brokenPipe = written < 0 && errno == EPIPE;
            closed_ = true;
            break;
        }
    }
    if (brokenPipe && !alreadyPending) {
        const timespec noWait{};
        while (sigtimedwait(&blocked, nullptr, &noWait) == -1 && errno == EINTR) {
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
}
} // namespace faset::editor
