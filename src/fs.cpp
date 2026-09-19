#include "fs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace buffer_fabric::detail {
namespace {

namespace stdfs = std::filesystem;

#if defined(_WIN32)
[[nodiscard]] std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<usize>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                              static_cast<int>(utf8.size()), out.data(), needed);
    if (written <= 0) return std::wstring();
    return out;
}

[[nodiscard]] Status win32_error(const char* what) {
    const DWORD code = ::GetLastError();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s failed with win32 error %lu", what,
                  static_cast<unsigned long>(code));
    return Status(ErrorCode::PersistenceError, buffer);
}
#endif

[[nodiscard]] Status io_error(const char* what, const std::string& path) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s failed for '%s': %s", what, path.c_str(),
                  std::strerror(errno));
    return Status(ErrorCode::PersistenceError, buffer);
}

}  // namespace

std::string join_path(const std::string& directory, const std::string& leaf) {
    if (directory.empty()) return leaf;
    const char last = directory.back();
    if (last == '/' || last == '\\') return directory + leaf;
    return directory + "/" + leaf;
}

bool path_exists(const std::string& path) {
    std::error_code ec;
    return stdfs::exists(stdfs::path(path), ec) && !ec;
}

Status ensure_directory(const std::string& path) {
    if (path.empty()) return Status(ErrorCode::InvalidArgument, "directory path is empty");
    std::error_code ec;
    const auto p = stdfs::path(path);
    if (stdfs::exists(p, ec)) {
        if (ec) return Status(ErrorCode::PersistenceError, "cannot stat directory: " + ec.message());
        if (!stdfs::is_directory(p, ec)) {
            return Status(ErrorCode::PersistenceError, "path exists and is not a directory: " + path);
        }
        return Status::success();
    }
    stdfs::create_directories(p, ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "cannot create directory: " + ec.message());
    }
    return Status::success();
}

Status remove_file(const std::string& path) {
    std::error_code ec;
    stdfs::remove(stdfs::path(path), ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "cannot remove file: " + ec.message());
    }
    return Status::success();
}

Result<u64> file_size(const std::string& path) {
    std::error_code ec;
    const auto size = stdfs::file_size(stdfs::path(path), ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "cannot size file: " + ec.message());
    }
    return static_cast<u64>(size);
}

Result<std::vector<u8>> read_file_bounded(const std::string& path, u64 max_bytes, bool* existed) {
    if (existed != nullptr) *existed = false;
    std::error_code ec;
    const auto p = stdfs::path(path);
    if (!stdfs::exists(p, ec)) {
        if (existed != nullptr) *existed = false;
        return std::vector<u8>{};
    }
    if (existed != nullptr) *existed = true;

    const auto size = stdfs::file_size(p, ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "cannot size file: " + ec.message());
    }
    if (static_cast<u64>(size) > max_bytes) {
        return Status(ErrorCode::OversizedMessage, "file exceeds the configured bound");
    }

    std::vector<u8> buffer(static_cast<usize>(size));
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return win32_error("CreateFile");
    DWORD read = 0;
    if (!buffer.empty()) {
        if (!::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            const Status failure = win32_error("ReadFile");
            ::CloseHandle(handle);
            return failure;
        }
    }
    ::CloseHandle(handle);
    if (static_cast<usize>(read) != buffer.size()) {
        return Status(ErrorCode::PersistenceError, "short read");
    }
#else
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return io_error("fopen", path);
    const usize read = buffer.empty() ? 0 : std::fread(buffer.data(), 1, buffer.size(), file);
    std::fclose(file);
    if (read != buffer.size()) return Status(ErrorCode::PersistenceError, "short read");
#endif
    return buffer;
}

Status append_file(const std::string& path, const void* data, usize size, bool durable) {
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return win32_error("CreateFile(append)");
    // The file pointer for FILE_APPEND_DATA always writes at end of file; the
    // offset is ignored for FILE_APPEND_DATA handles, but we set it explicitly
    // so the intent is unambiguous.
    LARGE_INTEGER zero{};
    ::SetFilePointerEx(handle, zero, nullptr, FILE_END);
    const auto* cursor = static_cast<const u8*>(data);
    usize remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(remaining > 0x10000000ull ? 0x10000000ull : remaining);
        DWORD written = 0;
        if (!::WriteFile(handle, cursor, chunk, &written, nullptr)) {
            const Status failure = win32_error("WriteFile");
            ::CloseHandle(handle);
            return failure;
        }
        cursor += written;
        remaining -= written;
    }
    if (durable && !::FlushFileBuffers(handle)) {
        const Status failure = win32_error("FlushFileBuffers");
        ::CloseHandle(handle);
        return failure;
    }
    ::CloseHandle(handle);
    return Status::success();
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return io_error("open", path);
    const auto* cursor = static_cast<const u8*>(data);
    usize remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written <= 0) {
            const Status failure = io_error("write", path);
            ::close(fd);
            return failure;
        }
        cursor += static_cast<usize>(written);
        remaining -= static_cast<usize>(written);
    }
    if (durable && ::fsync(fd) != 0) {
        const Status failure = io_error("fsync", path);
        ::close(fd);
        return failure;
    }
    ::close(fd);
    return Status::success();
#endif
}

Status write_file_atomic(const std::string& path, const std::string& temp_path, const void* data,
                         usize size, bool durable) {
    std::error_code ec;
    stdfs::remove(stdfs::path(temp_path), ec);

#if defined(_WIN32)
    const std::wstring wide = widen(temp_path);
    HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return win32_error("CreateFile(temp)");
    const auto* cursor = static_cast<const u8*>(data);
    usize remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(remaining > 0x10000000ull ? 0x10000000ull : remaining);
        DWORD written = 0;
        if (!::WriteFile(handle, cursor, chunk, &written, nullptr)) {
            const Status failure = win32_error("WriteFile(temp)");
            ::CloseHandle(handle);
            return failure;
        }
        cursor += written;
        remaining -= written;
    }
    if (durable && !::FlushFileBuffers(handle)) {
        const Status failure = win32_error("FlushFileBuffers(temp)");
        ::CloseHandle(handle);
        return failure;
    }
    ::CloseHandle(handle);
#else
    const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return io_error("open", temp_path);
    const auto* cursor = static_cast<const u8*>(data);
    usize remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, cursor, remaining);
        if (written <= 0) {
            const Status failure = io_error("write", temp_path);
            ::close(fd);
            return failure;
        }
        cursor += static_cast<usize>(written);
        remaining -= static_cast<usize>(written);
    }
    if (durable && ::fsync(fd) != 0) {
        const Status failure = io_error("fsync", temp_path);
        ::close(fd);
        return failure;
    }
    ::close(fd);
#endif

    stdfs::rename(stdfs::path(temp_path), stdfs::path(path), ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "atomic replace failed: " + ec.message());
    }
    return Status::success();
}

Status truncate_file(const std::string& path, u64 size) {
    std::error_code ec;
    stdfs::resize_file(stdfs::path(path), static_cast<std::uintmax_t>(size), ec);
    if (ec) {
        return Status(ErrorCode::PersistenceError, "truncate failed: " + ec.message());
    }
    return Status::success();
}

Status sync_directory(const std::string& path) {
#if defined(_WIN32)
    const std::wstring wide = widen(path);
    HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Directory flushing is a best-effort durability strengthening on
        // Windows; the data file itself was already flushed.
        return Status::success();
    }
    ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    return Status::success();
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return io_error("open(dir)", path);
    const int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) return io_error("fsync(dir)", path);
    return Status::success();
#endif
}

Status flush_handle(void* native_handle) {
#if defined(_WIN32)
    if (native_handle == nullptr || native_handle == INVALID_HANDLE_VALUE) {
        return Status(ErrorCode::PersistenceError, "invalid handle");
    }
    if (!::FlushFileBuffers(static_cast<HANDLE>(native_handle))) {
        return win32_error("FlushFileBuffers");
    }
    return Status::success();
#else
    if (native_handle == nullptr) return Status(ErrorCode::PersistenceError, "invalid handle");
    const int fd = *static_cast<int*>(native_handle);
    if (::fsync(fd) != 0) return io_error("fsync", "<handle>");
    return Status::success();
#endif
}

}  // namespace buffer_fabric::detail
