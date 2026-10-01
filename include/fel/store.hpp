#pragma once

// Durable, single-writer, append-only ledger store.
//
// Layout of a ledger directory:
//   fel.manifest                atomically published pointer to the live files
//   fel.lock                    kernel-locked single-writer lock file
//   fel-segment-NNNNNNNN.fel    append-only record stream
//   fel-snapshot-NNNNNNNN.fel   compacted retained record set
//
// Exact commit point for a record: the record is committed once its complete
// header, payload, and chain trailer are on disk AND the append has been flushed
// with a durable write barrier. A record whose bytes are only partially present
// is not committed; recovery discards it.
//
// Exact commit point for a compaction: the atomic replacement of fel.manifest.
// Files written before that replacement but never named by the manifest are
// uncommitted and are collected as garbage on the next writer open.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "fel/journal.hpp"
#include "fel/platform.hpp"

namespace fel {

enum class OpenMode : std::uint8_t {
    Reader = 0,
    Writer = 1,
};

struct StoreOptions {
    OpenMode mode{OpenMode::Reader};
    bool create_if_missing{false};
    LedgerId ledger_id{};
    std::string runtime{"facility-efficiency-ledger"};
    // Deterministic clock injection. When empty the system UTC clock is used.
    std::function<Instant()> clock{};
};

struct RecoveryReport {
    bool recovered_torn_tail{false};
    std::uint64_t truncated_bytes{0};
    std::uint64_t committed_records{0};
    std::uint64_t orphan_files_removed{0};
    AdoptionBasis basis{AdoptionBasis::CleanReopen};
    Revision revision{};
    Sequence last_sequence{};
    Digest chain_head{};
    Epoch epoch{};
    std::string detail{};
};

// A record together with the frame that carries it.
struct EncodedFrame {
    std::vector<std::uint8_t> bytes{};
    Digest chain_digest{};
    FrameHeader header{};
};

EncodedFrame encode_record_frame(const Record& record, const Digest& previous_chain);

class Store {
public:
    Store() = default;
    ~Store() = default;
    Store(Store&&) noexcept = default;
    Store& operator=(Store&&) noexcept = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    static Result<Store> open(const std::filesystem::path& directory, const StoreOptions& options);

    // Assigns sequence/revision/epoch, frames the record, appends it, and makes
    // it durable before returning. On failure the store is poisoned: subsequent
    // appends are refused rather than risking a silently divergent in-memory
    // revision.
    Result<Record> append(Record record);

    // Collapses the active segment into a new snapshot plus a fresh segment and
    // publishes the change atomically through the manifest.
    Result<std::uint64_t> compact();

    const std::vector<Record>& records() const noexcept { return records_; }
    const RecoveryReport& recovery() const noexcept { return recovery_; }
    const std::filesystem::path& directory() const noexcept { return directory_; }
    OpenMode mode() const noexcept { return mode_; }
    bool is_writer() const noexcept { return mode_ == OpenMode::Writer; }
    Revision revision() const noexcept { return revision_; }
    Epoch epoch() const noexcept { return epoch_; }
    Sequence sequence() const noexcept { return sequence_; }
    Digest chain_head() const noexcept { return chain_head_; }
    std::string active_segment_name() const { return active_segment_; }
    std::string base_snapshot_name() const { return base_snapshot_; }

private:
    // Removes ledger files that the manifest no longer names. Compaction is the
    // only operation that retires files, and it publishes the manifest first, so
    // anything unreferenced here was never committed.
    std::uint64_t collect_orphans();

    std::filesystem::path directory_{};
    OpenMode mode_{OpenMode::Reader};
    platform::FileLock lock_{};
    platform::DurableFile segment_file_{};
    std::string active_segment_{};
    std::string base_snapshot_{};
    std::vector<Record> records_{};
    RecoveryReport recovery_{};
    Revision revision_{};
    Epoch epoch_{};
    Sequence sequence_{};
    Digest chain_head_{};
    LedgerId ledger_id_{};
    std::function<Instant()> clock_{};
    bool poisoned_{false};
};

struct IntegrityReport {
    bool ok{false};
    std::uint64_t snapshot_records{0};
    std::uint64_t segment_records{0};
    bool torn_tail{false};
    std::uint64_t torn_bytes{0};
    Digest chain_head{};
    Digest snapshot_digest{};
    Revision revision{};
    Sequence sequence{};
    Epoch epoch{};
    std::string detail{};
};

// Reads and validates the ledger directory without mutating it.
Result<IntegrityReport> verify_ledger_directory(const std::filesystem::path& directory);

// True when the directory contains no ledger artefacts at all.
bool ledger_directory_is_pristine(const std::filesystem::path& directory);

}  // namespace fel
