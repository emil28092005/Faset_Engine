#include "shader_contract.hpp"
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Function> void must_reject(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
} // namespace

int main() {
    const auto original = fs::path(FASET_TEST_SHADER_DIRECTORY);
    const auto temporary = fs::temp_directory_path() /
        faset::path_from_utf8("Faset temporal shaders " + faset::new_id());
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(faset::native_io_path(path), error); }
    } cleanup{temporary};
    fs::create_directories(faset::native_io_path(temporary));
    constexpr const char* entries[] = {"temporalResolveMain", "temporalCompositeVertexMain",
                                       "temporalCompositeFragmentMain"};
    for (const auto* entry : entries)
        for (const auto* extension : {".spv", ".reflection.json"}) {
            const auto name = std::string(entry) + extension;
            fs::copy_file(faset::native_io_path(original / name),
                          faset::native_io_path(temporary / name));
        }

    const auto bundle = faset::render::detail::load_temporal_shader_bundle(temporary);
    for (const auto& shader : bundle)
        require(!shader.words.empty() && !shader.layout_fingerprint.empty(),
                "Every temporal shader entry has valid checked SPIR-V and reflection");

    auto reflection = temporary / "temporalResolveMain.reflection.json";
    auto metadata = faset::read_json(reflection);
    require(metadata["layout"]["stage"] == "compute" &&
                metadata["layout"]["descriptors"].size() == 7 &&
                metadata["layout"]["push_constants"][0]["size"] == 64,
            "Temporal resolve ABI contains seven images and a 64-byte push block");
    metadata["layout"]["descriptors"][5]["binding"] = 8;
    metadata["layout_fingerprint"] = faset::sha256(metadata["layout"].dump());
    faset::atomic_write_json(reflection, metadata);
    must_reject([&] { (void)faset::render::detail::load_temporal_shader_bundle(temporary); },
                "A rehashed temporal image binding change must be rejected");
    faset::atomic_write_json(
        reflection, faset::read_json(original / "temporalResolveMain.reflection.json"));

    auto fragment = temporary / "temporalCompositeFragmentMain.spv";
    const auto bytes = faset::read_text(fragment);
    faset::atomic_write(fragment, "corrupt");
    must_reject([&] { (void)faset::render::detail::load_temporal_shader_bundle(temporary); },
                "A broken composite shader cannot enter the temporal bundle");
    faset::atomic_write(fragment, bytes);
    fs::remove(faset::native_io_path(temporary / "temporalCompositeVertexMain.spv"));
    must_reject([&] { (void)faset::render::detail::load_temporal_shader_bundle(temporary); },
                "A missing temporal entry cannot enter a complete shader bundle");
}
