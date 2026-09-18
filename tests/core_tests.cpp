#include <chrono>
#include <faset/core/error.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <iostream>
#include <set>
#include <thread>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Check failed: " #x);                                         \
    } while (false)
int test_main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--detached-child") {
        faset::Json arguments = faset::Json::array();
        for (int i = 3; i < argc; ++i)
            arguments.push_back(argv[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        faset::atomic_write_json(
            faset::path_from_utf8(argv[2]),
            {{"args", arguments}, {"cwd", faset::path_to_utf8(std::filesystem::current_path())}});
        return 0;
    }
    const auto directory =
        std::filesystem::temp_directory_path() / ("faset-core-" + faset::new_id());
    try {
        CHECK(faset::sha256("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(faset::sha256("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(faset::sha256(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        std::set<std::string> ids;
        for (int i = 0; i < 1000; ++i) {
            const auto id = faset::new_id();
            CHECK(id.size() == 36);
            CHECK(id[14] == '4');
            CHECK(ids.insert(id).second);
        }
        faset::atomic_write(directory / "state.json", "{\"value\":1}");
        faset::atomic_write_json(directory / "state.json", {{"value", 2}, {"text", "Привет 世界"}});
        CHECK(faset::read_json(directory / "state.json").at("value") == 2);
        CHECK(faset::sha256_file(directory / "state.json") ==
              faset::sha256(faset::read_text(directory / "state.json")));
        CHECK(std::filesystem::equivalent(faset::project_path(directory, "assets/../state.json"),
                                          directory / "state.json"));
        const auto unicode = directory / faset::path_from_utf8("Faset Café 世界/Сцена 😀.json");
        faset::atomic_write_json(unicode, {{"hello", "Unicode paths"}});
        faset::atomic_write_json(unicode, {{"hello", "Replaced"}});
        CHECK(faset::read_json(unicode).at("hello") == "Replaced");
        CHECK(faset::sha256_file(unicode) == faset::sha256(faset::read_text(unicode)));
        CHECK(faset::path_from_utf8(faset::path_to_utf8(unicode)) == unicode);
        CHECK(faset::generic_path_to_utf8(unicode.lexically_relative(directory)) ==
              "Faset Café 世界/Сцена 😀.json");
        CHECK(std::filesystem::equivalent(
            faset::project_path(directory, faset::path_from_utf8("Faset Café 世界/Сцена 😀.json")),
            unicode));
        for (const auto& invalid :
             std::vector<std::string>{std::string("a\0b", 3), "\xc0\xaf", "\xed\xa0\x80",
                                      "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) {
            bool invalid_rejected = false;
            try {
                faset::path_from_utf8(invalid);
            } catch (const faset::Error& error) {
                invalid_rejected = error.json().at("code") == "path.encoding";
            }
            CHECK(invalid_rejected);
        }
        auto deep = directory / faset::path_from_utf8("Long Café 世界");
        while (deep.native().size() < 310)
            deep /= "directory-segment-0123456789";
        const auto long_file = deep / faset::path_from_utf8("Сцена 世界.json");
        faset::atomic_write_json(long_file, {{"long_path", true}});
        CHECK(faset::read_json(long_file).at("long_path") == true);
        CHECK(faset::sha256_file(long_file) == faset::sha256(faset::read_text(long_file)));
        CHECK(std::filesystem::is_regular_file(faset::native_io_path(long_file)));
        CHECK(faset::path_to_utf8(long_file).find("\\\\?\\") == std::string::npos);
#ifdef _WIN32
        CHECK(faset::native_io_path(long_file).native().starts_with(LR"(\\?\)"));
        CHECK(faset::native_io_path(std::filesystem::path(LR"(\\server\share\file.txt)")) ==
              std::filesystem::path(LR"(\\?\UNC\server\share\file.txt)"));
        CHECK(faset::native_io_path(faset::native_io_path(long_file)) ==
              faset::native_io_path(long_file));
#endif
        bool rejected = false;
        try {
            faset::project_path(directory, "../escape");
        } catch (const faset::Error&) {
            rejected = true;
        }
        CHECK(rejected);
        rejected = false;
        try {
            faset::project_path(directory, directory / "state.json");
        } catch (const faset::Error&) {
            rejected = true;
        }
        CHECK(rejected);
        const auto external_result = directory / faset::path_from_utf8("Editor 世界.json");
        const std::vector<std::string> arguments = {
            faset::path_to_utf8(std::filesystem::absolute(faset::path_from_utf8(argv[0]))),
            "--detached-child",
            faset::path_to_utf8(external_result),
            "space argument",
            "quote\"backslash\\",
            "$(touch not-executed); & |",
            "",
            "Café 世界 Привет 😀"};
        faset::launch_detached(arguments, directory);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!std::filesystem::exists(external_result) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(std::filesystem::is_regular_file(external_result));
        const auto external = faset::read_json(external_result);
        CHECK(external.at("args") ==
              faset::Json::array({"space argument", "quote\"backslash\\",
                                  "$(touch not-executed); & |", "", "Café 世界 Привет 😀"}));
        CHECK(std::filesystem::equivalent(
            faset::path_from_utf8(external.at("cwd").get<std::string>()), directory));
        CHECK(!std::filesystem::exists(directory / "not-executed"));
        rejected = false;
        try {
            faset::launch_detached({"faset-nonexistent-editor-" + faset::new_id()}, directory);
        } catch (const std::exception&) {
            rejected = true;
        }
        CHECK(rejected);
        std::filesystem::remove_all(faset::native_io_path(directory));
        std::cout << "Core: SHA-256 vectors, persistent IDs, durable replace, Unicode, path "
                     "boundaries and detached literal-argv launch passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(faset::native_io_path(directory));
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, test_main);
}
#else
int main(int argc, char** argv) {
    return test_main(argc, argv);
}
#endif
