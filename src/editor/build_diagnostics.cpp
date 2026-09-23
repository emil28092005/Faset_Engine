#include <faset/editor/build_diagnostics.hpp>

#include <faset/core/io.hpp>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace faset::editor {
namespace {
constexpr std::size_t max_rows = 200;
constexpr std::size_t max_line = 8192;
constexpr std::size_t max_message = 2048;

std::string strip_ansi(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
            continue;
        }
        clean += text[index];
    }
    return clean;
}
std::string normalize_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    path = std::filesystem::path(path).lexically_normal().generic_string();
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}
bool has_windows_drive(std::string_view path) {
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
           path[1] == ':';
}
std::string lower_ascii(std::string value) {
    for (auto& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}
std::string project_source(std::string raw, const std::filesystem::path& project_root) {
    if (raw.starts_with("lua: "))
        raw.erase(0, 5);
    std::replace(raw.begin(), raw.end(), '\\', '/');
    for (const auto& component : std::filesystem::path(raw))
        if (component == "..")
            return {};
    const auto path = normalize_path(std::move(raw));
    const auto root = normalize_path(path_to_utf8(project_root));
    std::string relative;
    if (!path.empty() && path[0] != '/' && !has_windows_drive(path))
        relative = path;
    else {
        const auto windows = has_windows_drive(path) && has_windows_drive(root);
        const auto comparable_path = windows ? lower_ascii(path) : path;
        const auto comparable_root = windows ? lower_ascii(root) : root;
        if (comparable_path.size() <= comparable_root.size() ||
            !comparable_path.starts_with(comparable_root) ||
            comparable_path[comparable_root.size()] != '/')
            return {};
        relative = path.substr(root.size() + 1);
    }
    if (!relative.starts_with("Scripts/") || relative.size() <= 8)
        return {};
    if (relative.find("/../") != std::string::npos || relative.ends_with("/.."))
        return {};
    return relative;
}
int positive_number(const std::string& text) {
    try {
        const auto value = std::stoll(text);
        return value > 0 && value <= 100000000 ? static_cast<int>(value) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}
} // namespace

Json parse_build_diagnostics(std::string_view raw_log, std::string_view phase,
                             const std::filesystem::path& project_root) {
    static const std::regex msvc(
        R"(^(.+)\(([0-9]+)(?:,([0-9]+))?\)\s*:\s*(fatal error|error|warning|note)\s*([A-Za-z]+[0-9]+)?\s*:\s*(.*)$)");
    static const std::regex clang_column(
        R"(^(.+):([0-9]+):([0-9]+):\s*(fatal error|error|warning|note)(?:\s+([A-Za-z]+[0-9]+))?\s*:\s*(.*)$)");
    static const std::regex clang_line(
        R"(^(.+):([0-9]+):\s*(fatal error|error|warning|note)(?:\s+([A-Za-z]+[0-9]+))?\s*:\s*(.*)$)");
    static const std::regex lua(R"(^(.+\.lua):([0-9]+):\s*(.*)$)");
    Json rows = Json::array();
    std::istringstream stream{std::string(raw_log)};
    std::string raw;
    while (rows.size() < max_rows && std::getline(stream, raw)) {
        if (raw.size() > max_line)
            raw.resize(max_line);
        auto line = strip_ansi(raw);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch match;
        std::string file, message, severity, code;
        int row = 0, column = 0;
        bool has_column = false;
        if (std::regex_match(line, match, msvc) ||
            std::regex_match(line, match, clang_column)) {
            file = match[1].str();
            row = positive_number(match[2].str());
            has_column = !match[3].str().empty();
            column = positive_number(match[3].str());
            severity = match[4].str();
            code = match[5].str();
            message = match[6].str();
        } else if (std::regex_match(line, match, clang_line)) {
            file = match[1].str();
            row = positive_number(match[2].str());
            severity = match[3].str();
            code = match[4].str();
            message = match[5].str();
        } else if (std::regex_match(line, match, lua)) {
            file = match[1].str();
            row = positive_number(match[2].str());
            severity = "error";
            message = match[3].str();
        } else
            continue;
        if (row == 0 || (has_column && column == 0))
            continue;
        if (severity == "fatal error")
            severity = "error";
        if (message.size() > max_message)
            message.resize(max_message);
        Json diagnostic{{"severity", severity},
                        {"phase", std::string(phase)},
                        {"message", message},
                        {"line", row}};
        if (column > 0)
            diagnostic["column"] = column;
        if (!code.empty())
            diagnostic["code"] = code;
        if (auto relative = project_source(file, project_root); !relative.empty())
            diagnostic["file"] = std::move(relative);
        rows.push_back(std::move(diagnostic));
    }
    return rows;
}
} // namespace faset::editor
