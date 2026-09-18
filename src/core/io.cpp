#include <array>
#include <faset/core/error.hpp>
#include <faset/core/io.hpp>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace faset {
namespace {
void validate_utf8_path(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        require(first != 0, "path.encoding", "NUL in filesystem path");
        if (first < 0x80)
            continue;
        unsigned remaining = first >= 0xc2 && first <= 0xdf   ? 1
                             : first >= 0xe0 && first <= 0xef ? 2
                             : first >= 0xf0 && first <= 0xf4 ? 3
                                                              : 0;
        require(remaining != 0 && i + remaining <= text.size(), "path.encoding",
                "Invalid UTF-8 path");
        const unsigned minimum = remaining == 1 ? 0x80 : remaining == 2 ? 0x800 : 0x10000;
        unsigned code = first & ((1u << (6 - remaining)) - 1u);
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            require((next & 0xc0) == 0x80, "path.encoding", "Invalid UTF-8 path");
            code = (code << 6) | (next & 0x3f);
        }
        require(code >= minimum && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff),
                "path.encoding", "Invalid UTF-8 path");
    }
}
std::string utf8_bytes(const std::u8string& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
} // namespace
std::filesystem::path path_from_utf8(std::string_view text) {
    validate_utf8_path(text);
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::string path_to_utf8(const std::filesystem::path& path) {
    return utf8_bytes(path.u8string());
}
std::string generic_path_to_utf8(const std::filesystem::path& path) {
    return utf8_bytes(path.generic_u8string());
}
std::filesystem::path native_io_path(const std::filesystem::path& path) {
#ifdef _WIN32
    auto normalized = std::filesystem::absolute(path).lexically_normal();
    normalized.make_preferred();
    const auto& native = normalized.native();
    if (native.starts_with(LR"(\\?\)") || native.starts_with(LR"(\\.\)"))
        return normalized;
    if (native.starts_with(LR"(\\)"))
        return std::filesystem::path(std::wstring(LR"(\\?\UNC\)") + native.substr(2));
    return std::filesystem::path(std::wstring(LR"(\\?\)") + native);
#else
    return path;
#endif
}
#ifdef _WIN32
int run_utf8_main(int argc, wchar_t** argv, int (*entry)(int, char**)) noexcept {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                                    nullptr, 0, nullptr, nullptr);
            require(length > 0, "cli.encoding", "Invalid Unicode command-line argument");
            std::string value(static_cast<std::size_t>(length), '\0');
            require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(),
                                        length, nullptr, nullptr) == length,
                    "cli.encoding", "Cannot convert command-line argument to UTF-8");
            value.pop_back();
            arguments.push_back(std::move(value));
        }
        std::vector<char*> pointers;
        pointers.reserve(arguments.size() + 1);
        for (auto& value : arguments)
            pointers.push_back(value.data());
        pointers.push_back(nullptr);
        return entry(argc, pointers.data());
    } catch (const std::exception& error) {
        std::cerr << "Command line failed: " << error.what() << '\n';
        return 1;
    }
}
#endif
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
    std::ifstream stream(native_io_path(path), std::ios::binary);
    require(bool(stream), "io.open", "Cannot open file: " + path_to_utf8(path));
    std::string value((std::istreambuf_iterator<char>(stream)), {});
    require(!stream.bad(), "io.read", "Cannot read file: " + path_to_utf8(path));
    return value;
}
Json read_json(const std::filesystem::path& path) {
    try {
        return Json::parse(read_text(path));
    } catch (const Json::exception& error) {
        throw Error("format.json", "Invalid JSON in " + path_to_utf8(path),
                    {{"reason", error.what()}});
    }
}
void atomic_write(const std::filesystem::path& path, std::string_view bytes) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(native_io_path(parent));
    auto temporary = path;
    temporary += ".tmp-" + new_id(); // Append ASCII to the native path without a narrow round trip.
    try {
#ifdef _WIN32
        HANDLE file = CreateFileW(native_io_path(temporary).c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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
        require(MoveFileExW(native_io_path(temporary).c_str(), native_io_path(path).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "io.replace", "Cannot publish file: " + path_to_utf8(path));
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
        std::filesystem::remove(native_io_path(temporary), ignored);
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
