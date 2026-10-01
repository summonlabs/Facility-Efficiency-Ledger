#pragma once

// Deterministic fold of the journal into observable ledger state.
//
// State transitions live in exactly one place: LedgerView::apply. A full replay
// folds every committed record through apply; the live index inside a Ledger
// applies records one at a time through the same function. Replay and live state
// therefore cannot drift apart.
//
// apply() maintains structural state only. Conflicts that require comparing
// records with each other (overlapping evidence, duplicate observations, over
// allocation) are computed by analyze(), which is run by full folds and by every
// reconciliation.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "fel/journal.hpp"

namespace fel {

// Freshness of a source's evidence as seen from a reconciliation reference
// instant. Recovered evidence is usable as history but never as current.
enum class FreshnessState : std::uint8_t {
    Unattested = 0,
    Current = 1,
    Expired = 2,
    Recovered = 3,
    Retired = 4,
};

std::string_view to_string(FreshnessState state) noexcept;

struct SourceState {
    SourceId id{};
    SourceKind kind{SourceKind::Meter};
    Unit unit{Unit::Joule};
    AuthorityId authority{};
    std::string label{};
    Instant registered_at{};
    Digest registration_digest{};
    bool retired{false};
    Instant retired_at{};
    std::string retirement_reason{};
    std::string retired_by{};
    bool has_generation{false};
    Generation generation{};
    Digest generation_evidence{};
    Instant generation_published_at{};
    Instant generation_valid_until{};
    Epoch generation_epoch{};
    bool generation_attested{false};
    FreshnessState freshness{FreshnessState::Unattested};
    std::uint64_t measurement_count{0};
};

struct MeasurementState {
    EntryId id{};
    Interval interval{};
    SourceId source{};
    Generation generation{};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string method{};
    Revision revision{};
    Epoch epoch{};
    Digest record_digest{};
    std::string superseded_by{};
    bool voided{false};
    std::string voided_by{};
    std::string void_reason{};
    Quantity allocated{};
    std::uint64_t allocation_count{0};
};

struct AllocationState {
    AllocationId id{};
    EntryId entry{};
    ServiceClass klass{ServiceClass::Useful};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string method{};
    Revision revision{};
    Epoch epoch{};
    Digest record_digest{};
    std::string superseded_by{};
    bool voided{false};
    std::string voided_by{};
    std::string void_reason{};
};

struct ResidualState {
    ResidualId id{};
    Interval interval{};
    ServiceClass klass{ServiceClass::Unknown};
    bool quantified{true};
    Quantity quantity{};
    bool bound_to_entry{false};
    EntryId entry{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    Instant recorded_at{};
    std::string basis{};
    Revision revision{};
    Epoch epoch{};
    Digest record_digest{};
    std::string superseded_by{};
    bool voided{false};
    std::string voided_by{};
    std::string void_reason{};
};

struct SealState {
    SealId id{};
    Interval interval{};
    Revision revision_at_seal{};
    bool closed{false};
    Digest report_digest{};
    Instant sealed_at{};
    std::string note{};
    Digest record_digest{};
    Revision seal_revision{};
};

struct IdempotencyEntry {
    IdempotencyKey key{};
    Digest request_digest{};
    RecordType type{RecordType::SegmentHeader};
    std::string outcome_id{};
    Digest record_digest{};
    Revision revision{};
    Sequence sequence{};
};

struct ViewIssue {
    ReasonCode code{ReasonCode::Ok};
    std::string subject{};
    std::string detail{};
};

class LedgerView {
public:
    LedgerView() = default;

    // Full deterministic replay: apply every record, then analyze, then judge
    // freshness at the reference instant.
    static Result<LedgerView> fold(const std::vector<Record>& records,
                                   const Instant& freshness_reference);

    // Applies one record. Used by fold() and by the live index alike.
    Status apply(const Record& record);

    // Compares records with each other. Idempotent: the analysis section of the
    // issue list is recomputed from scratch on every call.
    void analyze();

    // Recomputes per-source freshness at the reference instant.
    void finalize(const Instant& freshness_reference);

    const LedgerId& ledger() const noexcept { return ledger_; }
    Revision revision() const noexcept { return revision_; }
    Epoch epoch() const noexcept { return epoch_; }
    Digest chain_head() const noexcept { return chain_head_; }
    Epoch recovery_barrier() const noexcept { return recovery_barrier_; }
    bool recovered_session() const noexcept { return recovered_session_; }
    std::string recovery_detail() const { return recovery_detail_; }
    Instant freshness_reference() const noexcept { return freshness_reference_; }

    const std::vector<SourceState>& sources() const noexcept { return sources_; }
    const std::vector<MeasurementState>& measurements() const noexcept { return measurements_; }
    const std::vector<AllocationState>& allocations() const noexcept { return allocations_; }
    const std::vector<ResidualState>& residuals() const noexcept { return residuals_; }
    const std::vector<SealState>& seals() const noexcept { return seals_; }
    const std::vector<ViewIssue>& issues() const noexcept { return issues_; }
    const std::vector<IdempotencyEntry>& idempotency_index() const noexcept { return idempotency_; }

    const SourceState* find_source(const SourceId& id) const;
    const MeasurementState* find_measurement(const EntryId& id) const;
    const AllocationState* find_allocation(const AllocationId& id) const;
    const ResidualState* find_residual(const ResidualId& id) const;
    const IdempotencyEntry* find_idempotency(const IdempotencyKey& key) const;

    bool is_retired(const std::string& identifier) const;

private:
    // Allocates the per-entry classification total for a newly applied entry.
    void seed_entry(MeasurementState& measurement) const;

    LedgerId ledger_{};
    Revision revision_{};
    Epoch epoch_{};
    Digest chain_head_{};
    Epoch recovery_barrier_{};
    bool recovered_session_{false};
    std::string recovery_detail_{};
    Instant freshness_reference_{};
    std::vector<SourceState> sources_{};
    std::vector<MeasurementState> measurements_{};
    std::vector<AllocationState> allocations_{};
    std::vector<ResidualState> residuals_{};
    std::vector<SealState> seals_{};
    std::vector<ViewIssue> issues_{};
    std::vector<IdempotencyEntry> idempotency_{};
    std::size_t structural_issue_count_{0};
    std::unordered_map<std::string, std::size_t> source_index_{};
    std::unordered_map<std::string, std::size_t> measurement_index_{};
    std::unordered_map<std::string, std::size_t> allocation_index_{};
    std::unordered_map<std::string, std::size_t> residual_index_{};
    std::unordered_map<std::string, std::size_t> idempotency_index_{};
    std::unordered_map<std::string, std::string> retired_{};
};

}  // namespace fel
