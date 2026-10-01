#pragma once

// Thin platform layer for the durable substrate.
//
// Everything here is required to be real: kernel-enforced file locking, durable
// flushes, and atomic replacement. The Windows and POSIX implementations use
// the native primitives of each platform; there is no simulated fallback that
// would let a claim of durability pass without durability.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "fel/status.hpp"
#include "fel/time.hpp"

namespace fel::platform {

// Kernel-enforced advisory lock over a lock file. The lock is released when the
// process exits, so an abandoned lock never survives a crash.
class FileLock {
public:
    FileLock() noexcept = default;
    ~FileLock();

    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    static Result<FileLock> acquire(const std::filesystem::path& path, bool exclusive);

    bool held() const noexcept { return handle_ != nullptr; }
    bool exclusive() const noexcept { return exclusive_; }

private:
    void close() noexcept;

    void* handle_{nullptr};
    bool exclusive_{false};
};

// Append-only file with explicit durability control.
class DurableFile {
public:
    DurableFile() noexcept = default;
    ~DurableFile();

    DurableFile(DurableFile&& other) noexcept;
    DurableFile& operator=(DurableFile&& other) noexcept;
    DurableFile(const DurableFile&) = delete;
    DurableFile& operator=(const DurableFile&) = delete;

    static Result<DurableFile> open_for_append(const std::filesystem::path& path);
    static Result<DurableFile> open(const std::filesystem::path& path, bool write, bool create);

    Result<std::uint64_t> append(std::span<const std::uint8_t> data);
    Status flush();
    Result<std::uint64_t> size();
    Status truncate(std::uint64_t length);
    Result<std::vector<std::uint8_t>> read_all();
    Result<std::vector<std::uint8_t>> read_range(std::uint64_t offset, std::uint64_t length);

    bool open() const noexcept { return handle_ != nullptr; }

private:
    void close() noexcept;

    void* handle_{nullptr};
};

Status read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out);
Status write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> data);
Status remove_file(const std::filesystem::path& path) noexcept;
Status sync_directory(const std::filesystem::path& path) noexcept;
bool file_exists(const std::filesystem::path& path) noexcept;
Result<std::uint64_t> file_size(const std::filesystem::path& path);

// UTC wall clock, truncated to nanoseconds. Used only for records that describe
// when an operator action happened; accounting outcomes never depend on it.
Instant system_utc_now() noexcept;

}  // namespace fel::platform
