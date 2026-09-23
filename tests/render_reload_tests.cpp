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
    Process process({{FASET_PYTHON_EXECUTABLE, FASET_SHADER_COMPILE_TOOL, "--compiler",
                      FASET_TEST_SLANGC, "--source", path_to_utf8(source), "--entry",
                      "fragmentMain", "--output", path_to_utf8(output)},
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
    const auto temporary =
        fs::temp_directory_path() / path_from_utf8("Faset shader Café 世界 " + new_id());
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(native_io_path(path), ignored);
        }
    } cleanup{temporary};
    try {
        const auto bundle = temporary / "shaders";
        fs::create_directories(bundle);
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain",
                                  "gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                  "gpuHzbMain", "gpuPostCullMain"})
            for (const auto* extension : {".spv", ".reflection.json"}) {
                const auto name = std::string(entry) + extension;
                fs::copy_file(path_from_utf8(FASET_TEST_SHADER_DIRECTORY) / name, bundle / name);
            }
        const auto source = temporary / "reload.slang";
        const auto original_source = read_text(path_from_utf8(FASET_TEST_SHADER_SOURCE));
        const auto original_spirv = read_text(bundle / "fragmentMain.spv");
        const auto original_reflection = read_text(bundle / "fragmentMain.reflection.json");
        const auto original_fingerprint = Json::parse(original_reflection).at("layout_fingerprint");
        render::validate_shader_bundle(bundle);
        auto bad_lighting_stride = Json::parse(original_reflection);
        auto& lighting_descriptors = bad_lighting_stride["layout"]["descriptors"];
        bool found_local_buffer = false;
        for (auto& descriptor : lighting_descriptors)
            if (descriptor["set"] == 1 && descriptor["binding"] == 1) {
                descriptor["element_stride"] = 96;
                found_local_buffer = true;
            }
        require(found_local_buffer, "Lighting stride fixture exists");
        bad_lighting_stride["layout_fingerprint"] =
            sha256(bad_lighting_stride["layout"].dump());
        atomic_write_json(bundle / "fragmentMain.reflection.json", bad_lighting_stride);
        bool rejected_lighting_stride = false;
        try {
            render::validate_shader_bundle(bundle);
        } catch (const std::exception&) {
            rejected_lighting_stride = true;
        }
        require(rejected_lighting_stride,
                "Rehashed incompatible local-light element stride must be rejected");
        atomic_write(bundle / "fragmentMain.reflection.json", original_reflection);
        render::RendererConfig configuration;
        configuration.width = configuration.height = 64;
        configuration.headless = true;
        configuration.validation = true;
        configuration.shader_directory = bundle;
        render::Renderer renderer(configuration);
        const auto baseline_only = temporary / "baseline-only";
        fs::create_directories(baseline_only);
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain"})
            for (const auto* extension : {".spv", ".reflection.json"}) {
                const auto name = std::string(entry) + extension;
                fs::copy_file(bundle / name, baseline_only / name);
            }
        auto direct_only_configuration = configuration;
        direct_only_configuration.shader_directory = baseline_only;
        render::Renderer direct_only(direct_only_configuration);
        direct_only.render(render::Snapshot{});
        require(direct_only.stats().validation_errors == 0,
                "Direct renderer starts without optional GPU shader bundle");
        bool unavailable_gpu_bundle = false;
        try {
            direct_only.set_visibility_mode(render::VisibilityMode::GpuFrustum);
        } catch (const std::exception&) {
            unavailable_gpu_bundle = true;
        }
        require(unavailable_gpu_bundle,
                "Switching to GPU visibility reports missing optional shader bundle");
        direct_only.render(render::Snapshot{});
        require(direct_only.visibility_mode() == render::VisibilityMode::Direct &&
                    direct_only.stats().validation_errors == 0,
                "Failed GPU initialization preserves the direct renderer");
        auto gpu_configuration = configuration;
        gpu_configuration.visibility_mode = render::VisibilityMode::GpuFrustum;
        render::Renderer gpu_renderer(gpu_configuration);
        render::Snapshot opaque_scene;
        opaque_scene.eye = {0, 0, 5};
        opaque_scene.view_projection = render::multiply(
            render::perspective(.8f, 1.f, .1f, 20.f),
            render::look_at(opaque_scene.eye, {0, 0, 0}));
        render::DrawItem opaque_cube;
        opaque_cube.mesh = render::cube_mesh();
        opaque_cube.instance_key = "shader-reload-cube";
        opaque_cube.cast_shadow = false;
        opaque_scene.draws.push_back(opaque_cube);
        gpu_renderer.render(opaque_scene);
        const auto gpu_expected = gpu_renderer.pixels();
        render::Snapshot scene;
        scene.ui_quads.push_back({0, 0, 32, 64, {1, .8f, .4f, 1}});
        scene.sprites.push_back({{.5f, 0, .5f}, {1, 2}, {.2f, 1, .4f, 1}});
        renderer.render(scene);
        auto expected = renderer.pixels();
        // Cooked/exported generations can exceed MAX_PATH even for modest project
        // names. Exercise the renderer's file boundaries without relying on the
        // external shader compiler's own long-path policy.
        const auto deep_bundle = temporary / std::string(96, 'a') / std::string(96, 'b') /
                                 std::string(96, 'c') / "shaders";
        require(deep_bundle.native().size() > 300,
                "Shader file fixture must exceed the legacy Windows path limit");
        fs::create_directories(native_io_path(deep_bundle));
        for (const auto* entry : {"vertexMain", "fragmentMain", "shadowMain",
                                  "gpuVertexMain", "gpuShadowMain", "gpuCullMain",
                                  "gpuHzbMain", "gpuPostCullMain"})
            for (const auto* extension : {".spv", ".reflection.json"}) {
                const auto name = std::string(entry) + extension;
                atomic_write(deep_bundle / name, read_text(bundle / name));
            }
        render::validate_shader_bundle(deep_bundle);
        {
            auto deep_configuration = configuration;
            deep_configuration.shader_directory = deep_bundle;
            render::Renderer deep_renderer(deep_configuration);
            deep_renderer.render(scene);
            require(deep_renderer.pixels() == expected,
                    "Deep Unicode shader bundle must render the same pixels");
            const auto capture = deep_bundle / path_from_utf8("Capture Café 世界.ppm");
            deep_renderer.capture(capture);
            const auto bytes = read_text(capture);
            require(bytes.starts_with("P6\n64 64\n255\n") && bytes.size() == 13 + 64 * 64 * 3,
                    "Deep Unicode capture must contain the complete rendered image");
            require(deep_renderer.stats().validation_errors == 0,
                    "Deep shader bundle has no Vulkan validation errors");
        }
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
        auto incompatible = original_source;
        auto at = incompatible.find("    float4 reserved;");
        require(at != std::string::npos, "Local-light stride fixture exists");
        incompatible.replace(at, std::string("    float4 reserved;").size(),
                             "    float4 reserved;\n    float4 incompatibleExtraLane;");
        atomic_write(source, incompatible);
        require(compile(source, bundle) == 0, "Compile incompatible light-buffer stride");
        require(read_json(bundle / "fragmentMain.reflection.json").at("layout_fingerprint") !=
                    original_fingerprint,
                "Lighting stride edit changes normalized layout fingerprint");
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
        incompatible = original_source;
        at = incompatible.find("[[vk::binding(2,0)]]");
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
        require(gpu_renderer.reload_shaders(error),
                "Compatible fragment edit reloads GPU scene pipeline");
        gpu_renderer.render(opaque_scene);
        const auto gpu_changed = gpu_renderer.pixels();
        require(gpu_changed != gpu_expected,
                "Compatible fragment reload must change GPU opaque pixels");
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
