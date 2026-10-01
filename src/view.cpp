#include "fel/view.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace fel {
namespace {

std::string outcome_identifier(const Record& record) {
    switch (record.type()) {
        case RecordType::SourceRegistered:
            return record.source_registered().source.str();
        case RecordType::SourceRetired:
            return record.source_retired().source.str();
        case RecordType::GenerationPublished:
            return record.generation_published().source.str();
        case RecordType::GenerationAttested:
            return record.generation_attested().source.str();
        case RecordType::MeasurementRecorded:
            return record.measurement().entry.str();
        case RecordType::ClassificationRecorded:
            return record.classification().allocation.str();
        case RecordType::ResidualRecorded:
            return record.residual().residual.str();
        case RecordType::TargetVoided:
            return record.target_voided().void_id.str();
        case RecordType::IntervalSealed:
            return record.interval_sealed().seal.str();
        case RecordType::SegmentHeader:
        case RecordType::LedgerOpened:
        case RecordType::EpochAdopted:
            return std::string{};
    }
    return std::string{};
}

bool is_live(const MeasurementState& state) noexcept {
    return state.superseded_by.empty() && !state.voided;
}

}  // namespace

std::string_view to_string(FreshnessState state) noexcept {
    switch (state) {
        case FreshnessState::Unattested:
            return "unattested";
        case FreshnessState::Current:
            return "current";
        case FreshnessState::Expired:
            return "expired";
        case FreshnessState::Recovered:
            return "recovered";
        case FreshnessState::Retired:
            return "retired";
    }
    return "unknown";
}

void LedgerView::seed_entry(MeasurementState& measurement) const {
    measurement.allocated = Quantity{Rational{0}, measurement.quantity.unit()};
}

Status LedgerView::apply(const Record& record) {
    // Keeps each entry's classification total current as records arrive. The
    // total is maintained incrementally so that a command can validate an
    // allocation against an entry without rescanning the journal.
    const auto accumulate = [this](const EntryId& entry, const Quantity& quantity,
                                   bool negate) -> Status {
        const auto found = measurement_index_.find(entry.str());
        if (found == measurement_index_.end()) {
            return Status::fail(ReasonCode::UnknownEntry,
                                "entry " + entry.str() + " is not present in the ledger");
        }
        MeasurementState& target = measurements_[found->second];
        const Result<Quantity> converted = quantity.convert_to(target.quantity.unit());
        if (!converted.ok()) {
            return Status{converted.reason()};
        }
        const Result<Quantity> updated = negate ? target.allocated.subtract(converted.value())
                                                : target.allocated.add(converted.value());
        if (!updated.ok()) {
            return Status{updated.reason()};
        }
        target.allocated = updated.value();
        if (!negate) {
            target.allocation_count += 1U;
        } else {
            target.allocation_count = target.allocation_count == 0U ? 0U
                                                                    : target.allocation_count - 1U;
        }
        return Status::ok();
    };

    if (record.revision() > revision_) {
        revision_ = record.revision();
    }
    if (record.epoch() > epoch_) {
        epoch_ = record.epoch();
    }
    if (!record.chain_digest().is_zero()) {
        chain_head_ = record.chain_digest();
    }

    switch (record.type()) {
        case RecordType::SegmentHeader:
            break;
        case RecordType::LedgerOpened:
            ledger_ = record.ledger_opened().ledger_id;
            break;
        case RecordType::EpochAdopted: {
            const EpochAdoptedBody& body = record.epoch_adopted();
            if (body.basis == AdoptionBasis::RecoveredTornTail) {
                recovered_session_ = true;
                if (body.epoch > recovery_barrier_) {
                    recovery_barrier_ = body.epoch;
                }
                recovery_detail_ = body.detail;
            }
            break;
        }
        case RecordType::SourceRegistered: {
            const SourceRegisteredBody& body = record.source_registered();
            const std::string key = body.source.str();
            if (source_index_.count(key) != 0U) {
                issues_.push_back(ViewIssue{ReasonCode::DuplicateEvidence, key,
                                            "source is registered more than once"});
                break;
            }
            SourceState state;
            state.id = body.source;
            state.kind = body.kind;
            state.unit = body.unit;
            state.authority = body.authority;
            state.label = body.label;
            state.registered_at = body.registered_at;
            state.registration_digest = record.content_digest();
            source_index_[key] = sources_.size();
            sources_.push_back(std::move(state));
            break;
        }
        case RecordType::SourceRetired: {
            const SourceRetiredBody& body = record.source_retired();
            const auto found = source_index_.find(body.source.str());
            if (found == source_index_.end()) {
                issues_.push_back(ViewIssue{ReasonCode::UnknownSource, body.source.str(),
                                            "retirement names an unregistered source"});
                break;
            }
            SourceState& state = sources_[found->second];
            state.retired = true;
            state.retired_at = body.retired_at;
            state.retirement_reason = body.reason;
            state.retired_by = record.content_digest().to_hex();
            break;
        }
        case RecordType::GenerationPublished:
        case RecordType::GenerationAttested: {
            const bool attested = record.type() == RecordType::GenerationAttested;
            const SourceId source = attested ? record.generation_attested().source
                                             : record.generation_published().source;
            const Generation generation = attested ? record.generation_attested().generation
                                                   : record.generation_published().generation;
            const auto found = source_index_.find(source.str());
            if (found == source_index_.end()) {
                issues_.push_back(ViewIssue{ReasonCode::UnknownSource, source.str(),
                                            "generation names an unregistered source"});
                break;
            }
            SourceState& state = sources_[found->second];
            if (attested) {
                const GenerationAttestedBody& body = record.generation_attested();
                if (!state.has_generation || state.generation != body.generation) {
                    issues_.push_back(ViewIssue{
                        ReasonCode::StaleGeneration, source.str(),
                        "attestation names a generation that is not the recorded one"});
                    break;
                }
                state.generation_evidence = body.evidence;
                state.generation_valid_until = body.valid_until;
                state.generation_epoch = record.epoch();
                state.generation_attested = true;
            } else {
                const GenerationPublishedBody& body = record.generation_published();
                if (state.has_generation && generation <= state.generation) {
                    issues_.push_back(ViewIssue{
                        ReasonCode::GenerationRegressed, source.str(),
                        "published generation does not advance the source sequence"});
                    break;
                }
                state.has_generation = true;
                state.generation = generation;
                state.generation_evidence = body.evidence;
                state.generation_published_at = body.published_at;
                state.generation_valid_until = body.valid_until;
                state.generation_epoch = record.epoch();
                state.generation_attested = false;
            }
            break;
        }
        case RecordType::MeasurementRecorded: {
            const MeasurementRecordedBody& body = record.measurement();
            const std::string key = body.entry.str();
            if (measurement_index_.count(key) != 0U) {
                issues_.push_back(ViewIssue{ReasonCode::DuplicateEvidence, key,
                                            "measurement entry is recorded more than once"});
                break;
            }
            MeasurementState state;
            state.id = body.entry;
            state.interval = body.interval;
            state.source = body.source;
            state.generation = body.generation;
            state.quantity = body.quantity;
            state.subject_kind = body.subject_kind;
            state.subject = body.subject;
            state.evidence = body.evidence;
            state.recorded_at = body.recorded_at;
            state.method = body.method;
            state.revision = record.revision();
            state.epoch = record.epoch();
            state.record_digest = record.content_digest();
            seed_entry(state);
            const auto superseded = retired_.find(key);
            if (superseded != retired_.end()) {
                state.superseded_by = superseded->second;
            }
            measurement_index_[key] = measurements_.size();
            measurements_.push_back(std::move(state));
            const auto source = source_index_.find(body.source.str());
            if (source != source_index_.end()) {
                sources_[source->second].measurement_count += 1U;
            }
            break;
        }
        case RecordType::ClassificationRecorded: {
            const ClassificationRecordedBody& body = record.classification();
            const std::string key = body.allocation.str();
            if (allocation_index_.count(key) != 0U) {
                issues_.push_back(ViewIssue{ReasonCode::DuplicateEvidence, key,
                                            "classification is recorded more than once"});
                break;
            }
            AllocationState state;
            state.id = body.allocation;
            state.entry = body.entry;
            state.klass = body.klass;
            state.quantity = body.quantity;
            state.subject_kind = body.subject_kind;
            state.subject = body.subject;
            state.evidence = body.evidence;
            state.recorded_at = body.recorded_at;
            state.method = body.method;
            state.revision = record.revision();
            state.epoch = record.epoch();
            state.record_digest = record.content_digest();
            const auto superseded = retired_.find(key);
            if (superseded != retired_.end()) {
                state.superseded_by = superseded->second;
            }
            allocation_index_[key] = allocations_.size();
            allocations_.push_back(std::move(state));
            if (allocations_.back().superseded_by.empty() && !allocations_.back().voided) {
                const Status linked =
                    accumulate(allocations_.back().entry, allocations_.back().quantity, false);
                if (!linked.is_ok()) {
                    issues_.push_back(ViewIssue{linked.code(), allocations_.back().id.str(),
                                                "classification cannot be counted against its "
                                                "entry: " + linked.message()});
                }
            }
            break;
        }
        case RecordType::ResidualRecorded: {
            const ResidualRecordedBody& body = record.residual();
            const std::string key = body.residual.str();
            if (residual_index_.count(key) != 0U) {
                issues_.push_back(ViewIssue{ReasonCode::DuplicateEvidence, key,
                                            "residual is recorded more than once"});
                break;
            }
            ResidualState state;
            state.id = body.residual;
            state.interval = body.interval;
            state.klass = body.klass;
            state.quantified = body.quantified;
            state.quantity = body.quantity;
            state.bound_to_entry = body.bound_to_entry;
            state.entry = body.entry;
            state.subject_kind = body.subject_kind;
            state.subject = body.subject;
            state.evidence = body.evidence;
            state.recorded_at = body.recorded_at;
            state.basis = body.basis;
            state.revision = record.revision();
            state.epoch = record.epoch();
            state.record_digest = record.content_digest();
            const auto superseded = retired_.find(key);
            if (superseded != retired_.end()) {
                state.superseded_by = superseded->second;
            }
            residual_index_[key] = residuals_.size();
            residuals_.push_back(std::move(state));
            if (residuals_.back().bound_to_entry && residuals_.back().quantified &&
                residuals_.back().superseded_by.empty() && !residuals_.back().voided) {
                const Status linked =
                    accumulate(residuals_.back().entry, residuals_.back().quantity, false);
                if (!linked.is_ok()) {
                    issues_.push_back(ViewIssue{linked.code(), residuals_.back().id.str(),
                                                "residual cannot be counted against its entry: " +
                                                    linked.message()});
                }
            }
            break;
        }
        case RecordType::TargetVoided: {
            const TargetVoidedBody& body = record.target_voided();
            switch (body.target_kind) {
                case TargetKind::Measurement: {
                    const auto found = measurement_index_.find(body.target_id);
                    if (found == measurement_index_.end()) {
                        issues_.push_back(ViewIssue{ReasonCode::UnknownEntry, body.target_id,
                                                    "void names an unknown measurement"});
                        break;
                    }
                    MeasurementState& state = measurements_[found->second];
                    state.voided = true;
                    state.voided_by = body.void_id.str();
                    state.void_reason = body.reason;
                    break;
                }
                case TargetKind::Classification: {
                    const auto found = allocation_index_.find(body.target_id);
                    if (found == allocation_index_.end()) {
                        issues_.push_back(ViewIssue{ReasonCode::UnknownAllocation, body.target_id,
                                                    "void names an unknown classification"});
                        break;
                    }
                    AllocationState& state = allocations_[found->second];
                    state.voided = true;
                    state.voided_by = body.void_id.str();
                    state.void_reason = body.reason;
                    const EntryId entry = state.entry;
                    const Quantity quantity = state.quantity;
                    (void)accumulate(entry, quantity, true);
                    break;
                }
                case TargetKind::Residual: {
                    const auto found = residual_index_.find(body.target_id);
                    if (found == residual_index_.end()) {
                        issues_.push_back(ViewIssue{ReasonCode::UnknownResidual, body.target_id,
                                                    "void names an unknown residual"});
                        break;
                    }
                    ResidualState& state = residuals_[found->second];
                    state.voided = true;
                    state.voided_by = body.void_id.str();
                    state.void_reason = body.reason;
                    if (state.bound_to_entry && state.quantified) {
                        const EntryId entry = state.entry;
                        const Quantity quantity = state.quantity;
                        (void)accumulate(entry, quantity, true);
                    }
                    break;
                }
                case TargetKind::SourceRegistration: {
                    const auto found = source_index_.find(body.target_id);
                    if (found == source_index_.end()) {
                        issues_.push_back(ViewIssue{ReasonCode::UnknownSource, body.target_id,
                                                    "void names an unknown source"});
                        break;
                    }
                    SourceState& state = sources_[found->second];
                    state.retired = true;
                    state.retired_at = body.voided_at;
                    state.retirement_reason = body.reason;
                    state.retired_by = record.content_digest().to_hex();
                    break;
                }
            }
            break;
        }
        case RecordType::IntervalSealed: {
            const IntervalSealedBody& body = record.interval_sealed();
            SealState state;
            state.id = body.seal;
            state.interval = body.interval;
            state.revision_at_seal = body.revision_at_seal;
            state.closed = body.closed;
            state.report_digest = body.report_digest;
            state.sealed_at = body.sealed_at;
            state.note = body.note;
            state.record_digest = record.content_digest();
            state.seal_revision = record.revision();
            seals_.push_back(std::move(state));
            break;
        }
    }

    // Retirements recorded by this record, applied to state that is already
    // present. Ordering cannot change the outcome because a superseding record
    // always names a record the ledger has already accepted.
    if (record.meta().supersedes) {
        const std::string& target = record.meta().superseded_id;
        const std::string reference = record.content_digest().to_hex();
        retired_[target] = reference;
        const auto measurement = measurement_index_.find(target);
        if (measurement != measurement_index_.end()) {
            measurements_[measurement->second].superseded_by = reference;
        }
        const auto allocation = allocation_index_.find(target);
        if (allocation != allocation_index_.end()) {
            AllocationState& state = allocations_[allocation->second];
            if (state.superseded_by.empty() && !state.voided) {
                const EntryId entry = state.entry;
                const Quantity quantity = state.quantity;
                (void)accumulate(entry, quantity, true);
            }
            state.superseded_by = reference;
        }
        const auto residual = residual_index_.find(target);
        if (residual != residual_index_.end()) {
            ResidualState& state = residuals_[residual->second];
            if (state.bound_to_entry && state.quantified && state.superseded_by.empty() &&
                !state.voided) {
                const EntryId entry = state.entry;
                const Quantity quantity = state.quantity;
                (void)accumulate(entry, quantity, true);
            }
            state.superseded_by = reference;
        }
    }
    if (record.type() == RecordType::TargetVoided) {
        retired_[record.target_voided().target_id] = record.target_voided().void_id.str();
    }

    if (record.meta().idempotency.valid()) {
        const std::string key = record.meta().idempotency.str();
        if (idempotency_index_.count(key) != 0U) {
            issues_.push_back(ViewIssue{ReasonCode::DuplicateIdempotencyKey, key,
                                        "idempotency key is used by more than one record"});
        } else {
            IdempotencyEntry entry;
            entry.key = record.meta().idempotency;
            entry.request_digest = record.meta().request_digest;
            entry.type = record.type();
            entry.outcome_id = outcome_identifier(record);
            entry.record_digest = record.content_digest();
            entry.revision = record.revision();
            entry.sequence = record.sequence();
            idempotency_index_[key] = idempotency_.size();
            idempotency_.push_back(std::move(entry));
        }
    }
    return Status::ok();
}

void LedgerView::analyze() {
    issues_.resize(structural_issue_count_);

    for (const AllocationState& allocation : allocations_) {
        if (allocation.superseded_by.empty() && !allocation.voided &&
            measurement_index_.count(allocation.entry.str()) == 0U) {
            issues_.push_back(ViewIssue{ReasonCode::UnknownEntry, allocation.id.str(),
                                        "classification names unknown entry " +
                                            allocation.entry.str()});
        }
    }
    for (const ResidualState& residual : residuals_) {
        if (residual.bound_to_entry && residual.superseded_by.empty() && !residual.voided &&
            measurement_index_.count(residual.entry.str()) == 0U) {
            issues_.push_back(ViewIssue{ReasonCode::UnknownEntry, residual.id.str(),
                                        "residual names unknown entry " + residual.entry.str()});
        }
    }

    for (std::size_t index = 0; index < measurements_.size(); ++index) {
        const MeasurementState& measurement = measurements_[index];
        if (!is_live(measurement)) {
            continue;
        }
        const Result<int> order = measurement.allocated.compare(measurement.quantity);
        if (order.ok() && order.value() > 0) {
            issues_.push_back(ViewIssue{
                ReasonCode::OverAllocation, measurement.id.str(),
                "classifications total " + measurement.allocated.to_string() +
                    " which exceeds the measured " + measurement.quantity.to_string()});
        }
    }

    for (std::size_t a = 0; a < measurements_.size(); ++a) {
        if (!is_live(measurements_[a])) {
            continue;
        }
        for (std::size_t b = a + 1; b < measurements_.size(); ++b) {
            if (!is_live(measurements_[b])) {
                continue;
            }
            const MeasurementState& left = measurements_[a];
            const MeasurementState& right = measurements_[b];
            if (left.source != right.source) {
                continue;
            }
            if (left.interval.overlaps(right.interval)) {
                issues_.push_back(ViewIssue{
                    ReasonCode::IntervalOverlap, left.id.str(),
                    "measurement interval overlaps " + right.id.str() + " from the same source"});
            }
            if (left.interval == right.interval && left.evidence == right.evidence &&
                left.quantity == right.quantity && left.generation == right.generation) {
                issues_.push_back(ViewIssue{
                    ReasonCode::DuplicateEvidence, left.id.str(),
                    "measurement duplicates the observation recorded as " + right.id.str()});
            }
        }
    }

    structural_issue_count_ = issues_.size();
}

void LedgerView::finalize(const Instant& freshness_reference) {
    freshness_reference_ = freshness_reference;
    for (SourceState& source : sources_) {
        if (source.retired) {
            source.freshness = FreshnessState::Retired;
            continue;
        }
        if (!source.has_generation) {
            source.freshness = FreshnessState::Unattested;
            continue;
        }
        if (source.generation_epoch < recovery_barrier_) {
            source.freshness = FreshnessState::Recovered;
            continue;
        }
        const bool no_expiry =
            source.generation_valid_until.seconds() == 0 && source.generation_valid_until.nanos() == 0;
        if (!no_expiry && source.generation_valid_until < freshness_reference) {
            source.freshness = FreshnessState::Expired;
            continue;
        }
        source.freshness = FreshnessState::Current;
    }
}

Result<LedgerView> LedgerView::fold(const std::vector<Record>& records,
                                    const Instant& freshness_reference) {
    LedgerView view;
    for (const Record& record : records) {
        const Status applied = view.apply(record);
        if (!applied.is_ok()) {
            return applied.reason();
        }
    }
    view.analyze();
    view.finalize(freshness_reference);
    return view;
}

const SourceState* LedgerView::find_source(const SourceId& id) const {
    const auto found = source_index_.find(id.str());
    return found == source_index_.end() ? nullptr : &sources_[found->second];
}

const MeasurementState* LedgerView::find_measurement(const EntryId& id) const {
    const auto found = measurement_index_.find(id.str());
    return found == measurement_index_.end() ? nullptr : &measurements_[found->second];
}

const AllocationState* LedgerView::find_allocation(const AllocationId& id) const {
    const auto found = allocation_index_.find(id.str());
    return found == allocation_index_.end() ? nullptr : &allocations_[found->second];
}

const ResidualState* LedgerView::find_residual(const ResidualId& id) const {
    const auto found = residual_index_.find(id.str());
    return found == residual_index_.end() ? nullptr : &residuals_[found->second];
}

const IdempotencyEntry* LedgerView::find_idempotency(const IdempotencyKey& key) const {
    const auto found = idempotency_index_.find(key.str());
    return found == idempotency_index_.end() ? nullptr : &idempotency_[found->second];
}

bool LedgerView::is_retired(const std::string& identifier) const {
    return retired_.count(identifier) != 0U;
}

}  // namespace fel
