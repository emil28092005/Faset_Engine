#include <faset/core/io.hpp>
#include <faset/render/renderer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace faset::render;
namespace fs = std::filesystem;

namespace {
struct Options {
    unsigned lights{}, width{1920}, height{1080}, warmup{10}, frames{30}, run_index{};
    bool shadows{}, validation{};
    VisibilityMode visibility{VisibilityMode::Direct};
    std::string commit{"unknown"}, driver{"unknown"};
    fs::path csv, capture;
};

unsigned number(std::string_view text, std::string_view name) {
    std::size_t end{};
    const auto value = std::stoul(std::string(text), &end);
    if (end != text.size() || value > 100000)
        throw std::invalid_argument("Invalid value for " + std::string(name));
    return static_cast<unsigned>(value);
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--list-runs") {
            std::cout << "{\"lights\":[0,4,16,32,64,128],"
                         "\"visibility\":[\"direct\",\"gpu-frustum\",\"gpu-occlusion\"],"
                         "\"shadows\":[\"off\",\"on\"]}\n";
            std::exit(0);
        }
        if (name == "--help") {
            std::cout << "Usage: faset_p3_lighting_benchmark --lights 0|4|16|32|64|128 "
                         "--shadows on|off --visibility direct|gpu-frustum|gpu-occlusion "
                         "--csv PATH [--width N --height N --warmup N --frames N "
                         "--run-index N --commit SHA --driver NAME --validation on|off "
                         "--capture PATH]\n";
            std::exit(0);
        }
        if (i + 1 >= argc)
            throw std::invalid_argument("Missing value for " + name);
        const std::string value = argv[++i];
        if (name == "--lights") options.lights = number(value, name);
        else if (name == "--width") options.width = number(value, name);
        else if (name == "--height") options.height = number(value, name);
        else if (name == "--warmup") options.warmup = number(value, name);
        else if (name == "--frames") options.frames = number(value, name);
        else if (name == "--run-index") options.run_index = number(value, name);
        else if (name == "--commit") options.commit = value;
        else if (name == "--driver") options.driver = value;
        else if (name == "--csv") options.csv = faset::path_from_utf8(value);
        else if (name == "--capture") options.capture = faset::path_from_utf8(value);
        else if (name == "--shadows") {
            if (value != "on" && value != "off")
                throw std::invalid_argument("--shadows must be on or off");
            options.shadows = value == "on";
        } else if (name == "--validation") {
            if (value != "on" && value != "off")
                throw std::invalid_argument("--validation must be on or off");
            options.validation = value == "on";
        } else if (name == "--visibility") {
            if (value == "direct") options.visibility = VisibilityMode::Direct;
            else if (value == "gpu-frustum") options.visibility = VisibilityMode::GpuFrustum;
            else if (value == "gpu-occlusion") options.visibility = VisibilityMode::GpuOcclusion;
            else throw std::invalid_argument("Unknown visibility mode: " + value);
        } else throw std::invalid_argument("Unknown option: " + name);
    }
    constexpr std::array allowed_lights{0u, 4u, 16u, 32u, 64u, 128u};
    if (options.csv.empty() || options.width == 0 || options.height == 0 ||
        options.frames == 0 || options.warmup > 1000 ||
        std::find(allowed_lights.begin(), allowed_lights.end(), options.lights) ==
            allowed_lights.end())
        throw std::invalid_argument("Invalid benchmark configuration");
    return options;
}

const char* mode_name(VisibilityMode mode) {
    switch (mode) {
    case VisibilityMode::Direct: return "direct";
    case VisibilityMode::GpuFrustum: return "gpu-frustum";
    case VisibilityMode::GpuOcclusion: return "gpu-occlusion";
    }
    return "unknown";
}

void csv_text(std::ostream& out, std::string_view value) {
    out << '"';
    for (const char c : value) {
        if (c == '"') out << '"';
        out << c;
    }
    out << '"';
}

Snapshot benchmark_scene(const Options& options) {
    Snapshot scene;
    scene.view_id = "p3-lighting-benchmark-fixed-scene";
    scene.eye = {0, 0, 9};
    scene.projection = perspective(.9f, float(options.width) / float(options.height), .1f, 100);
    const auto view = look_at(scene.eye, {0, 0, 0});
    scene.view_projection = multiply(scene.projection, view);
    scene.camera_frustum = CameraFrustum{view, scene.projection, .1f, 100, true};
    scene.authored_lights_present = true;
    scene.clear_color = {.035f, .04f, .05f, 1};
    DrawItem receiver;
    receiver.mesh = cube_mesh();
    receiver.model = transform({0, 0, -.15f}, {}, {10, 7.5f, .2f});
    receiver.color = {.65f, .67f, .7f, 1};
    receiver.roughness = .65f;
    receiver.cast_shadow = options.shadows;
    receiver.instance_key = "large-receiver";
    scene.draws.push_back(receiver);
    for (int i = 0; i < 9; ++i) {
        DrawItem object;
        object.mesh = cube_mesh();
        object.model = transform({(float(i % 3) - 1.f) * 2.5f,
                                  (float(i / 3) - 1.f) * 1.8f, .45f},
                                 {}, {.42f, .42f, .6f});
        object.color = {.6f + .1f * float(i % 3), .5f, .4f + .1f * float(i / 3), 1};
        object.cast_shadow = options.shadows;
        object.instance_key = "caster-" + std::to_string(i);
        scene.draws.push_back(std::move(object));
    }
    for (unsigned i = 0; i < options.lights; ++i) {
        LocalLight light;
        light.kind = LocalLight::Kind::Point;
        light.stable_id = "benchmark-light-" + std::to_string(i);
        light.position = {(float(i % 8) - 3.5f) * 1.35f,
                          (float((i / 8) % 8) - 3.5f) * .95f,
                          2.f + .35f * float(i % 3)};
        light.color = {.6f + .4f * float(i % 3 == 0),
                       .6f + .4f * float(i % 3 == 1),
                       .6f + .4f * float(i % 3 == 2), 1};
        light.intensity = 5.f;
        light.range = 8.f;
        light.casts_shadow = options.shadows;
        scene.local_lights.push_back(std::move(light));
    }
    return scene;
}

void benchmark(const Options& options) {
    RendererConfig config;
    config.width = options.width;
    config.height = options.height;
    config.headless = true;
    config.validation = options.validation;
    config.visibility_mode = options.visibility;
    auto renderer = Renderer(config);
    const auto scene = benchmark_scene(options);
    for (unsigned i = 0; i < options.warmup; ++i)
        renderer.render(scene);
    if (!options.csv.parent_path().empty())
        fs::create_directories(faset::native_io_path(options.csv.parent_path()));
    std::ofstream csv(faset::native_io_path(options.csv));
    if (!csv)
        throw std::runtime_error("Cannot open benchmark CSV: " + faset::path_to_utf8(options.csv));
    csv << "light_count,shadows,visibility,effective_visibility,lighting_path,"
           "build_configuration,run_index,frame,"
           "device,driver,commit,width,height,validation_enabled,validation_errors,"
           "submitted_local_lights,omitted_local_lights,shadow_tiles,draw_calls,gpu_bytes,"
           "gpu_main_raster_ms,gpu_post_raster_ms,gpu_post_visible,visibility_counters_valid,"
           "gpu_shadow_ms,gpu_ms,cpu_ms,readback_cpu_ms\n";
    csv << std::fixed << std::setprecision(6);
    for (unsigned frame = 0; frame < options.frames; ++frame) {
        renderer.render(scene);
        const auto stats = renderer.stats();
        if (stats.validation_errors != 0)
            throw std::runtime_error("Vulkan validation error during benchmark");
        if (stats.submitted_local_lights != options.lights || stats.omitted_local_lights != 0)
            throw std::runtime_error("Renderer did not submit every requested local light");
        if (stats.effective_visibility_mode != options.visibility)
            throw std::runtime_error("Requested visibility path fell back during benchmark");
        if (stats.gpu_main_raster_ms <= 0 || stats.gpu_ms <= 0)
            throw std::runtime_error("GPU raster or frame timestamp was unavailable");
        csv << options.lights << ',' << (options.shadows ? "on" : "off") << ','
            << mode_name(options.visibility) << ',' << mode_name(stats.effective_visibility_mode)
            << ",forward," << FASET_BENCHMARK_CONFIGURATION << ','
            << options.run_index << ',' << frame << ',';
        csv_text(csv, stats.device);
        csv << ',';
        csv_text(csv, options.driver);
        csv << ',';
        csv_text(csv, options.commit);
        csv << ',' << options.width << ',' << options.height << ','
            << (stats.validation_enabled ? 1 : 0) << ',' << stats.validation_errors << ','
            << stats.submitted_local_lights << ',' << stats.omitted_local_lights
            << ",0," << stats.draw_calls << ',' << stats.gpu_allocated_bytes << ','
            << stats.gpu_main_raster_ms << ',' << stats.gpu_post_raster_ms << ','
            << stats.gpu_post_visible << ',' << (stats.visibility_counters_valid ? 1 : 0)
            << ",0," << stats.gpu_ms << ','
            << stats.cpu_ms << ',' << stats.readback_cpu_ms << '\n';
    }
    if (!csv)
        throw std::runtime_error("Cannot finish benchmark CSV: " + faset::path_to_utf8(options.csv));
    if (!options.capture.empty())
        renderer.capture(faset::native_io_path(options.capture));
}
} // namespace

int benchmark_main(int argc, char** argv) {
    try {
        benchmark(parse(argc, argv));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "P3 lighting benchmark: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return faset::run_utf8_main(argc, argv, benchmark_main);
}
#else
int main(int argc, char** argv) {
    return benchmark_main(argc, argv);
}
#endif
