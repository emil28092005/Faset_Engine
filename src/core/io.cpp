#include <array>
#include <faset/core/error.hpp>
#include <faset/core/io.hpp>
#include <fstream>
#include <mutex>
#include <random>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace faset {
std::string new_id() {
    static std::mutex mutex;
    static std::random_device random;
    std::array<unsigned char, 16> bytes{};
    {
        std::lock_guard lock(mutex);
        for (auto& byte : bytes)
            byte = static_cast<unsigned char>(random());
    }
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            result += '-';
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 15];
    }
    return result;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "io.open", "Cannot open file: " + path.string());
    std::string value((std::istreambuf_iterator<char>(stream)), {});
    require(!stream.bad(), "io.read", "Cannot read file: " + path.string());
    return value;
}
Json read_json(const std::filesystem::path& path) {
    try {
        return Json::parse(read_text(path));
    } catch (const Json::exception& error) {
        throw Error("format.json", "Invalid JSON in " + path.string(), {{"reason", error.what()}});
    }
}
void atomic_write(const std::filesystem::path& path, std::string_view bytes) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(parent);
    const auto temporary = parent / (path.filename().string() + ".tmp-" + new_id());
    try {
#ifdef _WIN32
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        require(file != INVALID_HANDLE_VALUE, "io.create", "Cannot create temporary file");
        bool ok = true;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            const auto count =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 30));
            if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written == 0) {
                ok = false;
                break;
            }
            offset += written;
        }
        ok = FlushFileBuffers(file) && ok;
        CloseHandle(file);
        require(ok, "io.write", "Cannot flush temporary file");
        require(MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "io.replace", "Cannot publish file: " + path.string());
#else
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        require(fd >= 0, "io.create", "Cannot create temporary file");
        bool ok = true;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                ok = false;
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
        ok = (::fsync(fd) == 0) && ok;
        const auto closed = ::close(fd);
        ok = ok && (closed == 0);
        require(ok, "io.write", "Cannot flush temporary file");
        std::filesystem::rename(temporary, path);
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) {
            ::fsync(directory);
            ::close(directory);
        }
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
void atomic_write_json(const std::filesystem::path& path, const Json& value) {
    atomic_write(path, value.dump(2) + "\n");
}
std::filesystem::path project_path(const std::filesystem::path& root,
                                   const std::filesystem::path& relative) {
    require(!relative.is_absolute(), "path.outside_project",
            "Expected a path relative to the project");
    const auto canonical = std::filesystem::weakly_canonical(root);
    const auto target = std::filesystem::weakly_canonical(canonical / relative);
    auto a = canonical.begin(), b = target.begin();
    for (; a != canonical.end(); ++a, ++b)
        require(b != target.end() && *a == *b, "path.outside_project",
                "Path escapes the project root");
    return target;
}
} // namespace faset
