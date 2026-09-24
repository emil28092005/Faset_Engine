#include "shader_contract.hpp"
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void require(bool valid, const char* message) {
    if (!valid)
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
    const auto temporary = fs::temp_directory_path() / "faset-gpu-shader-contract-test";
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{temporary};
    try {
        fs::create_directories(temporary);
        constexpr const char* entries[] = {"gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                           "gpuHzbMain", "gpuPostCullMain"};
        for (const auto* entry : entries)
            for (const auto* extension : {".spv", ".reflection.json"}) {
                const auto file = std::string(entry) + extension;
                fs::copy_file(original / file, temporary / file, fs::copy_options::overwrite_existing);
            }
        auto shaders = faset::render::detail::load_gpu_shader_bundle(temporary);
        for (const auto& shader : shaders)
            require(!shader.words.empty() && !shader.layout_fingerprint.empty(),
                    "Every P2 entry is valid SPIR-V with checked metadata");
        auto shader_file = temporary / "gpuCullMain.spv";
        const auto shader_bytes = faset::read_text(shader_file);
        faset::atomic_write(shader_file, "corrupt");
        must_reject([&] { (void)faset::render::detail::load_gpu_shader_bundle(temporary); },
                    "Corrupt P2 SPIR-V must be rejected");
        faset::atomic_write(shader_file, shader_bytes);
        auto reflection_file = temporary / "gpuPostCullMain.reflection.json";
        auto metadata = faset::read_json(reflection_file);
        metadata["layout_fingerprint"] = "tampered";
        faset::atomic_write_json(reflection_file, metadata);
        must_reject([&] { (void)faset::render::detail::load_gpu_shader_bundle(temporary); },
                    "Tampered P2 layout fingerprint must be rejected");
        metadata = faset::read_json(original / "gpuPostCullMain.reflection.json");
        metadata["layout"]["descriptors"][0]["element_stride"] = 208;
        metadata["layout_fingerprint"] = faset::sha256(metadata["layout"].dump());
        faset::atomic_write_json(reflection_file, metadata);
        must_reject([&] { (void)faset::render::detail::load_gpu_shader_bundle(temporary); },
                    "A consistently rehashed but incompatible GPU record stride must be rejected");
        faset::atomic_write_json(reflection_file,
                                 faset::read_json(original / "gpuPostCullMain.reflection.json"));
        reflection_file = temporary / "gpuVertexMain.reflection.json";
        metadata = faset::read_json(original / "gpuVertexMain.reflection.json");
        metadata["layout"]["descriptors"][0]["set"] = 1;
        metadata["layout_fingerprint"] = faset::sha256(metadata["layout"].dump());
        faset::atomic_write_json(reflection_file, metadata);
        must_reject([&] { (void)faset::render::detail::load_gpu_shader_bundle(temporary); },
                    "GPU graphics scene buffers must stay in descriptor set two");
        fs::remove(temporary / "gpuHzbMain.spv");
        must_reject([&] { (void)faset::render::detail::load_gpu_shader_bundle(temporary); },
                    "Missing P2 entry must be rejected");
        std::cout << "GPU shader bundle validates every entry and rejects corrupt or missing artifacts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
