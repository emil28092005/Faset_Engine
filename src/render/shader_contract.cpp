#include "shader_contract.hpp"
#include <cstring>
#include <faset/core/hash.hpp>
#include <faset/core/io.hpp>
#include <faset/render/renderer.hpp>
#include <stdexcept>
#include <string_view>

namespace faset::render {
namespace {
using faset::Json;
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error("Shader contract: " + message);
}
std::string read_bounded(const std::filesystem::path& path, std::uintmax_t maximum) {
    const auto native = faset::native_io_path(path);
    require(std::filesystem::is_regular_file(native), "missing " + faset::path_to_utf8(path));
    require(std::filesystem::file_size(native) <= maximum,
            "oversized " + faset::path_to_utf8(path));
    return faset::read_text(path);
}
void locations(const Json& fields, std::initializer_list<const char*> types, const char* label) {
    require(fields.is_array() && fields.size() == types.size(),
            std::string(label) + " count changed");
    std::size_t index{};
    for (const auto* type : types) {
        require(fields[index].at("location") == index && fields[index].at("type") == type,
                std::string(label) + " location/type changed");
        ++index;
    }
}
void validate_tile_layout(const Json& layout) {
    require(layout.at("stage") == "compute", "light tile shader stage changed");
    const auto& descriptors = layout.at("descriptors");
    require(descriptors.is_array() && descriptors.size() == 2,
            "light tile descriptor count changed");
    for (std::size_t i = 0; i < 2; ++i)
        require(descriptors[i].at("set") == 0 && descriptors[i].at("binding") == i &&
                    descriptors[i].at("count") == 1 &&
                    descriptors[i].at("type") == "storage_buffer" &&
                    descriptors[i].at("element_stride") == (i == 0 ? 80 : 4),
                "light tile descriptor ABI changed");
    const auto& constants = layout.at("push_constants");
    require(constants.is_array() && constants.size() == 1 &&
                constants[0].at("offset") == 0 && constants[0].at("size") == 96,
            "light tile push size changed");
    const auto& members = constants[0].at("members");
    require(members.is_array() && members.size() == 3,
            "light tile push members changed");
    const int offsets[] = {0, 64, 80};
    const char* types[] = {"float32x4x4", "float32x4", "uint32x4"};
    for (std::size_t i = 0; i < 3; ++i)
        require(members[i].at("offset") == offsets[i] &&
                    members[i].at("size") == (i == 0 ? 64 : 16) &&
                    members[i].at("type") == types[i],
                "light tile push field changed");
    const auto& blocks = layout.at("spirv_push_constants");
    require(blocks.is_array() && blocks.size() == 1 &&
                blocks[0].at("members").size() == 3,
            "light tile SPIR-V push block changed");
    const auto& actual = blocks[0].at("members");
    for (std::size_t i = 0; i < 3; ++i)
        require(actual[i].at("member") == i && actual[i].at("offset") == offsets[i],
                "light tile SPIR-V push offset changed");
    require(actual[0].at("matrix_layout") == "row-major" &&
                actual[0].at("matrix_stride") == 16,
            "light tile SPIR-V matrix storage convention changed");
    locations(layout.at("inputs"), {}, "light tile inputs");
    locations(layout.at("outputs"), {}, "light tile outputs");
}
void validate_layout(const Json& layout, std::string_view entry) {
    const bool fragment = entry == "fragmentMain" || entry == "temporalFragmentMain";
    const bool temporal = entry == "temporalVertexMain" ||
                          entry == "temporalFragmentMain";
    require(layout.at("stage") == (fragment ? "fragment" : "vertex"), "shader stage changed");
    const auto& descriptors = layout.at("descriptors");
    require(descriptors.is_array() && descriptors.size() == 9, "descriptor count changed");
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& binding = descriptors[i];
        const auto set = i < 4 ? 0 : 1;
        const auto slot = set == 0 ? i : i - 4;
        require(binding.at("set") == set && binding.at("binding") == slot &&
                    binding.at("count") == 1,
                "descriptor set, binding or array count changed");
        const auto* expected_type = set == 0 ? (slot % 2 ? "sampler" : "sampled_image_2d")
                                    : slot == 3 ? "sampled_image_2d" : "storage_buffer";
        require(binding.at("type") == expected_type,
                "descriptor type changed");
        if (set == 1 && slot != 3)
            require(binding.at("element_stride") == (slot == 4 ? 4 : slot == 2 ? 112 : 80),
                    "lighting storage record stride changed");
        require(fragment || !binding.at("used").get<bool>(),
                "vertex texture bindings are unsupported");
    }
    const auto& constants = layout.at("push_constants");
    require(constants.is_array() && constants.size() == 1 && constants[0].at("offset") == 0 &&
                constants[0].at("size") == 96,
            "push-constant block changed");
    const auto& members = constants[0].at("members");
    require(members.is_array() && members.size() == 3, "push-constant member count changed");
    const int offsets[] = {0, 64, 80}, sizes[] = {64, 16, 16};
    const char* types[] = {"float32x4x4", "float32x4", "float32x4"};
    for (std::size_t i = 0; i < 3; ++i)
        require(members[i].at("offset") == offsets[i] && members[i].at("size") == sizes[i] &&
                    members[i].at("type") == types[i],
                "push-constant member layout changed");
    const auto& blocks = layout.at("spirv_push_constants");
    require(blocks.is_array() && blocks.size() <= 1, "SPIR-V push-constant block count changed");
    for (const auto& block : blocks) {
        const auto& actual = block.at("members");
        require(actual.is_array() && actual.size() == 3, "SPIR-V push-constant members changed");
        for (std::size_t i = 0; i < 3; ++i)
            require(actual[i].at("member") == i && actual[i].at("offset") == offsets[i],
                    "SPIR-V push-constant offsets changed");
        // Slang lowers column-major host matrices to a transposed SPIR-V matrix type.
        require(actual[0].at("matrix_layout") == "row-major" && actual[0].at("matrix_stride") == 16,
                "SPIR-V matrix storage convention changed");
    }
    if (fragment) {
        if (temporal) {
            locations(layout.at("inputs"),
                      {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2",
                       "float32x4", "float32x4", "float32"}, "temporal fragment inputs");
            locations(layout.at("outputs"), {"float32x4", "float32x4"},
                      "temporal fragment outputs");
        } else {
            locations(layout.at("inputs"),
                      {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2"},
                      "fragment inputs");
            locations(layout.at("outputs"), {"float32x4"}, "fragment outputs");
        }
    } else {
        if (temporal) {
            locations(layout.at("inputs"),
                      {"float32x4", "float32x3", "float32x3", "float32x4", "float32x2",
                       "float32x2", "float32x4", "float32"}, "temporal vertex inputs");
            locations(layout.at("outputs"),
                      {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2",
                       "float32x4", "float32x4", "float32"}, "temporal vertex outputs");
        } else {
            locations(layout.at("inputs"),
                      {"float32x4", "float32x3", "float32x3", "float32x4", "float32x2", "float32x2"},
                      "vertex inputs");
        if (entry == "vertexMain")
            locations(layout.at("outputs"),
                      {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2"},
                      "vertex outputs");
        else
            locations(layout.at("outputs"), {}, "shadow outputs");
        }
    }
}
void validate_gpu_layout(const Json& layout, std::string_view entry) {
    const bool graphics = entry == "gpuVertexMain" || entry == "gpuShadowMain" ||
                          entry == "gpuTemporalVertexMain";
    const bool hzb = entry == "gpuHzbMain";
    const bool compute = !graphics;
    require(layout.at("stage") == (compute ? "compute" : "vertex"), "GPU shader stage changed");
    const auto& descriptors = layout.at("descriptors");
    const std::size_t expected_count = graphics ? 3 : hzb ? 2 : 10;
    require(descriptors.is_array() && descriptors.size() == expected_count,
            "GPU descriptor count changed");
    const std::array<int, 10> compute_strides{288, 16, 16, 4, 16, 4, 4, 0, 0, 208};
    const std::array<int, 3> graphics_strides{288, 4, 208};
    for (std::size_t i = 0; i < expected_count; ++i) {
        const auto& binding = descriptors[i];
        require(binding.at("set") == (graphics ? 2 : 0) && binding.at("binding") == i &&
                    binding.at("count") == 1,
                "GPU descriptor set, binding or count changed");
        const int stride = graphics ? graphics_strides[i] : hzb ? 0 : compute_strides[i];
        const char* type = hzb ? (i == 0 ? "sampled_image_2d" : "storage_image_2d")
                           : stride > 0 ? "storage_buffer" : "sampled_image_2d";
        require(binding.at("type") == type, "GPU descriptor type changed");
        if (stride > 0)
            require(binding.at("element_stride") == stride, "GPU storage record stride changed");
    }
    const auto& constants = layout.at("push_constants");
    require(constants.is_array() && constants.size() == 1 &&
                constants[0].at("offset") == 0 &&
                constants[0].at("size") == (graphics ? 112 : 16),
            "GPU push-constant block changed");
    const auto& members = constants[0].at("members");
    require(members.is_array() && members.size() == 4, "GPU push-constant fields changed");
    const int graphics_offsets[] = {0, 64, 80, 96};
    const int graphics_sizes[] = {64, 16, 16, 16};
    const char* graphics_types[] = {"float32x4x4", "float32x4", "float32x4", "uint32x4"};
    for (std::size_t i = 0; i < 4; ++i) {
        require(members[i].at("offset") == (graphics ? graphics_offsets[i] : int(i) * 4) &&
                    members[i].at("size") == (graphics ? graphics_sizes[i] : 4) &&
                    members[i].at("type") == (graphics ? graphics_types[i] : "uint32"),
                "GPU push-constant layout changed");
    }
    const auto& blocks = layout.at("spirv_push_constants");
    require(blocks.is_array() && blocks.size() == 1, "GPU SPIR-V push block changed");
    const auto& actual = blocks[0].at("members");
    require(actual.is_array() && actual.size() == 4, "GPU SPIR-V push members changed");
    for (std::size_t i = 0; i < 4; ++i)
        require(actual[i].at("member") == i &&
                    actual[i].at("offset") == (graphics ? graphics_offsets[i] : int(i) * 4),
                "GPU SPIR-V push offsets changed");
    if (graphics) {
        require(actual[0].at("matrix_layout") == "row-major" &&
                    actual[0].at("matrix_stride") == 16,
                "GPU SPIR-V matrix storage convention changed");
        locations(layout.at("inputs"),
                  {"float32x3", "float32x3", "float32x4", "float32x2"},
                  "GPU vertex inputs");
        if (entry == "gpuVertexMain" || entry == "gpuTemporalVertexMain")
            if (entry == "gpuTemporalVertexMain")
                locations(layout.at("outputs"),
                          {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2",
                           "float32x4", "float32x4", "float32"},
                          "GPU temporal vertex outputs");
            else
            locations(layout.at("outputs"),
                      {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2"},
                      "GPU vertex outputs");
        else
            locations(layout.at("outputs"), {}, "GPU shadow outputs");
    } else {
        locations(layout.at("inputs"), {}, "GPU compute inputs");
        locations(layout.at("outputs"), {}, "GPU compute outputs");
    }
}
void validate_temporal_layout(const Json& layout, std::string_view entry) {
    const bool resolve = entry == "temporalResolveMain";
    const bool vertex = entry == "temporalCompositeVertexMain";
    require(resolve || vertex || entry == "temporalCompositeFragmentMain",
            "unknown temporal shader entry");
    require(layout.at("stage") == (resolve ? "compute" : vertex ? "vertex" : "fragment"),
            "temporal shader stage changed");
    const auto& descriptors = layout.at("descriptors");
    require(descriptors.is_array() && descriptors.size() == (resolve ? 8u : 1u),
            "temporal descriptor count changed");
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& descriptor = descriptors[i];
        require(descriptor.at("set") == 0 && descriptor.at("binding") == i &&
                    descriptor.at("count") == 1,
                "temporal descriptor set, binding or count changed");
        require(descriptor.at("type") ==
                    (resolve && i == 7 ? "storage_buffer"
                     : resolve && i >= 5 ? "storage_image_2d" : "sampled_image_2d"),
                "temporal descriptor type changed");
        if (resolve && i == 7)
            require(descriptor.at("element_stride") == 4,
                    "temporal counter element stride changed");
        require(descriptor.at("used") == (resolve || !vertex),
                "temporal entry uses an unexpected image binding");
    }
    const auto& constants = layout.at("push_constants");
    const auto& spirv_constants = layout.at("spirv_push_constants");
    require(constants.is_array() && spirv_constants.is_array(),
            "temporal push constants are malformed");
    if (resolve) {
        require(constants.size() == 1 && constants[0].at("offset") == 0 &&
                    constants[0].at("size") == 80 && spirv_constants.size() == 1,
                "temporal resolve push block changed");
        const auto& members = constants[0].at("members");
        const auto& spirv_members = spirv_constants[0].at("members");
        require(members.size() == 5 && spirv_members.size() == 5,
                "temporal resolve push members changed");
        const char* types[] = {"uint32x4", "float32x4", "float32x4", "uint32x4",
                               "float32x4"};
        for (std::size_t i = 0; i < 5; ++i)
            require(members[i].at("offset") == 16 * i &&
                        members[i].at("size") == 16 && members[i].at("type") == types[i] &&
                        spirv_members[i].at("member") == i &&
                        spirv_members[i].at("offset") == 16 * i,
                    "temporal resolve push member ABI changed");
    } else
        require(constants.empty() && spirv_constants.empty(),
                "temporal composite unexpectedly uses push constants");
    if (resolve) {
        locations(layout.at("inputs"), {}, "temporal resolve inputs");
        locations(layout.at("outputs"), {}, "temporal resolve outputs");
    } else if (vertex) {
        locations(layout.at("inputs"), {"float32x4"}, "temporal composite vertex inputs");
        locations(layout.at("outputs"), {}, "temporal composite vertex outputs");
    } else {
        locations(layout.at("inputs"), {}, "temporal composite fragment inputs");
        locations(layout.at("outputs"), {"float32x4"},
                  "temporal composite fragment outputs");
    }
    const auto& input_builtins = layout.at("input_builtins");
    require(input_builtins.is_array() && input_builtins.size() == (vertex ? 0u : 1u),
            "temporal entry input builtin count changed");
    if (!vertex)
        require(input_builtins[0].at("semantic") ==
                    (resolve ? "SV_DISPATCHTHREADID" : "SV_POSITION") &&
                    input_builtins[0].at("type") ==
                    (resolve ? "uint32x3" : "float32x4"),
                "temporal entry input builtin changed");
    const auto& output_builtins = layout.at("output_builtins");
    require(output_builtins.is_array() && output_builtins.size() == (vertex ? 1u : 0u),
            "temporal entry output builtin count changed");
    if (vertex)
        require(output_builtins[0].at("semantic") == "SV_POSITION" &&
                    output_builtins[0].at("type") == "float32x4",
                "temporal composite vertex position changed");
}
void validate_spirv(const std::vector<std::uint32_t>& words, std::uint32_t execution_model) {
    require(words.size() >= 5 && words[0] == 0x07230203 && words[1] >= 0x00010000 &&
                words[1] <= 0x00010600 && words[3] > 0 && words[3] < (1u << 20) && words[4] == 0,
            "invalid SPIR-V header");
    bool entry_found{};
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16;
        const auto opcode = words[offset] & 0xffff;
        require(count > 0 && count <= words.size() - offset, "malformed SPIR-V instruction");
        if (opcode == 15) { // OpEntryPoint
            require(count >= 4, "malformed SPIR-V entry point");
            const char* name = reinterpret_cast<const char*>(&words[offset + 3]);
            const auto available = (count - 3) * sizeof(std::uint32_t);
            const auto* terminator = static_cast<const char*>(std::memchr(name, 0, available));
            require(terminator != nullptr, "unterminated SPIR-V entry name");
            if (std::string_view(name, terminator - name) == "main") {
                require(words[offset + 1] == execution_model, "SPIR-V entry stage changed");
                entry_found = true;
            }
        }
        offset += count;
    }
    require(entry_found, "SPIR-V main entry point missing");
}
detail::ShaderCode load(const std::filesystem::path& directory, const char* entry,
                        bool gpu = false, bool temporal = false) {
    const auto bytes = read_bounded(directory / (std::string(entry) + ".spv"), 16 * 1024 * 1024);
    require(bytes.size() >= 20 && bytes.size() % 4 == 0, "invalid SPIR-V byte length");
    const auto metadata = Json::parse(
        read_bounded(directory / (std::string(entry) + ".reflection.json"), 1024 * 1024));
    require(metadata.at("format") == "faset.shader-reflection" && metadata.at("version") == 1,
            "unsupported reflection version");
    require(metadata.at("source_entry") == entry && metadata.at("entry_point") == "main",
            "reflection entry point mismatch");
    require(metadata.at("spirv_sha256") == faset::sha256(bytes), "SPIR-V/reflection hash mismatch");
    const auto& layout = metadata.at("layout");
    const auto fingerprint = faset::sha256(layout.dump());
    require(metadata.at("layout_fingerprint") == fingerprint, "layout fingerprint mismatch");
    if (temporal)
        validate_temporal_layout(layout, entry);
    else if (gpu)
        validate_gpu_layout(layout, entry);
    else if (std::string_view(entry) == "lightTileMain")
        validate_tile_layout(layout);
    else
        validate_layout(layout, entry);
    detail::ShaderCode result;
    result.layout_fingerprint = fingerprint;
    result.words.resize(bytes.size() / 4);
    std::memcpy(result.words.data(), bytes.data(), bytes.size());
    const auto stage = std::string_view(entry) == "lightTileMain" ? 5u : temporal
        ? (std::string_view(entry) == "temporalResolveMain" ? 5u
           : std::string_view(entry) == "temporalCompositeVertexMain" ? 0u : 4u)
        : gpu ? ((std::string_view(entry) == "gpuVertexMain" ||
                  std::string_view(entry) == "gpuShadowMain" ||
                  std::string_view(entry) == "gpuTemporalVertexMain") ? 0u : 5u)
              : ((std::string_view(entry) == "fragmentMain" ||
                  std::string_view(entry) == "temporalFragmentMain") ? 4u : 0u);
    validate_spirv(result.words, stage);
    return result;
}
} // namespace
std::array<detail::ShaderCode, 4>
detail::load_shader_bundle(const std::filesystem::path& directory) {
    return {load(directory, "vertexMain"), load(directory, "fragmentMain"),
            load(directory, "shadowMain"), load(directory, "lightTileMain")};
}
std::array<detail::ShaderCode, 5>
detail::load_gpu_shader_bundle(const std::filesystem::path& directory) {
    return {load(directory, "gpuVertexMain", true), load(directory, "gpuShadowMain", true),
            load(directory, "gpuCullMain", true), load(directory, "gpuHzbMain", true),
            load(directory, "gpuPostCullMain", true)};
}
std::array<detail::ShaderCode, 3>
detail::load_temporal_shader_bundle(const std::filesystem::path& directory) {
    return {load(directory, "temporalResolveMain", false, true),
            load(directory, "temporalCompositeVertexMain", false, true),
            load(directory, "temporalCompositeFragmentMain", false, true)};
}
std::array<detail::ShaderCode, 3>
detail::load_temporal_scene_shader_bundle(const std::filesystem::path& directory) {
    return {load(directory, "temporalVertexMain"),
            load(directory, "temporalFragmentMain"),
            load(directory, "gpuTemporalVertexMain", true)};
}
void validate_shader_bundle(const std::filesystem::path& directory) {
    (void)detail::load_shader_bundle(directory);
    (void)detail::load_temporal_shader_bundle(directory);
    (void)detail::load_temporal_scene_shader_bundle(directory);
}
void validate_gpu_shader_bundle(const std::filesystem::path& directory) {
    (void)detail::load_gpu_shader_bundle(directory);
}
} // namespace faset::render
