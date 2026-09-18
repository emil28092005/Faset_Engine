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
void validate_layout(const Json& layout, std::string_view entry) {
    const bool fragment = entry == "fragmentMain";
    require(layout.at("stage") == (fragment ? "fragment" : "vertex"), "shader stage changed");
    const auto& descriptors = layout.at("descriptors");
    require(descriptors.is_array() && descriptors.size() == 4, "descriptor count changed");
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& binding = descriptors[i];
        require(binding.at("set") == 0 && binding.at("binding") == i && binding.at("count") == 1,
                "descriptor set, binding or array count changed");
        require(binding.at("type") == (i % 2 ? "sampler" : "sampled_image_2d"),
                "descriptor type changed");
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
        locations(layout.at("inputs"),
                  {"float32x3", "float32x3", "float32x4", "float32x2", "float32x2"},
                  "fragment inputs");
        locations(layout.at("outputs"), {"float32x4"}, "fragment outputs");
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
void validate_spirv(const std::vector<std::uint32_t>& words, bool fragment) {
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
                require(words[offset + 1] == (fragment ? 4u : 0u), "SPIR-V entry stage changed");
                entry_found = true;
            }
        }
        offset += count;
    }
    require(entry_found, "SPIR-V main entry point missing");
}
detail::ShaderCode load(const std::filesystem::path& directory, const char* entry) {
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
    validate_layout(layout, entry);
    detail::ShaderCode result;
    result.layout_fingerprint = fingerprint;
    result.words.resize(bytes.size() / 4);
    std::memcpy(result.words.data(), bytes.data(), bytes.size());
    validate_spirv(result.words, std::string_view(entry) == "fragmentMain");
    return result;
}
} // namespace
std::array<detail::ShaderCode, 3>
detail::load_shader_bundle(const std::filesystem::path& directory) {
    return {load(directory, "vertexMain"), load(directory, "fragmentMain"),
            load(directory, "shadowMain")};
}
void validate_shader_bundle(const std::filesystem::path& directory) {
    (void)detail::load_shader_bundle(directory);
}
} // namespace faset::render
