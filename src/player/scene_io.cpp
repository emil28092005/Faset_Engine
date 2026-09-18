#include <cstring>
#include <faset/core/io.hpp>
#include <faset/player/SceneView.hpp>
#include <stdexcept>

namespace faset::player {
nlohmann::json readScene(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("Scene file does not exist: " + faset::path_to_utf8(path));
    if (std::filesystem::file_size(faset::native_io_path(path)) > 256 * 1024 * 1024)
        throw std::runtime_error("Scene exceeds the 256 MiB reader limit");
    const auto bytes = faset::read_text(path);
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "FASETSCN", 8) == 0) {
        if (bytes.size() < 20)
            throw std::runtime_error("Truncated cooked scene header");
        std::uint32_t version = 0;
        std::uint64_t size = 0;
        for (int i = 0; i < 4; ++i)
            version |= std::uint32_t(static_cast<unsigned char>(bytes[8 + i])) << (8 * i);
        for (int i = 0; i < 8; ++i)
            size |= std::uint64_t(static_cast<unsigned char>(bytes[12 + i])) << (8 * i);
        if (version != 1)
            throw std::runtime_error("Unsupported cooked scene version");
        if (size != bytes.size() - 20)
            throw std::runtime_error("Cooked scene payload size mismatch");
        return nlohmann::json::from_cbor(bytes.begin() + 20, bytes.end(), true, true);
    }
    if (path.extension() == ".fscene")
        throw std::runtime_error("Cooked scene magic is invalid");
    return nlohmann::json::parse(bytes);
}
} // namespace faset::player
