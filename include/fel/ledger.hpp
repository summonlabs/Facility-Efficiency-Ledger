#pragma once

// Command surface of the ledger.
//
// Every command validates against the live index, then commits exactly one
// journal record. A command never mutates state without a durable record of the
// decision, the caller's request identity, and the evidence it relied on.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "fel/digest.hpp"
#include "fel/ids.hpp"
#include "fel/journal.hpp"
#include "fel/quantity.hpp"
#include "fel/reconcile.hpp"
#include "fel/store.hpp"
#include "fel/time.hpp"
#include "fel/view.hpp"

namespace fel {

struct CommandOutcome {
    bool applied{false};
    bool replayed{false};
    ReasonCode code{ReasonCode::Ok};
    std::string detail{};
    std::string outcome_id{};
    RecordType record_type{RecordType::SegmentHeader};
    Revision revision{};
    Sequence sequence{};
    Epoch epoch{};
    Digest record_digest{};
    Digest chain_head{};
};

// Request identity and staleness guards. expected_epoch and expected_revision
// are optional: zero means "no expectation". A non-zero value that no longer
// matches the ledger is refused rather than applied optimistically.
struct CommandContext {
    IdempotencyKey idempotency{};
    Revision expected_revision{};
    Epoch expected_epoch{};
    Instant now{};
};

struct RegisterSourceRequest {
    CommandContext context{};
    SourceId source{};
    SourceKind kind{SourceKind::Meter};
    Unit unit{Unit::Joule};
    AuthorityId authority{};
    std::string label{};
};

struct RetireSourceRequest {
    CommandContext context{};
    SourceId source{};
    std::string reason{};
};

struct PublishGenerationRequest {
    CommandContext context{};
    SourceId source{};
    Generation generation{};
    Digest evidence{};
    Instant valid_until{};
    std::string method{};
};

struct AttestGenerationRequest {
    CommandContext context{};
    SourceId source{};
    Generation generation{};
    Digest evidence{};
    Instant valid_until{};
    std::string method{};
};

struct RecordMeasurementRequest {
    CommandContext context{};
    EntryId entry{};
    Interval interval{};
    SourceId source{};
    Generation generation{};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    std::string method{};
    bool supersedes{false};
    EntryId supersedes_entry{};
    Digest supersedes_digest{};
    std::string reason{};
};

struct RecordClassificationRequest {
    CommandContext context{};
    AllocationId allocation{};
    EntryId entry{};
    ServiceClass klass{ServiceClass::Useful};
    Quantity quantity{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    std::string method{};
    bool supersedes{false};
    AllocationId supersedes_allocation{};
    Digest supersedes_digest{};
    std::string reason{};
};

struct RecordResidualRequest {
    CommandContext context{};
    ResidualId residual{};
    Interval interval{};
    ServiceClass klass{ServiceClass::Unknown};
    bool quantified{true};
    Quantity quantity{};
    bool bound_to_entry{false};
    EntryId entry{};
    SubjectKind subject_kind{SubjectKind::Unattributed};
    std::string subject{};
    Digest evidence{};
    std::string basis{};
    bool supersedes{false};
    ResidualId supersedes_residual{};
    Digest supersedes_digest{};
    std::string reason{};
};

struct VoidTargetRequest {
    CommandContext context{};
    VoidId void_id{};
    TargetKind target_kind{TargetKind::Measurement};
    std::string target_id{};
    Digest target_digest{};
    std::string reason{};
};

struct SealIntervalRequest {
    CommandContext context{};
    SealId seal{};
    Interval interval{};
    std::string note{};
};

// Thread-safe command surface.
//
// Lock discipline, in one place:
//   * every public method takes exactly one lock and never takes another;
//   * read paths (view, reconcile) take a shared lock;
//   * write paths (commands, compaction) take an exclusive lock;
//   * no lock is ever upgraded, no lock is held across a call into user code,
//     and nothing is ever invoked under the kernel file lock except the durable
//     append it protects;
//   * helpers called with the lock held never take a lock themselves.
//
// Moving a handle is defined only while the handle is quiescent: the move takes
// the source handle's exclusive lock, so a moved-from handle is left empty and
// must not be used again.
class Ledger {
public:
    Ledger() = default;
    ~Ledger() = default;
    Ledger(Ledger&& other) noexcept;
    Ledger& operator=(Ledger&& other) noexcept;
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    static Result<Ledger> open(const std::filesystem::path& directory, const StoreOptions& options);
    static Result<Ledger> create(const std::filesystem::path& directory, const LedgerId& ledger_id,
                                 const std::string& runtime,
                                 std::function<Instant()> clock = {});

    Result<CommandOutcome> register_source(const RegisterSourceRequest& request);
    Result<CommandOutcome> retire_source(const RetireSourceRequest& request);
    Result<CommandOutcome> publish_generation(const PublishGenerationRequest& request);
    Result<CommandOutcome> attest_generation(const AttestGenerationRequest& request);
    Result<CommandOutcome> record_measurement(const RecordMeasurementRequest& request);
    Result<CommandOutcome> record_classification(const RecordClassificationRequest& request);
    Result<CommandOutcome> record_residual(const RecordResidualRequest& request);
    Result<CommandOutcome> void_target(const VoidTargetRequest& request);
    Result<CommandOutcome> seal_interval(const SealIntervalRequest& request);

    Result<ReconciliationReport> reconcile(const ReconcileRequest& request) const;
    Result<LedgerView> view(const Instant& freshness_reference) const;
    Result<std::uint64_t> compact();

    // Unsynchronised access to the underlying store and live index. These are
    // for single-threaded use and for reporting after quiescing; a caller that
    // reads them while another thread issues commands must supply its own
    // synchronisation.
    const Store& store() const noexcept { return store_; }
    const LedgerView& index() const noexcept { return index_; }
    bool is_writer() const noexcept { return store_.is_writer(); }
    Revision revision() const noexcept { return store_.revision(); }
    Epoch epoch() const noexcept { return store_.epoch(); }
    const RecoveryReport& recovery() const noexcept { return store_.recovery(); }

private:
    enum class Precheck { Proceed, Replay };

    Result<Precheck> precheck(const CommandContext& context, const Digest& request_digest,
                              CommandOutcome& replay) const;

    Result<CommandOutcome> commit(const Record& record, const std::string& outcome_id);

    // Reconciliation without taking the lock. Callers hold either a shared lock
    // (read path) or the exclusive lock (the seal path), never both.
    Result<ReconciliationReport> reconcile_locked(const ReconcileRequest& request) const;

    Store store_{};
    LedgerView index_{};
    mutable std::shared_mutex mutex_{};
};

// Canonical digest of a command request, used for idempotent retry detection.
Digest digest_request(const std::string& command, const std::vector<std::string>& fields);

}  // namespace fel
