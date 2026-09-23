#include <faset/core/io.hpp>
#include <faset/editor/build_cache.hpp>
#include <faset/editor/build_service.hpp>
#include <faset/scripting/project.hpp>
#include <iostream>

using namespace faset;
namespace fs = std::filesystem;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
void run(const fs::path& root) {
    const auto project = root / "project";
    const auto scripts = project / "Scripts";
    atomic_write(scripts / "Gameplay.cpp", "#include \"Gameplay.hpp\"\n");
    atomic_write(scripts / "Gameplay.hpp", "#pragma once\n");
    atomic_write(scripts / "Extensions/Extra.hpp", "#define SPEED 1\n");
    const auto compiler = root / "fake-compiler";
    const auto cmake = root / "fake-cmake";
    const auto slang = root / "fake-slangc";
    atomic_write(compiler, "compiler-v1\n");
    atomic_write(cmake, "cmake-v1\n");
    atomic_write(slang, "slang-v1\n");
    editor::BuildConfig config;
    config.project_root = project;
    config.engine_root = path_from_utf8(FASET_ENGINE_SOURCE);
    config.build_directory = project / ".faset/build";
    config.cmake = path_to_utf8(cmake);
    config.configure_arguments = {
        "-DCMAKE_C_COMPILER=" + path_to_utf8(compiler),
        "-DCMAKE_CXX_COMPILER=" + path_to_utf8(compiler),
        "-DSLANGC_EXECUTABLE=" + path_to_utf8(slang)};

    const scripting::LuaProject none;
    const auto initial = editor::capture_build_inputs(config, none);
    atomic_write(scripts / "Extensions/Extra.hpp", "#define SPEED 2\n");
    const auto changed_header = editor::capture_build_inputs(config, none);
    check(initial.source_hash != changed_header.source_hash &&
              initial.fingerprint() != changed_header.fingerprint(),
          "Nested gameplay header changes the complete source snapshot");

    atomic_write(scripts / "main.lua", "return {}\n");
    atomic_write_json(project / "project.faset.json",
                      {{"format", "faset.project"},
                       {"version", 1},
                       {"scripting", {{"lua", {{"scripts", {"Scripts/main.lua"}}}}}}});
    auto lua = scripting::loadLuaProject(project);
    const auto with_lua = editor::capture_build_inputs(config, lua);
    atomic_write(scripts / "other.lua", "return {}\n");
    auto manifest = read_json(project / "project.faset.json");
    manifest["scripting"]["lua"]["scripts"].push_back("Scripts/other.lua");
    atomic_write_json(project / "project.faset.json", manifest);
    lua = scripting::loadLuaProject(project);
    const auto changed_declaration = editor::capture_build_inputs(config, lua);
    check(with_lua.source_hash != changed_declaration.source_hash,
          "Lua entry declaration changes the source snapshot");

    const auto native = config.build_directory / "Debug";
    editor::ensure_native_toolchain_stamp(config, native, changed_declaration);
    atomic_write(native / "sentinel", "keep\n");
    editor::ensure_native_toolchain_stamp(config, native, changed_declaration);
    check(fs::exists(native / "sentinel"), "Unchanged toolchain preserves the native tree");
    atomic_write(compiler, "compiler-v2\n");
    const auto changed_tool = editor::capture_build_inputs(config, lua);
    check(changed_tool.source_hash == changed_declaration.source_hash &&
              changed_tool.toolchain_hash != changed_declaration.toolchain_hash,
          "Changing compiler bytes at the same path changes toolchain identity");
    editor::ensure_native_toolchain_stamp(config, native, changed_tool);
    check(!fs::exists(native / "sentinel"),
          "Changed toolchain invalidates only the generated native tree");

    const auto user_directory = root / "important-user-data";
    atomic_write(user_directory / "do-not-delete.txt", "valuable\n");
    bool refused_unowned{};
    try {
        editor::ensure_native_toolchain_stamp(config, user_directory, changed_tool);
    } catch (const std::exception&) {
        refused_unowned = true;
    }
    check(refused_unowned && fs::is_regular_file(user_directory / "do-not-delete.txt"),
          "Unowned native directory is never cleared");
    std::error_code native_link_error;
    fs::create_directory_symlink(user_directory, config.build_directory / "Release",
                                 native_link_error);
    if (!native_link_error) {
        bool refused_link{};
        try {
            editor::ensure_native_toolchain_stamp(config, config.build_directory / "Release",
                                                  changed_tool);
        } catch (const std::exception&) {
            refused_link = true;
        }
        check(refused_link && fs::is_regular_file(user_directory / "do-not-delete.txt"),
              "Symlinked native directory cannot redirect cleanup into user data");
    }

    const auto relative_tool = project / "tools/cmake-fixture";
    atomic_write(relative_tool, "relative-cmake-v1\n");
    auto relative_config = config;
    relative_config.cmake = "tools/cmake-fixture";
    const auto relative_before = editor::capture_build_inputs(relative_config, lua);
    atomic_write(relative_tool, "relative-cmake-v2\n");
    check(relative_before.toolchain_hash !=
              editor::capture_build_inputs(relative_config, lua).toolchain_hash,
          "Relative CMake executable resolves from the project working directory");
    auto typed_config = config;
    const auto typed_compiler = root / "typed-compiler";
    atomic_write(typed_compiler, "typed-v1\n");
    typed_config.configure_arguments = {
        "-DCMAKE_C_COMPILER:FILEPATH=" + path_to_utf8(typed_compiler)};
    const auto typed_before = editor::capture_build_inputs(typed_config, lua);
    atomic_write(typed_compiler, "typed-v2\n");
    check(typed_before.toolchain_hash !=
              editor::capture_build_inputs(typed_config, lua).toolchain_hash,
          "Typed CMake compiler overrides are included in toolchain identity");
    auto malicious_config = config;
    malicious_config.configure_arguments.push_back(
        "-DFASET_GAMEPLAY_SOURCE_DIR:PATH=" + path_to_utf8(root / "outside"));
    bool refused_source_override{};
    try {
        (void)editor::capture_build_inputs(malicious_config, lua);
    } catch (const std::exception&) {
        refused_source_override = true;
    }
    check(refused_source_override,
          "Configure arguments cannot redirect gameplay compilation away from hashed Scripts");

    const auto staged = editor::stage_gameplay_sources(config, changed_tool, lua,
                                                        "snapshot-fixture");
    check(staged != scripts && fs::is_regular_file(staged / "Gameplay.cpp") &&
              read_text(staged / "Extensions/Extra.hpp") == "#define SPEED 2\n",
          "Native gameplay compilation uses a complete staged Scripts snapshot");
    atomic_write(scripts / "Extensions/Extra.hpp", "#define SPEED 999\n");
    check(read_text(staged / "Extensions/Extra.hpp") == "#define SPEED 2\n",
          "A live edit cannot alter the source bytes of an in-flight native build");
    atomic_write(scripts / "Extensions/Extra.hpp", "#define SPEED 2\n");
    check(editor::stage_gameplay_sources(config, changed_tool, lua, "snapshot-again") ==
              staged,
          "Identical sources retain a stable native path for incremental Ninja builds");

    atomic_write(native / "faset_player", "player-v1\n");
    atomic_write(native / "faset_schema_exporter", "exporter-v1\n");
    atomic_write(native / "CMakeCache.txt", "recipe-v1\n");
    atomic_write(native / "shaders/vertexMain.spv", "shader-v1\n");
    atomic_write(native / "faset_runtime.dll", "runtime-v1\n");
    auto package_key = [&] (const editor::BuildInputs& inputs) {
        return editor::build_package_key(inputs, native, "Debug", native / "faset_player",
                                         native / "faset_schema_exporter");
    };
    const auto first_key = package_key(changed_tool);
    check(first_key != package_key(changed_declaration) &&
              first_key != package_key(with_lua),
          "Toolchain and Lua source changes invalidate package identity");
    auto option_config = config;
    option_config.configure_arguments.push_back("-DFASET_TEST_OPTION=ON");
    check(first_key != package_key(editor::capture_build_inputs(option_config, lua)),
          "Configure option changes invalidate package identity");
    atomic_write(native / "shaders/vertexMain.spv", "shader-v2\n");
    check(first_key != package_key(changed_tool),
          "Shader bytes invalidate package identity");
    atomic_write(native / "shaders/vertexMain.spv", "shader-v1\n");
    atomic_write(native / "faset_runtime.dll", "runtime-v2\n");
    check(first_key != package_key(changed_tool),
          "Runtime DLL bytes invalidate package identity");

    fs::create_directories(root / "outside");
    std::error_code link_error;
    fs::create_directory_symlink(root / "outside", scripts / "linked", link_error);
    if (!link_error) {
        bool rejected = false;
        try {
            (void)editor::capture_build_inputs(config, lua);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Source snapshot refuses a symlink escaping Scripts");
    }
}
} // namespace

int main() {
    const auto root = fs::temp_directory_path() / path_from_utf8("Faset build cache Café 世界 " + new_id());
    try {
        run(root);
        fs::remove_all(root);
        std::cout << "Source, toolchain and native package identity contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
