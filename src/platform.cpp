#include "fel/platform.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fel::platform {
namespace {

std::string describe_last_error() {
#ifdef _WIN32
    const DWORD code = ::GetLastError();
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (length != 0 && buffer != nullptr) {
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                                                 nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            text.resize(static_cast<std::size_t>(needed));
            ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), needed,
                                  nullptr, nullptr);
        }
        ::LocalFree(buffer);
    }
    if (text.empty()) {
        text = "windows error";
    }
    return "os error " + std::to_string(code) + ": " + text;
#else
    return std::string{"os error "} + std::to_string(errno) + ": " + std::strerror(errno);
#endif
}

}  // namespace

FileLock::~FileLock() { close(); }

FileLock::FileLock(FileLock&& other) noexcept
    : handle_(other.handle_), exclusive_(other.exclusive_) {
    other.handle_ = nullptr;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        exclusive_ = other.exclusive_;
        other.handle_ = nullptr;
    }
    return *this;
}

void FileLock::close() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    OVERLAPPED overlapped{};
    ::UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped);
    ::CloseHandle(static_cast<HANDLE>(handle_));
#else
    ::flock(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), LOCK_UN);
    ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
#endif
    handle_ = nullptr;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, bool exclusive) {
    FileLock lock;
    lock.exclusive_ = exclusive;
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Reason{ReasonCode::LockUnavailable, describe_last_error()};
    }
    OVERLAPPED overlapped{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (exclusive) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    if (::LockFileEx(handle, flags, 0, 1, 0, &overlapped) == 0) {
        const std::string detail = describe_last_error();
        ::CloseHandle(handle);
        return Reason{ReasonCode::LockHeldExclusive,
                      "another process holds the ledger lock: " + detail};
    }
    lock.handle_ = handle;
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return Reason{ReasonCode::LockUnavailable, describe_last_error()};
    }
    const int operation = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(descriptor, operation) != 0) {
        const std::string detail = describe_last_error();
        ::close(descriptor);
        return Reason{ReasonCode::LockHeldExclusive,
                      "another process holds the ledger lock: " + detail};
    }
    lock.handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(descriptor));
#endif
    return lock;
}

DurableFile::~DurableFile() { close(); }

DurableFile::DurableFile(DurableFile&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

DurableFile& DurableFile::operator=(DurableFile&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void DurableFile::close() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(static_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
#endif
    handle_ = nullptr;
}

Result<DurableFile> DurableFile::open(const std::filesystem::path& path, bool write, bool create) {
    DurableFile file;
#ifdef _WIN32
    const DWORD access = write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    const DWORD disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
    const HANDLE handle = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr,
                                        disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Reason{ReasonCode::IoError, path.string() + ": " + describe_last_error()};
    }
    file.handle_ = handle;
#else
    const int flags = (write ? O_RDWR : O_RDONLY) | (create ? O_CREAT : 0) | O_CLOEXEC;
    const int descriptor = ::open(path.c_str(), flags, 0644);
    if (descriptor < 0) {
        return Reason{ReasonCode::IoError, path.string() + ": " + describe_last_error()};
    }
    file.handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(descriptor));
#endif
    return file;
}

Result<DurableFile> DurableFile::open_for_append(const std::filesystem::path& path) {
    return open(path, true, true);
}

Result<std::uint64_t> DurableFile::append(std::span<const std::uint8_t> data) {
    if (handle_ == nullptr) {
        return Reason{ReasonCode::InvalidState, "file is not open"};
    }
#ifdef _WIN32
    LARGE_INTEGER distance{};
    LARGE_INTEGER position{};
    if (::SetFilePointerEx(static_cast<HANDLE>(handle_), distance, &position, FILE_END) == 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (data.size() - written_total) > 0x7FFFFFFFU ? 0x7FFFFFFFU : (data.size() - written_total));
        DWORD written = 0;
        if (::WriteFile(static_cast<HANDLE>(handle_), data.data() + written_total, chunk, &written,
                        nullptr) == 0) {
            return Reason{ReasonCode::IoError, describe_last_error()};
        }
        written_total += written;
    }
    return static_cast<std::uint64_t>(position.QuadPart);
#else
    const off_t position = ::lseek(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), 0, SEEK_END);
    if (position < 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const ssize_t written = ::write(static_cast<int>(reinterpret_cast<intptr_t>(handle_)),
                                        data.data() + written_total, data.size() - written_total);
        if (written < 0) {
            return Reason{ReasonCode::IoError, describe_last_error()};
        }
        written_total += static_cast<std::size_t>(written);
    }
    return static_cast<std::uint64_t>(position);
#endif
}

Status DurableFile::flush() {
    if (handle_ == nullptr) {
        return Status::fail(ReasonCode::InvalidState, "file is not open");
    }
#ifdef _WIN32
    if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
#else
    if (::fsync(static_cast<int>(reinterpret_cast<intptr_t>(handle_))) != 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
#endif
    return Status::ok();
}

Result<std::uint64_t> DurableFile::size() {
    if (handle_ == nullptr) {
        return Reason{ReasonCode::InvalidState, "file is not open"};
    }
#ifdef _WIN32
    LARGE_INTEGER result{};
    if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &result) == 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    return static_cast<std::uint64_t>(result.QuadPart);
#else
    struct stat info {};
    if (::fstat(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), &info) != 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    return static_cast<std::uint64_t>(info.st_size);
#endif
}

Status DurableFile::truncate(std::uint64_t length) {
    if (handle_ == nullptr) {
        return Status::fail(ReasonCode::InvalidState, "file is not open");
    }
#ifdef _WIN32
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(length);
    if (::SetFilePointerEx(static_cast<HANDLE>(handle_), target, nullptr, FILE_BEGIN) == 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
    if (::SetEndOfFile(static_cast<HANDLE>(handle_)) == 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
#else
    if (::ftruncate(static_cast<int>(reinterpret_cast<intptr_t>(handle_)),
                    static_cast<off_t>(length)) != 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
#endif
    return flush();
}

Result<std::vector<std::uint8_t>> DurableFile::read_range(std::uint64_t offset,
                                                          std::uint64_t length) {
    if (handle_ == nullptr) {
        return Reason{ReasonCode::InvalidState, "file is not open"};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(length));
#ifdef _WIN32
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(static_cast<HANDLE>(handle_), target, nullptr, FILE_BEGIN) == 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    std::size_t read_total = 0;
    while (read_total < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (out.size() - read_total) > 0x7FFFFFFFU ? 0x7FFFFFFFU : (out.size() - read_total));
        DWORD read = 0;
        if (::ReadFile(static_cast<HANDLE>(handle_), out.data() + read_total, chunk, &read, nullptr) ==
            0) {
            return Reason{ReasonCode::IoError, describe_last_error()};
        }
        if (read == 0) {
            break;
        }
        read_total += read;
    }
    out.resize(read_total);
#else
    if (::lseek(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), static_cast<off_t>(offset),
                SEEK_SET) < 0) {
        return Reason{ReasonCode::IoError, describe_last_error()};
    }
    std::size_t read_total = 0;
    while (read_total < out.size()) {
        const ssize_t read = ::read(static_cast<int>(reinterpret_cast<intptr_t>(handle_)),
                                    out.data() + read_total, out.size() - read_total);
        if (read < 0) {
            return Reason{ReasonCode::IoError, describe_last_error()};
        }
        if (read == 0) {
            break;
        }
        read_total += static_cast<std::size_t>(read);
    }
    out.resize(read_total);
#endif
    return out;
}

Result<std::vector<std::uint8_t>> DurableFile::read_all() {
    const Result<std::uint64_t> total = size();
    if (!total.ok()) {
        return total.reason();
    }
    return read_range(0, total.value());
}

Status read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out) {
    Result<DurableFile> file = DurableFile::open(path, false, false);
    if (!file.ok()) {
        return Status{file.reason()};
    }
    DurableFile handle = file.take();
    const Result<std::vector<std::uint8_t>> data = handle.read_all();
    if (!data.ok()) {
        return Status{data.reason()};
    }
    out = data.value();
    return Status::ok();
}

Status write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        Result<DurableFile> file = DurableFile::open(temporary, true, true);
        if (!file.ok()) {
            return Status{file.reason()};
        }
        DurableFile handle = file.take();
        const Status truncated = handle.truncate(0);
        if (!truncated.is_ok()) {
            return truncated;
        }
        const Result<std::uint64_t> written = handle.append(data);
        if (!written.ok()) {
            return Status{written.reason()};
        }
        const Status flushed = handle.flush();
        if (!flushed.is_ok()) {
            return flushed;
        }
    }
#ifdef _WIN32
    if (::MoveFileExW(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return Status::fail(ReasonCode::IoError,
                            "atomic replace of " + path.string() + " failed: " + describe_last_error());
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        return Status::fail(ReasonCode::IoError,
                            "atomic replace of " + path.string() + " failed: " + describe_last_error());
    }
#endif
    const Status synced = sync_directory(path.parent_path());
    (void)synced;
    return Status::ok();
}

Status remove_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        return Status::fail(ReasonCode::IoError, "cannot remove " + path.string() + ": " + error.message());
    }
    return Status::ok();
}

Status sync_directory(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Windows does not expose a portable directory flush; NTFS metadata
        // ordering after MoveFileEx with MOVEFILE_WRITE_THROUGH is the guarantee
        // relied upon, so a failure here is reported but not fatal.
        return Status::fail(ReasonCode::Unsupported, "directory flush is not available on this platform");
    }
    ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        return Status::fail(ReasonCode::IoError, describe_last_error());
    }
    ::close(descriptor);
#endif
    return Status::ok();
}

bool file_exists(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return Reason{ReasonCode::IoError, path.string() + ": " + error.message()};
    }
    return static_cast<std::uint64_t>(size);
}

Instant system_utc_now() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    return Instant{static_cast<std::int64_t>(seconds.count()),
                   static_cast<std::int32_t>(nanos.count())};
}

}  // namespace fel::platform
