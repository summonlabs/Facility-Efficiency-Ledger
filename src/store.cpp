#include "fel/store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

#include "fel/codec.hpp"
#include "fel/digest.hpp"
#include "fel/version.hpp"

namespace fel {
namespace {

constexpr std::string_view kManifestName = "fel.manifest";
constexpr std::string_view kLockName = "fel.lock";
constexpr std::string_view kSegmentPrefix = "fel-segment-";
constexpr std::string_view kSnapshotPrefix = "fel-snapshot-";
constexpr std::string_view kFileSuffix = ".fel";

bool all_zero(std::span<const std::uint8_t> data) noexcept {
    for (std::uint8_t byte : data) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string numbered_name(std::string_view prefix, std::uint64_t number) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.*s%08llu%.*s",
                                      static_cast<int>(prefix.size()), prefix.data(),
                                      static_cast<unsigned long long>(number),
                                      static_cast<int>(kFileSuffix.size()), kFileSuffix.data());
    if (written <= 0) {
        return std::string{prefix};
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

std::optional<std::uint64_t> number_from_name(std::string_view name, std::string_view prefix) {
    if (name.size() != prefix.size() + 8U + kFileSuffix.size()) {
        return std::nullopt;
    }
    if (name.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    if (name.substr(name.size() - kFileSuffix.size()) != kFileSuffix) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t i = prefix.size(); i < prefix.size() + 8U; ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

struct Manifest {
    std::string ledger_id{};
    std::string active_segment{};
    std::string base_snapshot{};
    Epoch published_epoch{};
    std::string runtime{};
};

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest) {
    Writer writer;
    writer.u16(kManifestFormatVersion);
    writer.string(manifest.ledger_id);
    writer.string(manifest.active_segment);
    writer.string(manifest.base_snapshot);
    writer.u64(manifest.published_epoch.value());
    writer.string(manifest.runtime);
    writer.digest(Sha256::hash(writer.bytes()));
    return writer.take();
}

Result<Manifest> decode_manifest(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kDigestBytes + 2U) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest is shorter than its own header"};
    }
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - kDigestBytes);
    const std::span<const std::uint8_t> trailer = bytes.subspan(bytes.size() - kDigestBytes);
    const Digest computed = Sha256::hash(body);
    if (!std::equal(computed.bytes().begin(), computed.bytes().end(), trailer.begin())) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest digest does not match its contents"};
    }
    Reader reader{body};
    const Result<std::uint16_t> version = reader.u16();
    if (!version.ok()) {
        return version.reason();
    }
    if (version.value() != kManifestFormatVersion) {
        return Reason{ReasonCode::FormatVersionUnsupported,
                      "manifest format version " + std::to_string(version.value()) +
                          " is not supported by this build"};
    }
    Manifest manifest;
    const Result<std::string_view> ledger_id = reader.string();
    if (!ledger_id.ok()) return ledger_id.reason();
    manifest.ledger_id.assign(ledger_id.value());
    const Result<std::string_view> segment = reader.string();
    if (!segment.ok()) return segment.reason();
    manifest.active_segment.assign(segment.value());
    const Result<std::string_view> snapshot = reader.string();
    if (!snapshot.ok()) return snapshot.reason();
    manifest.base_snapshot.assign(snapshot.value());
    const Result<std::uint64_t> epoch = reader.u64();
    if (!epoch.ok()) return epoch.reason();
    manifest.published_epoch = Epoch{epoch.value()};
    const Result<std::string_view> runtime = reader.string();
    if (!runtime.ok()) return runtime.reason();
    manifest.runtime.assign(runtime.value());
    if (!reader.at_end()) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest has trailing bytes"};
    }
    if (manifest.active_segment.empty()) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest names no active segment"};
    }
    if (number_from_name(manifest.active_segment, kSegmentPrefix).has_value() == false) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest active segment name is malformed"};
    }
    if (!manifest.base_snapshot.empty() &&
        !number_from_name(manifest.base_snapshot, kSnapshotPrefix).has_value()) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest snapshot name is malformed"};
    }
    return manifest;
}

struct SnapshotHeader {
    LedgerId ledger_id{};
    Revision base_revision{};
    Digest chain_digest{};
    Epoch epoch{};
    Sequence min_sequence{};
    Sequence max_sequence{};
    std::uint64_t retained_count{0};
    std::uint64_t dropped_count{0};
    Digest dropped_digest{};
    Instant created_at{};
};

std::vector<std::uint8_t> encode_snapshot(const SnapshotHeader& header,
                                          const std::vector<Record>& records) {
    Writer writer;
    writer.u16(kSnapshotFormatVersion);
    writer.string(header.ledger_id.str());
    writer.u64(header.base_revision.value());
    writer.digest(header.chain_digest);
    writer.u64(header.epoch.value());
    writer.u64(header.min_sequence.value());
    writer.u64(header.max_sequence.value());
    writer.u64(header.retained_count);
    writer.u64(header.dropped_count);
    writer.digest(header.dropped_digest);
    writer.instant(header.created_at);
    for (const Record& record : records) {
        const Result<std::vector<std::uint8_t>> payload = encode_record_payload(record);
        const std::vector<std::uint8_t> bytes =
            payload.ok() ? payload.value() : std::vector<std::uint8_t>{};
        writer.u64(record.sequence().value());
        writer.u64(record.revision().value());
        writer.u64(record.epoch().value());
        writer.u16(static_cast<std::uint16_t>(record.type()));
        writer.u32(static_cast<std::uint32_t>(bytes.size()));
        writer.raw(bytes);
        writer.digest(Sha256::hash(std::span<const std::uint8_t>{bytes.data(), bytes.size()}));
    }
    writer.digest(Sha256::hash(writer.bytes()));
    return writer.take();
}

Result<std::pair<SnapshotHeader, std::vector<Record>>> decode_snapshot(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kDigestBytes) {
        return Reason{ReasonCode::SnapshotCorrupt, "snapshot is shorter than its own trailer"};
    }
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - kDigestBytes);
    const std::span<const std::uint8_t> trailer = bytes.subspan(bytes.size() - kDigestBytes);
    const Digest computed = Sha256::hash(body);
    if (!std::equal(computed.bytes().begin(), computed.bytes().end(), trailer.begin())) {
        return Reason{ReasonCode::SnapshotCorrupt, "snapshot digest does not match its contents"};
    }
    Reader reader{body};
    const Result<std::uint16_t> version = reader.u16();
    if (!version.ok()) return version.reason();
    if (version.value() != kSnapshotFormatVersion) {
        return Reason{ReasonCode::FormatVersionUnsupported,
                      "snapshot format version " + std::to_string(version.value()) +
                          " is not supported by this build"};
    }
    SnapshotHeader header;
    const Result<std::string_view> ledger_id = reader.string();
    if (!ledger_id.ok()) return ledger_id.reason();
    const Result<LedgerId> parsed_ledger = LedgerId::parse(ledger_id.value());
    if (!parsed_ledger.ok()) {
        return Reason{ReasonCode::SnapshotCorrupt, "snapshot ledger id is not a valid identifier"};
    }
    header.ledger_id = parsed_ledger.value();
    const Result<std::uint64_t> base_revision = reader.u64();
    if (!base_revision.ok()) return base_revision.reason();
    header.base_revision = Revision{base_revision.value()};
    const Result<Digest> chain_digest = reader.digest();
    if (!chain_digest.ok()) return chain_digest.reason();
    header.chain_digest = chain_digest.value();
    const Result<std::uint64_t> epoch = reader.u64();
    if (!epoch.ok()) return epoch.reason();
    header.epoch = Epoch{epoch.value()};
    const Result<std::uint64_t> min_sequence = reader.u64();
    if (!min_sequence.ok()) return min_sequence.reason();
    header.min_sequence = Sequence{min_sequence.value()};
    const Result<std::uint64_t> max_sequence = reader.u64();
    if (!max_sequence.ok()) return max_sequence.reason();
    header.max_sequence = Sequence{max_sequence.value()};
    const Result<std::uint64_t> retained = reader.u64();
    if (!retained.ok()) return retained.reason();
    header.retained_count = retained.value();
    const Result<std::uint64_t> dropped = reader.u64();
    if (!dropped.ok()) return dropped.reason();
    header.dropped_count = dropped.value();
    const Result<Digest> dropped_digest = reader.digest();
    if (!dropped_digest.ok()) return dropped_digest.reason();
    header.dropped_digest = dropped_digest.value();
    const Result<Instant> created_at = reader.instant();
    if (!created_at.ok()) return created_at.reason();
    header.created_at = created_at.value();

    if (header.retained_count > kMaxCollectionItems) {
        return Reason{ReasonCode::BoundsExceeded,
                      "snapshot declares more retained records than the permitted maximum"};
    }

    std::vector<Record> records;
    records.reserve(static_cast<std::size_t>(header.retained_count));
    for (std::uint64_t index = 0; index < header.retained_count; ++index) {
        const Result<std::uint64_t> sequence = reader.u64();
        if (!sequence.ok()) return sequence.reason();
        const Result<std::uint64_t> revision = reader.u64();
        if (!revision.ok()) return revision.reason();
        const Result<std::uint64_t> record_epoch = reader.u64();
        if (!record_epoch.ok()) return record_epoch.reason();
        const Result<std::uint16_t> type_code = reader.u16();
        if (!type_code.ok()) return type_code.reason();
        RecordType type = RecordType::SegmentHeader;
        bool known = false;
        constexpr std::array<RecordType, 12> kTypes{
            RecordType::SegmentHeader,       RecordType::LedgerOpened,
            RecordType::EpochAdopted,        RecordType::SourceRegistered,
            RecordType::SourceRetired,       RecordType::GenerationPublished,
            RecordType::GenerationAttested,  RecordType::MeasurementRecorded,
            RecordType::ClassificationRecorded, RecordType::ResidualRecorded,
            RecordType::TargetVoided,        RecordType::IntervalSealed};
        for (RecordType candidate : kTypes) {
            if (static_cast<std::uint16_t>(candidate) == type_code.value()) {
                type = candidate;
                known = true;
                break;
            }
        }
        if (!known) {
            return Reason{ReasonCode::SnapshotCorrupt, "snapshot record type is not supported"};
        }
        const Result<std::uint32_t> payload_length = reader.u32();
        if (!payload_length.ok()) return payload_length.reason();
        if (payload_length.value() > kMaxRecordPayloadBytes) {
            return Reason{ReasonCode::BoundsExceeded, "snapshot record payload is too large"};
        }
        const Result<std::span<const std::uint8_t>> payload = reader.raw(payload_length.value());
        if (!payload.ok()) return payload.reason();
        const Result<Digest> content = reader.digest();
        if (!content.ok()) return content.reason();
        const Digest computed_content =
            Sha256::hash(std::span<const std::uint8_t>{payload.value().data(), payload.value().size()});
        if (computed_content != content.value()) {
            return Reason{ReasonCode::SnapshotCorrupt,
                          "snapshot record content digest does not match its payload"};
        }
        Result<Record> decoded = decode_record_payload(type, payload.value());
        if (!decoded.ok()) return decoded.reason();
        Record record = decoded.take();
        record.set_sequence(Sequence{sequence.value()});
        record.set_revision(Revision{revision.value()});
        record.set_epoch(Epoch{record_epoch.value()});
        records.push_back(std::move(record));
    }
    if (!reader.at_end()) {
        return Reason{ReasonCode::SnapshotCorrupt, "snapshot has trailing bytes"};
    }
    return std::pair<SnapshotHeader, std::vector<Record>>{header, std::move(records)};
}

struct ScanResult {
    std::vector<Record> records{};
    Digest chain_head{};
    Sequence last_sequence{};
    Revision last_revision{};
    Epoch last_epoch{};
    std::uint64_t committed_bytes{0};
    bool torn_tail{false};
    std::uint64_t torn_bytes{0};
};

Result<ScanResult> scan_segment(std::span<const std::uint8_t> data, const Digest& base_chain,
                                const LedgerId& expected_ledger, Revision expected_base_revision,
                                bool require_header) {
    ScanResult result;
    result.chain_head = base_chain;
    result.last_revision = expected_base_revision;

    std::size_t offset = 0;
    bool first = true;
    while (offset < data.size()) {
        const std::size_t remaining = data.size() - offset;
        if (remaining < kRecordHeaderSize) {
            result.torn_tail = true;
            result.torn_bytes = remaining;
            break;
        }
        const std::span<const std::uint8_t> header_bytes = data.subspan(offset, kRecordHeaderSize);
        const Result<FrameHeader> header = decode_frame_header(header_bytes);
        if (!header.ok()) {
            // A prefix of a record that was never completed, or an unwritten
            // region, is a torn tail. Anything else is interior corruption and
            // is refused rather than repaired.
            if (all_zero(data.subspan(offset))) {
                result.torn_tail = true;
                result.torn_bytes = remaining;
                break;
            }
            if (remaining < kRecordOverhead) {
                result.torn_tail = true;
                result.torn_bytes = remaining;
                break;
            }
            return Reason{ReasonCode::HeaderCorrupt,
                          "record header at offset " + std::to_string(offset) + " is invalid: " +
                              header.reason().message()};
        }
        const std::size_t total = kRecordOverhead + header.value().payload_length;
        if (remaining < total) {
            result.torn_tail = true;
            result.torn_bytes = remaining;
            break;
        }
        const std::span<const std::uint8_t> payload =
            data.subspan(offset + kRecordHeaderSize, header.value().payload_length);
        if (crc32c(payload) != header.value().payload_crc) {
            if (all_zero(data.subspan(offset))) {
                result.torn_tail = true;
                result.torn_bytes = remaining;
                break;
            }
            return Reason{ReasonCode::InteriorCorruption,
                          "record payload checksum at offset " + std::to_string(offset) +
                              " does not match"};
        }
        const Digest chain = compute_chain_digest(result.chain_head, header_bytes, payload);
        const std::span<const std::uint8_t> stored =
            data.subspan(offset + kRecordHeaderSize + header.value().payload_length, kDigestBytes);
        if (!std::equal(chain.bytes().begin(), chain.bytes().end(), stored.begin())) {
            return Reason{ReasonCode::ChainBroken,
                          "record at offset " + std::to_string(offset) +
                              " does not link to its predecessor"};
        }
        Result<Record> decoded = decode_record_payload(header.value().type, payload);
        if (!decoded.ok()) {
            return decoded.reason();
        }
        Record record = decoded.take();
        record.set_sequence(header.value().sequence);
        record.set_revision(header.value().revision);
        record.set_epoch(header.value().epoch);
        record.set_chain_digest(chain);

        if (first) {
            if (require_header) {
                if (record.type() != RecordType::SegmentHeader) {
                    return Reason{ReasonCode::InteriorCorruption,
                                  "segment does not begin with a segment header"};
                }
                const SegmentHeaderBody& body = record.segment_header();
                if (body.ledger_id != expected_ledger) {
                    return Reason{ReasonCode::ChainBroken,
                                  "segment belongs to ledger " + body.ledger_id.str() +
                                      " but the manifest names " + expected_ledger.str()};
                }
                if (body.base_revision != expected_base_revision) {
                    return Reason{ReasonCode::ChainBroken,
                                  "segment base revision " + std::to_string(body.base_revision.value()) +
                                      " does not match the snapshot revision " +
                                      std::to_string(expected_base_revision.value())};
                }
                if (body.base_chain_digest != base_chain) {
                    return Reason{ReasonCode::ChainBroken,
                                  "segment base chain digest does not match the snapshot chain"};
                }
                if (record.revision() != expected_base_revision) {
                    return Reason{ReasonCode::ChainBroken,
                                  "segment header revision does not match the base revision"};
                }
            }
            result.last_revision = record.revision();
            result.last_epoch = record.epoch();
        } else {
            const Result<Sequence> expected_sequence = result.last_sequence.next();
            if (!expected_sequence.ok()) {
                return Reason{ReasonCode::ArithmeticOverflow, "record sequence is exhausted"};
            }
            if (record.sequence() != expected_sequence.value()) {
                return Reason{ReasonCode::InteriorCorruption,
                              "record sequence " + std::to_string(record.sequence().value()) +
                                  " is not contiguous with " +
                                  std::to_string(result.last_sequence.value())};
            }
            if (is_mutation_record(record.type())) {
                const Result<Revision> expected_revision = result.last_revision.next();
                if (!expected_revision.ok()) {
                    return Reason{ReasonCode::ArithmeticOverflow, "record revision is exhausted"};
                }
                if (record.revision() != expected_revision.value()) {
                    return Reason{ReasonCode::InteriorCorruption,
                                  "mutating record revision " +
                                      std::to_string(record.revision().value()) +
                                      " does not advance the previous revision " +
                                      std::to_string(result.last_revision.value()) + " by one"};
                }
                result.last_revision = record.revision();
            } else if (record.revision() != result.last_revision) {
                return Reason{ReasonCode::InteriorCorruption,
                              "non-mutating record changes the ledger revision"};
            }
            if (record.epoch() < result.last_epoch) {
                return Reason{ReasonCode::InteriorCorruption,
                              "record epoch regresses within a segment"};
            }
            result.last_epoch = record.epoch();
        }
        result.last_sequence = record.sequence();
        result.chain_head = chain;
        result.records.push_back(std::move(record));
        offset += total;
        result.committed_bytes = offset;
        first = false;
    }
    return result;
}

Digest digest_of_dropped(const std::vector<Digest>& dropped) {
    std::vector<std::string> hex;
    hex.reserve(dropped.size());
    for (const Digest& digest : dropped) {
        hex.push_back(digest.to_hex());
    }
    std::sort(hex.begin(), hex.end());
    Sha256 hasher;
    for (const std::string& value : hex) {
        hasher.update(value);
    }
    return hasher.finish();
}

std::string record_identity(const Record& record) {
    switch (record.type()) {
        case RecordType::MeasurementRecorded:
            return record.measurement().entry.str();
        case RecordType::ClassificationRecorded:
            return record.classification().allocation.str();
        case RecordType::ResidualRecorded:
            return record.residual().residual.str();
        default:
            return std::string{};
    }
}

bool droppable(RecordType type) {
    return type == RecordType::MeasurementRecorded || type == RecordType::ClassificationRecorded ||
           type == RecordType::ResidualRecorded;
}

std::string describe_recovery(const RecoveryReport& recovery, Revision revision) {
    if (recovery.recovered_torn_tail) {
        return "recovered " + std::to_string(recovery.truncated_bytes) +
               " bytes of an uncommitted record tail; dynamic evidence from the previous session is "
               "not fresh until it is re-attested";
    }
    return "opened cleanly at revision " + std::to_string(revision.value()) + " with " +
           std::to_string(recovery.committed_records) + " committed records";
}

}  // namespace

EncodedFrame encode_record_frame(const Record& record, const Digest& previous_chain) {
    const Result<std::vector<std::uint8_t>> payload = encode_record_payload(record);
    const std::vector<std::uint8_t> payload_bytes =
        payload.ok() ? payload.value() : std::vector<std::uint8_t>{};

    EncodedFrame frame;
    frame.header.format_version = kJournalFormatVersion;
    frame.header.type = record.type();
    frame.header.flags = 0;
    frame.header.sequence = record.sequence();
    frame.header.revision = record.revision();
    frame.header.epoch = record.epoch();
    frame.header.payload_length = static_cast<std::uint32_t>(payload_bytes.size());
    frame.header.payload_crc =
        crc32c(std::span<const std::uint8_t>{payload_bytes.data(), payload_bytes.size()});

    const std::array<std::uint8_t, kRecordHeaderSize> header_bytes =
        encode_frame_header(frame.header);
    frame.chain_digest = compute_chain_digest(previous_chain, header_bytes, payload_bytes);

    frame.bytes.reserve(kRecordOverhead + payload_bytes.size());
    frame.bytes.insert(frame.bytes.end(), header_bytes.begin(), header_bytes.end());
    frame.bytes.insert(frame.bytes.end(), payload_bytes.begin(), payload_bytes.end());
    frame.bytes.insert(frame.bytes.end(), frame.chain_digest.bytes().begin(),
                       frame.chain_digest.bytes().end());
    return frame;
}

bool ledger_directory_is_pristine(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        return true;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return false;
        }
        const std::string name = entry.path().filename().string();
        // The lock file is not a ledger artefact: acquiring the lock creates it
        // before any committed state exists.
        if (name == kLockName) {
            continue;
        }
        if (name.rfind("fel", 0) == 0) {
            return false;
        }
    }
    return true;
}

Result<Store> Store::open(const std::filesystem::path& directory, const StoreOptions& options) {
    Store store;
    store.directory_ = directory;
    store.mode_ = options.mode;
    store.clock_ = options.clock ? options.clock : std::function<Instant()>{&platform::system_utc_now};

    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
        if (options.mode != OpenMode::Writer || !options.create_if_missing) {
            return Reason{ReasonCode::NotFound, directory.string() + " does not exist"};
        }
        std::filesystem::create_directories(directory, error);
        if (error) {
            return Reason{ReasonCode::IoError,
                          "cannot create " + directory.string() + ": " + error.message()};
        }
    } else if (!std::filesystem::is_directory(directory, error) || error) {
        return Reason{ReasonCode::NotADirectory, directory.string() + " is not a directory"};
    }

    Result<platform::FileLock> lock = platform::FileLock::acquire(
        directory / std::filesystem::path{std::string{kLockName}}, options.mode == OpenMode::Writer);
    if (!lock.ok()) {
        return lock.reason();
    }
    store.lock_ = lock.take();

    const std::filesystem::path manifest_path = directory / std::filesystem::path{std::string{kManifestName}};
    Manifest manifest;
    const bool initialized = platform::file_exists(manifest_path);
    if (!initialized) {
        if (options.mode != OpenMode::Writer || !options.create_if_missing) {
            return Reason{ReasonCode::ManifestMissing,
                          manifest_path.string() +
                              " is missing; the directory holds no committed ledger"};
        }
        if (!ledger_directory_is_pristine(directory)) {
            return Reason{ReasonCode::ManifestMissing,
                          "directory contains uncommitted ledger files but no manifest; remove the "
                          "directory or finish initialization"};
        }
        if (!options.ledger_id.valid()) {
            return Reason{ReasonCode::InvalidArgument, "initialization requires a ledger identifier"};
        }
        store.ledger_id_ = options.ledger_id;
        store.active_segment_ = numbered_name(kSegmentPrefix, 1);
        store.base_snapshot_.clear();
        store.chain_head_ = Digest{};
        store.sequence_ = Sequence{};
        store.revision_ = Revision{};
        store.epoch_ = Epoch{};

        const std::filesystem::path segment_path = directory / std::filesystem::path{store.active_segment_};
        Result<platform::DurableFile> segment =
            platform::DurableFile::open(segment_path, true, true);
        if (!segment.ok()) {
            return segment.reason();
        }
        store.segment_file_ = segment.take();
        const Status truncated = store.segment_file_.truncate(0);
        if (!truncated.is_ok()) {
            return truncated.reason();
        }

        const Instant now = store.clock_();
        Record header = Record::make_segment_header(
            SegmentHeaderBody{store.ledger_id_, Revision{}, Digest{}, now});
        header.set_sequence(Sequence{1});
        header.set_revision(Revision{});
        header.set_epoch(Epoch{});
        const EncodedFrame header_frame = encode_record_frame(header, Digest{});
        Record ledger_opened = Record::make_ledger_opened(
            LedgerOpenedBody{store.ledger_id_, now, options.runtime});
        ledger_opened.set_sequence(Sequence{2});
        ledger_opened.set_revision(Revision{});
        ledger_opened.set_epoch(Epoch{});
        const EncodedFrame opened_frame = encode_record_frame(ledger_opened, header_frame.chain_digest);
        header.set_chain_digest(header_frame.chain_digest);
        ledger_opened.set_chain_digest(opened_frame.chain_digest);

        std::vector<std::uint8_t> initial = header_frame.bytes;
        initial.insert(initial.end(), opened_frame.bytes.begin(), opened_frame.bytes.end());
        const Result<std::uint64_t> offset = store.segment_file_.append(initial);
        if (!offset.ok()) {
            return offset.reason();
        }
        const Status flushed = store.segment_file_.flush();
        if (!flushed.is_ok()) {
            return flushed.reason();
        }
        store.chain_head_ = opened_frame.chain_digest;
        store.sequence_ = Sequence{2};
        store.recovery_.committed_records = 2;

        manifest.ledger_id = store.ledger_id_.str();
        manifest.active_segment = store.active_segment_;
        manifest.base_snapshot.clear();
        manifest.published_epoch = Epoch{};
        manifest.runtime = options.runtime;
        const std::vector<std::uint8_t> manifest_bytes = encode_manifest(manifest);
        const Status published = platform::write_file_atomic(manifest_path, manifest_bytes);
        if (!published.is_ok()) {
            return published.reason();
        }
        store.records_.push_back(header);
        store.records_.push_back(ledger_opened);
        store.recovery_.basis = AdoptionBasis::InitialCreation;
    } else {
        std::vector<std::uint8_t> manifest_bytes;
        const Status read = platform::read_file(manifest_path, manifest_bytes);
        if (!read.is_ok()) {
            return read.reason();
        }
        const Result<Manifest> decoded = decode_manifest(manifest_bytes);
        if (!decoded.ok()) {
            return decoded.reason();
        }
        manifest = decoded.value();
        const Result<LedgerId> ledger_id = LedgerId::parse(manifest.ledger_id);
        if (!ledger_id.ok()) {
            return Reason{ReasonCode::ManifestCorrupt, "manifest ledger id is not a valid identifier"};
        }
        store.ledger_id_ = ledger_id.value();
        store.active_segment_ = manifest.active_segment;
        store.base_snapshot_ = manifest.base_snapshot;
        store.epoch_ = manifest.published_epoch;

        std::vector<Record> snapshot_records;
        if (!manifest.base_snapshot.empty()) {
            std::vector<std::uint8_t> snapshot_bytes;
            const Status snapshot_read =
                platform::read_file(directory / std::filesystem::path{manifest.base_snapshot}, snapshot_bytes);
            if (!snapshot_read.is_ok()) {
                return Reason{ReasonCode::SnapshotCorrupt,
                              manifest.base_snapshot + ": " + snapshot_read.message()};
            }
            const Result<std::pair<SnapshotHeader, std::vector<Record>>> snapshot =
                decode_snapshot(snapshot_bytes);
            if (!snapshot.ok()) {
                return snapshot.reason();
            }
            const SnapshotHeader& header = snapshot.value().first;
            if (header.ledger_id != store.ledger_id_) {
                return Reason{ReasonCode::SnapshotCorrupt,
                              "snapshot belongs to ledger " + header.ledger_id.str() +
                                  " but the manifest names " + store.ledger_id_.str()};
            }
            store.chain_head_ = header.chain_digest;
            store.revision_ = header.base_revision;
            snapshot_records = snapshot.value().second;
            for (const Record& record : snapshot_records) {
                if (record.epoch() > store.epoch_) {
                    store.epoch_ = record.epoch();
                }
                if (record.revision() > store.revision_) {
                    store.revision_ = record.revision();
                }
                if (record.sequence() > store.sequence_) {
                    store.sequence_ = record.sequence();
                }
            }
        }

        std::vector<std::uint8_t> segment_bytes;
        const Status segment_read =
            platform::read_file(directory / std::filesystem::path{store.active_segment_}, segment_bytes);
        if (!segment_read.is_ok()) {
            return Reason{ReasonCode::SegmentMissing,
                          store.active_segment_ + ": " + segment_read.message()};
        }
        if (segment_bytes.empty()) {
            return Reason{ReasonCode::InteriorCorruption,
                          store.active_segment_ + " is empty and holds no segment header"};
        }
        Result<ScanResult> scan = scan_segment(segment_bytes, store.chain_head_, store.ledger_id_,
                                               store.revision_, true);
        if (!scan.ok()) {
            return scan.reason();
        }
        ScanResult result = scan.take();
        store.recovery_.recovered_torn_tail = result.torn_tail;
        store.recovery_.truncated_bytes = result.torn_tail ? result.torn_bytes : 0;
        if (result.torn_tail && options.mode == OpenMode::Writer) {
            Result<platform::DurableFile> segment =
                platform::DurableFile::open(directory / std::filesystem::path{store.active_segment_},
                                            true, false);
            if (!segment.ok()) {
                return segment.reason();
            }
            platform::DurableFile file = segment.take();
            const Status truncate = file.truncate(result.committed_bytes);
            if (!truncate.is_ok()) {
                return truncate.reason();
            }
        }

        if (result.records.empty()) {
            return Reason{ReasonCode::InteriorCorruption,
                          store.active_segment_ + " holds no complete record"};
        }
        store.records_ = std::move(snapshot_records);
        for (Record& record : result.records) {
            store.records_.push_back(std::move(record));
        }
        store.chain_head_ = result.chain_head;
        store.sequence_ = result.last_sequence;
        store.revision_ = result.last_revision;
        if (result.last_epoch > store.epoch_) {
            store.epoch_ = result.last_epoch;
        }
        store.recovery_.basis = result.torn_tail ? AdoptionBasis::RecoveredTornTail
                                                 : AdoptionBasis::CleanReopen;

        Result<platform::DurableFile> segment = platform::DurableFile::open(
            directory / std::filesystem::path{store.active_segment_}, true, false);
        if (!segment.ok()) {
            return segment.reason();
        }
        store.segment_file_ = segment.take();
    }

    store.recovery_.committed_records = store.records_.size();
    store.recovery_.revision = store.revision_;
    store.recovery_.last_sequence = store.sequence_;
    store.recovery_.epoch = store.epoch_;
    store.recovery_.chain_head = store.chain_head_;
    store.recovery_.detail = describe_recovery(store.recovery_, store.revision_);

    if (options.mode == OpenMode::Writer) {
        const Result<Epoch> next_epoch = store.epoch_.next();
        if (!next_epoch.ok()) {
            return next_epoch.reason();
        }
        store.epoch_ = next_epoch.value();
        Record adoption = Record::make_epoch_adopted(
            EpochAdoptedBody{store.epoch_, store.clock_(), store.recovery_.basis, store.chain_head_,
                             store.recovery_.detail},
            Sequence{});
        const Result<Record> appended = store.append(std::move(adoption));
        if (!appended.ok()) {
            return appended.reason();
        }
        store.recovery_.epoch = store.epoch_;
        store.recovery_.orphan_files_removed = store.collect_orphans();
        store.recovery_.detail = describe_recovery(store.recovery_, store.revision_);
    }
    return store;
}

Result<Record> Store::append(Record record) {
    if (poisoned_) {
        return Reason{ReasonCode::InvalidState,
                      "the store is poisoned by an earlier durability failure"};
    }
    if (mode_ != OpenMode::Writer) {
        return Reason{ReasonCode::AuthorityNotOwned,
                      "this store handle did not acquire the ledger write lock"};
    }
    const Result<Sequence> next_sequence = sequence_.next();
    if (!next_sequence.ok()) {
        return next_sequence.reason();
    }
    Revision next_revision = revision_;
    if (is_mutation_record(record.type())) {
        const Result<Revision> bumped = revision_.next();
        if (!bumped.ok()) {
            return bumped.reason();
        }
        next_revision = bumped.value();
    }
    record.set_sequence(next_sequence.value());
    record.set_revision(next_revision);
    record.set_epoch(epoch_);

    const EncodedFrame frame = encode_record_frame(record, chain_head_);
    const Result<std::uint64_t> offset = segment_file_.append(frame.bytes);
    if (!offset.ok()) {
        poisoned_ = true;
        return offset.reason();
    }
    const Status flushed = segment_file_.flush();
    if (!flushed.is_ok()) {
        poisoned_ = true;
        return flushed.reason();
    }
    record.set_chain_digest(frame.chain_digest);
    chain_head_ = frame.chain_digest;
    sequence_ = next_sequence.value();
    revision_ = next_revision;
    records_.push_back(record);
    recovery_.committed_records = records_.size();
    recovery_.revision = revision_;
    recovery_.last_sequence = sequence_;
    recovery_.chain_head = chain_head_;
    recovery_.epoch = epoch_;
    return record;
}

Result<std::uint64_t> Store::compact() {
    if (poisoned_) {
        return Reason{ReasonCode::InvalidState,
                      "the store is poisoned by an earlier durability failure"};
    }
    if (mode_ != OpenMode::Writer) {
        return Reason{ReasonCode::AuthorityNotOwned,
                      "compaction requires the ledger write lock"};
    }
    const std::optional<std::uint64_t> current_segment =
        number_from_name(active_segment_, kSegmentPrefix);
    if (!current_segment.has_value()) {
        return Reason{ReasonCode::InvalidState, "active segment name is malformed"};
    }
    std::uint64_t next_snapshot_number = 1;
    if (!base_snapshot_.empty()) {
        const std::optional<std::uint64_t> current_snapshot =
            number_from_name(base_snapshot_, kSnapshotPrefix);
        if (!current_snapshot.has_value()) {
            return Reason{ReasonCode::InvalidState, "base snapshot name is malformed"};
        }
        next_snapshot_number = current_snapshot.value() + 1;
    }

    std::unordered_set<std::string> retired;
    for (const Record& record : records_) {
        if (record.meta().supersedes) {
            retired.insert(record.meta().superseded_id);
        }
        if (record.type() == RecordType::TargetVoided) {
            retired.insert(record.target_voided().target_id);
        }
    }

    std::vector<Record> retained;
    std::vector<Digest> dropped;
    retained.reserve(records_.size());
    for (const Record& record : records_) {
        if (record.type() == RecordType::SegmentHeader) {
            continue;
        }
        if (droppable(record.type())) {
            const std::string identity = record_identity(record);
            if (!identity.empty() && retired.count(identity) != 0U) {
                dropped.push_back(record.content_digest());
                continue;
            }
        }
        retained.push_back(record);
    }

    SnapshotHeader header;
    header.ledger_id = ledger_id_;
    header.base_revision = revision_;
    header.chain_digest = chain_head_;
    header.epoch = epoch_;
    header.min_sequence = retained.empty() ? sequence_ : retained.front().sequence();
    header.max_sequence = retained.empty() ? sequence_ : retained.back().sequence();
    header.retained_count = retained.size();
    header.dropped_count = dropped.size();
    header.dropped_digest = digest_of_dropped(dropped);
    header.created_at = clock_();

    const std::string snapshot_name = numbered_name(kSnapshotPrefix, next_snapshot_number);
    const std::vector<std::uint8_t> snapshot_bytes = encode_snapshot(header, retained);
    const Status snapshot_status =
        platform::write_file_atomic(directory_ / std::filesystem::path{snapshot_name}, snapshot_bytes);
    if (!snapshot_status.is_ok()) {
        return snapshot_status.reason();
    }

    const std::string segment_name = numbered_name(kSegmentPrefix, current_segment.value() + 1);
    const std::filesystem::path segment_path = directory_ / std::filesystem::path{segment_name};
    Result<platform::DurableFile> new_segment = platform::DurableFile::open(segment_path, true, true);
    if (!new_segment.ok()) {
        return new_segment.reason();
    }
    platform::DurableFile file = new_segment.take();
    const Status truncated = file.truncate(0);
    if (!truncated.is_ok()) {
        return truncated.reason();
    }

    const Result<Epoch> next_epoch = epoch_.next();
    if (!next_epoch.ok()) {
        return next_epoch.reason();
    }
    const Result<Sequence> header_sequence = sequence_.next();
    if (!header_sequence.ok()) {
        return header_sequence.reason();
    }

    Record segment_header = Record::make_segment_header(
        SegmentHeaderBody{ledger_id_, revision_, chain_head_, clock_()});
    segment_header.set_sequence(header_sequence.value());
    segment_header.set_revision(revision_);
    segment_header.set_epoch(epoch_);
    const EncodedFrame segment_frame = encode_record_frame(segment_header, chain_head_);

    Record adoption = Record::make_epoch_adopted(
        EpochAdoptedBody{next_epoch.value(), clock_(), AdoptionBasis::Compaction,
                         segment_frame.chain_digest, "compaction re-anchored the journal chain"},
        Sequence{});
    adoption.set_sequence(Sequence{header_sequence.value().value() + 1U});
    adoption.set_revision(revision_);
    adoption.set_epoch(next_epoch.value());
    const EncodedFrame adoption_frame = encode_record_frame(adoption, segment_frame.chain_digest);

    std::vector<std::uint8_t> initial = segment_frame.bytes;
    initial.insert(initial.end(), adoption_frame.bytes.begin(), adoption_frame.bytes.end());
    const Result<std::uint64_t> written = file.append(initial);
    if (!written.ok()) {
        return written.reason();
    }
    const Status flushed = file.flush();
    if (!flushed.is_ok()) {
        return flushed.reason();
    }

    Manifest manifest;
    manifest.ledger_id = ledger_id_.str();
    manifest.active_segment = segment_name;
    manifest.base_snapshot = snapshot_name;
    manifest.published_epoch = next_epoch.value();
    manifest.runtime = kRuntimeIdentity.empty() ? std::string{"facility-efficiency-ledger"}
                                                : std::string{kRuntimeIdentity};
    const std::vector<std::uint8_t> manifest_bytes = encode_manifest(manifest);
    const Status published = platform::write_file_atomic(
        directory_ / std::filesystem::path{std::string{kManifestName}}, manifest_bytes);
    if (!published.is_ok()) {
        (void)platform::remove_file(segment_path);
        (void)platform::remove_file(directory_ / std::filesystem::path{snapshot_name});
        return published.reason();
    }

    // The manifest now names the new files, so the in-memory names must follow
    // before anything unreferenced is collected as garbage.
    segment_file_ = std::move(file);
    active_segment_ = segment_name;
    base_snapshot_ = snapshot_name;
    const std::uint64_t removed = collect_orphans();

    segment_header.set_chain_digest(segment_frame.chain_digest);
    adoption.set_chain_digest(adoption_frame.chain_digest);

    std::vector<Record> compacted;
    compacted.reserve(retained.size() + 2U);
    for (Record& record : retained) {
        compacted.push_back(std::move(record));
    }
    compacted.push_back(segment_header);
    compacted.push_back(adoption);
    records_ = std::move(compacted);

    chain_head_ = adoption_frame.chain_digest;
    sequence_ = adoption.sequence();
    epoch_ = next_epoch.value();
    recovery_.orphan_files_removed += removed;
    recovery_.revision = revision_;
    recovery_.last_sequence = sequence_;
    recovery_.chain_head = chain_head_;
    recovery_.epoch = epoch_;
    recovery_.committed_records = records_.size();
    return static_cast<std::uint64_t>(dropped.size());
}

std::uint64_t Store::collect_orphans() {
    std::uint64_t removed = 0;
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory_, error)) {
        if (error) {
            break;
        }
        const std::string name = entry.path().filename().string();
        const bool is_segment = number_from_name(name, kSegmentPrefix).has_value();
        const bool is_snapshot = number_from_name(name, kSnapshotPrefix).has_value();
        const bool is_temporary = name.size() > 4U && name.compare(name.size() - 4U, 4U, ".tmp") == 0;
        if (!is_segment && !is_snapshot && !is_temporary) {
            continue;
        }
        if (name == active_segment_ || name == base_snapshot_) {
            continue;
        }
        candidates.push_back(entry.path());
    }
    for (const std::filesystem::path& path : candidates) {
        const Status status = platform::remove_file(path);
        if (status.is_ok()) {
            ++removed;
        }
    }
    return removed;
}

Result<IntegrityReport> verify_ledger_directory(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return Reason{ReasonCode::NotADirectory, directory.string() + " is not a directory"};
    }
    const std::filesystem::path lock_path = directory / std::filesystem::path{std::string{kLockName}};
    platform::FileLock lock;
    if (platform::file_exists(lock_path)) {
        Result<platform::FileLock> acquired = platform::FileLock::acquire(lock_path, false);
        if (!acquired.ok()) {
            return acquired.reason();
        }
        lock = acquired.take();
    }

    const std::filesystem::path manifest_path =
        directory / std::filesystem::path{std::string{kManifestName}};
    std::vector<std::uint8_t> manifest_bytes;
    const Status read = platform::read_file(manifest_path, manifest_bytes);
    if (!read.is_ok()) {
        return Reason{ReasonCode::ManifestMissing, read.message()};
    }
    const Result<Manifest> manifest = decode_manifest(manifest_bytes);
    if (!manifest.ok()) {
        return manifest.reason();
    }
    const Result<LedgerId> ledger_id = LedgerId::parse(manifest.value().ledger_id);
    if (!ledger_id.ok()) {
        return Reason{ReasonCode::ManifestCorrupt, "manifest ledger id is not a valid identifier"};
    }

    IntegrityReport report;
    Digest chain = Digest{};
    Revision revision{};

    if (!manifest.value().base_snapshot.empty()) {
        std::vector<std::uint8_t> snapshot_bytes;
        const Status snapshot_read = platform::read_file(
            directory / std::filesystem::path{manifest.value().base_snapshot}, snapshot_bytes);
        if (!snapshot_read.is_ok()) {
            return Reason{ReasonCode::SnapshotCorrupt,
                          manifest.value().base_snapshot + ": " + snapshot_read.message()};
        }
        report.snapshot_digest = Sha256::hash(
            std::span<const std::uint8_t>{snapshot_bytes.data(), snapshot_bytes.size()});
        const Result<std::pair<SnapshotHeader, std::vector<Record>>> snapshot =
            decode_snapshot(snapshot_bytes);
        if (!snapshot.ok()) {
            return snapshot.reason();
        }
        if (snapshot.value().first.ledger_id != ledger_id.value()) {
            return Reason{ReasonCode::SnapshotCorrupt, "snapshot describes a different ledger"};
        }
        chain = snapshot.value().first.chain_digest;
        revision = snapshot.value().first.base_revision;
        report.snapshot_records = snapshot.value().second.size();
        for (const Record& record : snapshot.value().second) {
            if (record.revision() > revision) {
                revision = record.revision();
            }
        }
    }

    std::vector<std::uint8_t> segment_bytes;
    const Status segment_read = platform::read_file(
        directory / std::filesystem::path{manifest.value().active_segment}, segment_bytes);
    if (!segment_read.is_ok()) {
        return Reason{ReasonCode::SegmentMissing,
                      manifest.value().active_segment + ": " + segment_read.message()};
    }
    if (segment_bytes.empty()) {
        return Reason{ReasonCode::InteriorCorruption, "active segment is empty"};
    }
    Result<ScanResult> scan =
        scan_segment(segment_bytes, chain, ledger_id.value(), revision, true);
    if (!scan.ok()) {
        return scan.reason();
    }
    ScanResult result = scan.take();
    report.segment_records = result.records.size();
    report.torn_tail = result.torn_tail;
    report.torn_bytes = result.torn_tail ? result.torn_bytes : 0;
    report.chain_head = result.chain_head;
    report.revision = result.last_revision;
    report.sequence = result.last_sequence;
    report.epoch = result.last_epoch;
    report.ok = true;
    report.detail = result.torn_tail
                        ? "committed records verified; an uncommitted tail of " +
                              std::to_string(result.torn_bytes) + " bytes was found and ignored"
                        : "all committed records verified";
    return report;
}

}  // namespace fel
