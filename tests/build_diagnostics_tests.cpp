#include <faset/editor/build_diagnostics.hpp>
#include <faset/core/io.hpp>
#include <iostream>
#include <stdexcept>

using namespace faset;
namespace fs = std::filesystem;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
void contracts() {
    const auto project = fs::path("/project");
    auto rows = editor::parse_build_diagnostics(
        "/project/Scripts/Game.cpp:17:4: error: bad field\n", "compile", project);
    check(rows.size() == 1 && rows[0].at("file") == "Scripts/Game.cpp" &&
              rows[0].at("line") == 17 && rows[0].at("column") == 4 &&
              rows[0].at("severity") == "error" && rows[0].at("phase") == "compile",
          "Clang error has a project-relative source location");
    rows = editor::parse_build_diagnostics(
        "\x1b[31mScripts/player.lua:6: unexpected symbol near '='\x1b[0m\n",
        "schema", project);
    check(rows.size() == 1 && rows[0].at("file") == "Scripts/player.lua" &&
              rows[0].at("line") == 6 && rows[0].at("severity") == "error" &&
              rows[0].at("message") == "unexpected symbol near '='",
          "Lua syntax error and ANSI stripping work");
    const auto windows = path_from_utf8(R"(C:\Café)");
    rows = editor::parse_build_diagnostics(
        R"(C:\Café\Scripts\Game.cpp(17,4): error C2143: syntax error)" "\n"
        R"(C:\Café\Scripts\Game.cpp:19:2: warning: suspicious conversion)" "\n",
        "compile", windows);
    check(rows.size() == 2 && rows[0].at("file") == "Scripts/Game.cpp" &&
              rows[0].at("code") == "C2143" && rows[0].at("line") == 17 &&
              rows[1].at("severity") == "warning" && rows[1].at("line") == 19,
          "Windows drive colon and Unicode directory remain intact");
    rows = editor::parse_build_diagnostics(
        "Scripts/Game.cpp:9:3: error: missing value\n"
        "    broken call\n"
        "    ^~~~~~\n"
        "Scripts/Game.cpp:4:1: note: declared here\n",
        "compile", project);
    check(rows.size() == 2 && rows[0].at("severity") == "error" &&
              rows[1].at("severity") == "note" && rows[1].at("file") == "Scripts/Game.cpp",
          "Caret and source excerpts do not become duplicate diagnostics");
    rows = editor::parse_build_diagnostics(
        "/outside/Scripts/Game.cpp:4:2: error: external\n"
        "../Scripts/Escape.cpp:5:2: error: escaped\n",
        "compile", project);
    check(rows.size() == 2 && !rows[0].contains("file") && !rows[1].contains("file"),
          "External and traversing sources are never navigable");
    rows = editor::parse_build_diagnostics(
        "Scripts/../Scripts/Game.cpp:4:2: error: disguised traversal\n"
        "Scripts/Game.cpp:4:0: error: invalid column\n",
        "compile", project);
    check(rows.size() == 1 && !rows[0].contains("file"),
          "Traversal stays non-navigable and zero columns are rejected");
    std::string noisy;
    for (int index = 0; index < 300; ++index)
        noisy += "Scripts/Game.cpp:2:1: warning: repeated\n";
    check(editor::parse_build_diagnostics(noisy, "compile", project).size() <= 200,
          "Structured diagnostic count is bounded");
}
} // namespace

int main() {
    try {
        contracts();
        std::cout << "Build diagnostic parsing contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
