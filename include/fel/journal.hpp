#pragma once

// Durable journal record model.
//
// The journal is an append-only sequence of framed records. Every record that
// mutates ledger state carries the identity of the caller's request so that a
// retry cannot double-apply: the idempotency key and the request digest are
// committed in the same record as the mutation they describe.
//
// Corrections are additions, never edits. A correcting record names the record
// it supersedes, carries that record's digest, and states a reason. The
// superseded record stays in the journal; the fold reports both.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fel/codec.hpp"
#include "fel/digest.hpp"
#include "fel/ids.hpp"
#include "fel/quantity.hpp"
#include "fel/status.hpp"
#include "fel/time.hpp"
#include "fel/version.hpp"

namespace fel {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class RecordType : std::uint16_t {
    SegmentHeader = 1,
    LedgerOpened = 2,
    EpochAdopted = 3,
    SourceRegistered = 4,
    SourceRetired = 5,
    GenerationPublished = 6,
    GenerationAttested = 7,
    MeasurementRecorded = 8,
    ClassificationRecorded = 9,
    ResidualRecorded = 10,
    TargetVoided = 11,
    IntervalSealed = 12,
};

std::string_view to_string(RecordType type) noexcept;
std::optional<RecordType> record_type_from_string(std::string_view name) noexcept;
bool is_mutation_record(RecordType type) noexcept;

// How a source's quantity becomes known. This describes evidence provenance
// only; it never confers authority over another runtime's facts.
enum class SourceKind : std::uint8_t {
    Meter = 1,             // physical meter or sub-meter reading
    Submeter = 2,
    Estimate = 3,          // modelled or pro-rated estimate
    Allocation = 4,        // derived by an external allocation authority
    ExternalAuthority = 5, // supplied by an adjacent runtime's published report
};

std::string_view to_string(SourceKind kind) noexcept;
std::optional<SourceKind> source_kind_from_string(std::string_view name) noexcept;

// Classification of consumption. Unknown and Unmeasured are first-class
// outcomes: a residual is recorded explicitly and is never folded into waste or
// useful work by default.
enum class ServiceClass : std::uint8_t {
    Useful = 1,
    Avoidable = 2,
    Stranded = 3,
    Wasted = 4,
    Unknown = 5,
    Unmeasured = 6,
};

std::string_view to_string(ServiceClass klass) noexcept;
std::optional<ServiceClass> service_class_from_string(std::string_view name) noexcept;
bool is_residual_class(ServiceClass klass) noexcept;
bool is_consumption_class(ServiceClass klass) noexcept;

// What a measurement's consumption is attributed to. Attribution is a claim
// carried by evidence, not an ownership statement about another runtime.
enum class SubjectKind : std::uint8_t {
    Unattributed = 0,
    Facility = 1,
    Zone = 2,
    Hall = 3,
    Row = 4,
    Rack = 5,
    Device = 6,
    Tenant = 7,
    Workload = 8,
};

std::string_view to_string(SubjectKind kind) noexcept;
std::optional<SubjectKind> subject_kind_from_string(std::string_view name) noexcept;

// What kind of record a void targets.
enum class TargetKind : std::uint8_t {
    Measurement = 1,
    Classification = 2,
    Residual = 3,
    SourceRegistration = 4,
};

std::string_view to_string(TargetKind kind) noexcept;

// Why a ledger session began. Recovery sessions never promote recovered
// dynamic evidence to current.
enum class AdoptionBasis : std::uint8_t {
    InitialCreation = 1,
    CleanReopen = 2,
    RecoveredTornTail = 3,
    Compaction = 4,
};

std::string_view to_string(AdoptionBasis basis) noexcept;

// ---------------------------------------------------------------------------
// Record bodies
// ---------------------------------------------------------------------------

// Present exactly once, as the first record of a segment.
struct SegmentHeaderBody {
    LedgerId ledger_id{};
    Revision base_revision{};
    Digest base_chain_digest{};
    Instant created_at{};
};

struct LedgerOpenedBody {
    LedgerId ledger_id{};
    Instant opened_at{};
    std::string runtime{};
};

struct EpochAdoptedBody {
    Epoch epoch{};
    Instant adopted_at{};
    AdoptionBasis basis{AdoptionBasis::InitialCreation};
    Digest chain_head{};
    std::string detail{};
};

struct SourceRegisteredBody {
    SourceId source{};
    SourceKind kind{SourceKind::Meter};
    Unit unit{Unit::Joule};
    AuthorityId authority{};
    std::string label{};
    Instant registered_at{};
};

struct SourceRetiredBody {
    SourceId source{};
    Instant retired_at{};
    std::string reason{};
    Digest source_digest{};
};

struct GenerationPublishedBody {
    SourceId source{};
    Generation generation{};
    Instant published_at{};
    Digest evidence{};
    Instant valid_until{};
    std::string method{};
};

struct GenerationAttestedBody {
    SourceId source{};
    Generation generation{};
    Instant attested_at{};
    Digest evidence{};
    Instant valid_until{};
    std::string method{};
};

struct MeasurementRecordedBody {
    EntryId entry{};
    Interval interval{};
    SourceId source{};
    Generation generation{};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string method{};
};

struct ClassificationRecordedBody {
    AllocationId allocation{};
    EntryId entry{};
    ServiceClass klass{ServiceClass::Useful};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string method{};
};

struct ResidualRecordedBody {
    ResidualId residual{};
    Interval interval{};
    ServiceClass klass{ServiceClass::Unknown};
    // An unmeasured residual is explicitly not quantified. The boolean keeps
    // "we know it exists but cannot quantify it" distinct from "zero".
    bool quantified{true};
    Quantity quantity{};
    // A bound residual accounts for part of a measured entry: the entry's
    // measurement already counted the resource, so the residual must not be
    // added to inputs a second time. An unbound residual declares resource that
    // no measurement captured, so it does enter the input side.
    bool bound_to_entry{false};
    EntryId entry{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string basis{};
};

struct TargetVoidedBody {
    VoidId void_id{};
    TargetKind target_kind{TargetKind::Measurement};
    std::string target_id{};
    Digest target_digest{};
    Instant voided_at{};
    std::string reason{};
};

struct IntervalSealedBody {
    SealId seal{};
    Interval interval{};
    Revision revision_at_seal{};
    bool closed{false};
    Digest report_digest{};
    Instant sealed_at{};
    std::string note{};
};

// ---------------------------------------------------------------------------
// Record envelope
// ---------------------------------------------------------------------------

// Fields shared by every mutating record: request identity for idempotent
// retries and the supersession declaration for corrections.
struct MutationMeta {
    IdempotencyKey idempotency{};
    Digest request_digest{};
    bool supersedes{false};
    std::string superseded_id{};
    Digest superseded_digest{};
    std::string reason{};
    Epoch caller_epoch{};
    Revision expected_revision{};
};

class Record {
public:
    Record() = default;

    static Record make_segment_header(const SegmentHeaderBody& body);
    static Record make_ledger_opened(const LedgerOpenedBody& body);
    static Record make_epoch_adopted(const EpochAdoptedBody& body, Sequence sequence);
    static Record make_source_registered(const SourceRegisteredBody& body, const MutationMeta& meta);
    static Record make_source_retired(const SourceRetiredBody& body, const MutationMeta& meta);
    static Record make_generation_published(const GenerationPublishedBody& body,
                                            const MutationMeta& meta);
    static Record make_generation_attested(const GenerationAttestedBody& body,
                                           const MutationMeta& meta);
    static Record make_measurement(const MeasurementRecordedBody& body, const MutationMeta& meta);
    static Record make_classification(const ClassificationRecordedBody& body,
                                      const MutationMeta& meta);
    static Record make_residual(const ResidualRecordedBody& body, const MutationMeta& meta);
    static Record make_target_voided(const TargetVoidedBody& body, const MutationMeta& meta);
    static Record make_interval_sealed(const IntervalSealedBody& body, const MutationMeta& meta);

    RecordType type() const noexcept { return type_; }
    void set_type(RecordType type) noexcept { type_ = type; }

    Sequence sequence() const noexcept { return sequence_; }
    void set_sequence(Sequence sequence) noexcept { sequence_ = sequence; }
    Revision revision() const noexcept { return revision_; }
    void set_revision(Revision revision) noexcept { revision_ = revision; }
    Epoch epoch() const noexcept { return epoch_; }
    void set_epoch(Epoch epoch) noexcept { epoch_ = epoch; }
    Digest chain_digest() const noexcept { return chain_digest_; }
    void set_chain_digest(const Digest& digest) noexcept { chain_digest_ = digest; }

    const MutationMeta& meta() const noexcept { return meta_; }
    const SegmentHeaderBody& segment_header() const noexcept { return segment_header_; }
    const LedgerOpenedBody& ledger_opened() const noexcept { return ledger_opened_; }
    const EpochAdoptedBody& epoch_adopted() const noexcept { return epoch_adopted_; }
    const SourceRegisteredBody& source_registered() const noexcept { return source_registered_; }
    const SourceRetiredBody& source_retired() const noexcept { return source_retired_; }
    const GenerationPublishedBody& generation_published() const noexcept {
        return generation_published_;
    }
    const GenerationAttestedBody& generation_attested() const noexcept { return generation_attested_; }
    const MeasurementRecordedBody& measurement() const noexcept { return measurement_; }
    const ClassificationRecordedBody& classification() const noexcept { return classification_; }
    const ResidualRecordedBody& residual() const noexcept { return residual_; }
    const TargetVoidedBody& target_voided() const noexcept { return target_voided_; }
    const IntervalSealedBody& interval_sealed() const noexcept { return interval_sealed_; }

    // Stable identity of the record itself, independent of its position.
    Digest content_digest() const;

    std::string describe() const;

private:
    friend Result<Record> decode_record_payload(RecordType type, std::span<const std::uint8_t> payload);
    friend Result<std::vector<std::uint8_t>> encode_record_payload(const Record& record);

    RecordType type_{RecordType::SegmentHeader};
    Sequence sequence_{};
    Revision revision_{};
    Epoch epoch_{};
    Digest chain_digest_{};
    MutationMeta meta_{};

    SegmentHeaderBody segment_header_{};
    LedgerOpenedBody ledger_opened_{};
    EpochAdoptedBody epoch_adopted_{};
    SourceRegisteredBody source_registered_{};
    SourceRetiredBody source_retired_{};
    GenerationPublishedBody generation_published_{};
    GenerationAttestedBody generation_attested_{};
    MeasurementRecordedBody measurement_{};
    ClassificationRecordedBody classification_{};
    ResidualRecordedBody residual_{};
    TargetVoidedBody target_voided_{};
    IntervalSealedBody interval_sealed_{};
};

Result<std::vector<std::uint8_t>> encode_record_payload(const Record& record);
Result<Record> decode_record_payload(RecordType type, std::span<const std::uint8_t> payload);

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kRecordMagic = 0x524C4546U;  // "FELR" little-endian
inline constexpr std::uint32_t kMaxRecordPayloadBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kRecordHeaderSize = 48;
inline constexpr std::size_t kRecordTrailerSize = kDigestBytes;
inline constexpr std::size_t kRecordOverhead = kRecordHeaderSize + kRecordTrailerSize;

struct FrameHeader {
    std::uint16_t format_version{kJournalFormatVersion};
    RecordType type{RecordType::SegmentHeader};
    std::uint32_t flags{0};
    Sequence sequence{};
    Revision revision{};
    Epoch epoch{};
    std::uint32_t payload_length{0};
    std::uint32_t payload_crc{0};
    std::uint32_t header_crc{0};
};

struct Frame {
    FrameHeader header{};
    Digest chain_digest{};
};

std::array<std::uint8_t, kRecordHeaderSize> encode_frame_header(const FrameHeader& header);
Result<FrameHeader> decode_frame_header(std::span<const std::uint8_t> bytes);

// Chain digest for a record: SHA-256(previous_chain || header_bytes || payload).
Digest compute_chain_digest(const Digest& previous, std::span<const std::uint8_t> header_bytes,
                            std::span<const std::uint8_t> payload);

}  // namespace fel
