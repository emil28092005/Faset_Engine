#include <faset/core/error.hpp>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <iostream>
#include <set>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("Check failed: " #x);                                         \
    } while (false)
int main() {
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
        std::filesystem::remove_all(directory);
        std::cout << "Core: SHA-256 vectors, persistent IDs, durable replace, Unicode, path "
                     "boundaries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
