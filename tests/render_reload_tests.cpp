#include <chrono>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/core/process.hpp>
#include <faset/render/renderer.hpp>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace faset;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
int compile(const fs::path& source, const fs::path& output) {
    Process process(
        {{FASET_PYTHON_EXECUTABLE, FASET_SHADER_COMPILE_TOOL, "--compiler", FASET_TEST_SLANGC,
          "--source", source.string(), "--entry", "fragmentMain", "--output", output.string()},
         {},
         {}});
    std::string diagnostics;
    while (true) {
        auto result = process.poll();
        diagnostics += result.output;
        if (!result.running) {
            if (result.exit_code != 0)
                require(diagnostics.find("intentional_shader_compile_failure") != std::string::npos,
                        "Slang failure must preserve its useful compiler diagnostic");
            return result.exit_code.value_or(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace
int main() {
    const auto temporary = fs::temp_directory_path() / ("faset-shader-reload-" + new_id());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{temporary};
    try {
        const auto bundle = temporary / "shaders";
        fs::create_directories(bundle);
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain"})
            for (const auto* extension : {".spv", ".reflection.json"}) {
                const auto name = std::string(entry) + extension;
                fs::copy_file(fs::path(FASET_TEST_SHADER_DIRECTORY) / name, bundle / name);
            }
        const auto source = temporary / "reload.slang";
        const auto original_source = read_text(FASET_TEST_SHADER_SOURCE);
        const auto original_spirv = read_text(bundle / "fragmentMain.spv");
        const auto original_reflection = read_text(bundle / "fragmentMain.reflection.json");
        const auto original_fingerprint = Json::parse(original_reflection).at("layout_fingerprint");
        render::validate_shader_bundle(bundle);
        render::RendererConfig configuration;
        configuration.width = configuration.height = 64;
        configuration.headless = true;
        configuration.validation = true;
        configuration.shader_directory = bundle;
        render::Renderer renderer(configuration);
        render::Snapshot scene;
        scene.ui_quads.push_back({0, 0, 64, 64, {1, .8f, .4f, 1}});
        renderer.render(scene);
        auto expected = renderer.pixels();
        require(renderer.stats().gpu_allocated_bytes > 1024 * 1024 &&
                    renderer.stats().texture_count >= 1,
                "Frame profile reports live Vulkan allocations and textures");
        auto restore = [&] {
            atomic_write(bundle / "fragmentMain.spv", original_spirv);
            atomic_write(bundle / "fragmentMain.reflection.json", original_reflection);
        };
        auto retained = [&] {
            std::string error;
            require(!renderer.reload_shaders(error) && !error.empty(),
                    "Unsafe shader reload must fail with a diagnostic");
            renderer.render(scene);
            require(renderer.pixels() == expected,
                    "Rejected shader reload must retain working pixels");
            require(renderer.stats().validation_errors == 0,
                    "Rejected bytecode must not reach Vulkan validation");
        };
        atomic_write(bundle / "fragmentMain.spv", "damaged bytecode");
        retained();
        restore();
        auto malformed = original_spirv;
        for (int i = 0; i < 4; ++i)
            malformed[20 + i] = 0; // zero-word SPIR-V instruction
        auto metadata = Json::parse(original_reflection);
        metadata["spirv_sha256"] = sha256(malformed);
        atomic_write(bundle / "fragmentMain.spv", malformed);
        atomic_write_json(bundle / "fragmentMain.reflection.json", metadata);
        retained();
        restore();
        auto incompatible = original_source;
        auto at = incompatible.find("[[vk::binding(2,0)]]");
        require(at != std::string::npos, "Shader descriptor fixture exists");
        incompatible.replace(at, std::string("[[vk::binding(2,0)]]").size(),
                             "[[vk::binding(7,0)]]");
        atomic_write(source, incompatible);
        require(compile(source, bundle) == 0, "Compile real incompatible descriptor layout");
        require(read_json(bundle / "fragmentMain.reflection.json").at("layout_fingerprint") !=
                    original_fingerprint,
                "Descriptor edit changes normalized layout fingerprint");
        retained();
        restore();
        incompatible = original_source;
        at = incompatible.find("column_major float4x4");
        require(at != std::string::npos, "Shader matrix fixture exists");
        incompatible.replace(at, std::string("column_major float4x4").size(), "row_major float4x4");
        atomic_write(source, incompatible);
        require(compile(source, bundle) == 0, "Compile real incompatible matrix storage");
        retained();
        restore();
        auto compatible = original_source;
        at = compatible.find("    float4 sampled =");
        require(at != std::string::npos, "Shader fragment fixture exists");
        compatible.insert(at, "    v.color.rgb *= 0.5;\n");
        atomic_write(source, compatible);
        require(compile(source, bundle) == 0, "Compile real compatible shader edit");
        require(read_json(bundle / "fragmentMain.reflection.json").at("layout_fingerprint") ==
                    original_fingerprint,
                "Source-only behavior edit preserves normalized layout fingerprint");
        std::string error;
        require(renderer.reload_shaders(error), "Compatible shader edit reloads successfully");
        renderer.render(scene);
        const auto changed = renderer.pixels();
        require(changed[0] + 50 < expected[0], "Compatible reload changes actual rendered pixels");
        expected = changed;
        const auto last_spirv = read_text(bundle / "fragmentMain.spv");
        const auto last_reflection = read_text(bundle / "fragmentMain.reflection.json");
        atomic_write(source, compatible + "\n#error intentional_shader_compile_failure\n");
        require(compile(source, bundle) != 0, "Real Slang compile failure is reported");
        require(read_text(bundle / "fragmentMain.spv") == last_spirv &&
                    read_text(bundle / "fragmentMain.reflection.json") == last_reflection,
                "Compile failure preserves both published shader artifacts");
        require(renderer.reload_shaders(error),
                "Last successfully compiled artifacts remain reloadable");
        renderer.render(scene);
        require(renderer.pixels() == expected, "Compile failure preserves last good rendering");
        require(renderer.stats().validation_errors == 0,
                "Shader reload regression has no Vulkan validation errors");
        std::cout << "Normalized reflection, malformed SPIR-V, incompatible descriptors/matrices, "
                     "real compile failure and compatible pixel reload passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
