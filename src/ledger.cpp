#include "fel/ledger.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "fel/codec.hpp"
#include "fel/report.hpp"

namespace fel {
namespace {

std::string encode_bool(bool value) { return value ? std::string{"true"} : std::string{"false"}; }

std::string encode_counter(std::uint64_t value) { return std::to_string(value); }

std::string encode_quantity(const Quantity& value) { return value.to_string(); }

std::string encode_instant(const Instant& value) { return value.to_iso8601(); }

std::string encode_digest(const Digest& value) { return value.to_hex(); }

bool unit_is_registered(Unit unit) noexcept {
    const std::optional<Unit> round_trip = unit_from_code(unit_code(unit));
    return round_trip.has_value() && round_trip.value() == unit;
}

MutationMeta build_meta(const CommandContext& context, const Digest& request_digest,
                        Epoch session_epoch) {
    MutationMeta meta;
    meta.idempotency = context.idempotency;
    meta.request_digest = request_digest;
    meta.caller_epoch = context.expected_epoch.value() == 0 ? session_epoch : context.expected_epoch;
    meta.expected_revision = context.expected_revision;
    return meta;
}

}  // namespace

Digest digest_request(const std::string& command, const std::vector<std::string>& fields) {
    Writer writer;
    writer.string(command);
    writer.u32(static_cast<std::uint32_t>(fields.size()));
    for (const std::string& field : fields) {
        writer.string(field);
    }
    const std::vector<std::uint8_t>& bytes = writer.bytes();
    return Sha256::hash(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
}

Ledger::Ledger(Ledger&& other) noexcept {
    std::unique_lock<std::shared_mutex> guard(other.mutex_);
    store_ = std::move(other.store_);
    index_ = std::move(other.index_);
}

Ledger& Ledger::operator=(Ledger&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock<std::shared_mutex, std::shared_mutex> guard(mutex_, other.mutex_);
    store_ = std::move(other.store_);
    index_ = std::move(other.index_);
    return *this;
}
Result<Ledger> Ledger::open(const std::filesystem::path& directory, const StoreOptions& options) {
    Result<Store> store = Store::open(directory, options);
    if (!store.ok()) {
        return store.reason();
    }
    Ledger ledger;
    ledger.store_ = store.take();
    Result<LedgerView> index = LedgerView::fold(ledger.store_.records(), Instant{});
    if (!index.ok()) {
        return index.reason();
    }
    ledger.index_ = index.take();
    return ledger;
}

Result<Ledger> Ledger::create(const std::filesystem::path& directory, const LedgerId& ledger_id,
                              const std::string& runtime, std::function<Instant()> clock) {
    StoreOptions options;
    options.mode = OpenMode::Writer;
    options.create_if_missing = true;
    options.ledger_id = ledger_id;
    options.runtime = runtime.empty() ? std::string{kRuntimeIdentity} : runtime;
    options.clock = std::move(clock);
    return Ledger::open(directory, options);
}

Result<Ledger::Precheck> Ledger::precheck(const CommandContext& context,
                                          const Digest& request_digest,
                                          CommandOutcome& replay) const {
    if (!store_.is_writer()) {
        return Reason{ReasonCode::AuthorityNotOwned,
                      "this ledger handle did not acquire the ledger write lock"};
    }
    if (context.expected_epoch.value() != 0 && context.expected_epoch.value() != store_.epoch().value()) {
        return Reason{ReasonCode::StaleEpoch,
                      "request cites epoch " + std::to_string(context.expected_epoch.value()) +
                          " but the ledger is at epoch " + std::to_string(store_.epoch().value())};
    }
    if (context.expected_revision.value() != 0 &&
        context.expected_revision.value() != store_.revision().value()) {
        return Reason{ReasonCode::StaleRevision,
                      "request expects revision " + std::to_string(context.expected_revision.value()) +
                          " but the ledger is at revision " +
                          std::to_string(store_.revision().value())};
    }
    if (context.idempotency.valid()) {
        const IdempotencyEntry* prior = index_.find_idempotency(context.idempotency);
        if (prior != nullptr) {
            if (prior->request_digest != request_digest) {
                return Reason{ReasonCode::IdempotencyConflict,
                              "idempotency key " + context.idempotency.str() +
                                  " was already used by a different request"};
            }
            replay.applied = false;
            replay.replayed = true;
            replay.code = ReasonCode::Ok;
            replay.detail = "identical request was already committed at revision " +
                            std::to_string(prior->revision.value());
            replay.outcome_id = prior->outcome_id;
            replay.record_type = prior->type;
            replay.revision = prior->revision;
            replay.sequence = prior->sequence;
            replay.record_digest = prior->record_digest;
            replay.chain_head = store_.chain_head();
            replay.epoch = store_.epoch();
            return Precheck::Replay;
        }
    }
    return Precheck::Proceed;
}

Result<CommandOutcome> Ledger::commit(const Record& record, const std::string& outcome_id) {
    const Result<Record> appended = store_.append(record);
    if (!appended.ok()) {
        return appended.reason();
    }
    const Status applied = index_.apply(appended.value());
    if (!applied.is_ok()) {
        return applied.reason();
    }
    CommandOutcome outcome;
    outcome.applied = true;
    outcome.replayed = false;
    outcome.code = ReasonCode::Ok;
    outcome.detail = "committed";
    outcome.outcome_id = outcome_id;
    outcome.record_type = appended.value().type();
    outcome.revision = appended.value().revision();
    outcome.sequence = appended.value().sequence();
    outcome.epoch = appended.value().epoch();
    outcome.record_digest = appended.value().content_digest();
    outcome.chain_head = store_.chain_head();
    return outcome;
}

Result<CommandOutcome> Ledger::register_source(const RegisterSourceRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.source.str(), std::string(to_string(request.kind)),
                                          std::string(unit_symbol(request.unit)),
                                          request.authority.str(), request.label};
    const Digest request_digest = digest_request("register_source", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.source.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a source identifier is required"};
    }
    if (!unit_is_registered(request.unit)) {
        return Reason{ReasonCode::UnknownUnit, "the requested unit is not registered"};
    }
    if (request.label.size() > 256U) {
        return Reason{ReasonCode::BoundsExceeded, "source label exceeds 256 bytes"};
    }
    if (index_.find_source(request.source) != nullptr) {
        return Reason{ReasonCode::DuplicateSource,
                      "source " + request.source.str() + " is already registered"};
    }
    SourceRegisteredBody body;
    body.source = request.source;
    body.kind = request.kind;
    body.unit = request.unit;
    body.authority = request.authority;
    body.label = request.label;
    body.registered_at = request.context.now;
    return commit(Record::make_source_registered(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.source.str());
}

Result<CommandOutcome> Ledger::retire_source(const RetireSourceRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.source.str(), request.reason};
    const Digest request_digest = digest_request("retire_source", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.source.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a source identifier is required"};
    }
    const SourceState* source = index_.find_source(request.source);
    if (source == nullptr) {
        return Reason{ReasonCode::UnknownSource, "source " + request.source.str() + " is not registered"};
    }
    if (source->retired) {
        return Reason{ReasonCode::SourceRetired, "source " + request.source.str() + " is already retired"};
    }
    if (request.reason.empty()) {
        return Reason{ReasonCode::InvalidArgument, "retiring a source requires a reason"};
    }
    SourceRetiredBody body;
    body.source = request.source;
    body.retired_at = request.context.now;
    body.reason = request.reason;
    body.source_digest = source->registration_digest;
    return commit(Record::make_source_retired(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.source.str());
}

Result<CommandOutcome> Ledger::publish_generation(const PublishGenerationRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.source.str(), encode_counter(request.generation.value()),
                                          encode_digest(request.evidence),
                                          encode_instant(request.valid_until), request.method};
    const Digest request_digest = digest_request("publish_generation", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    const SourceState* source = index_.find_source(request.source);
    if (source == nullptr) {
        return Reason{ReasonCode::UnknownSource, "source " + request.source.str() + " is not registered"};
    }
    if (source->retired) {
        return Reason{ReasonCode::SourceRetired, "source " + request.source.str() + " is retired"};
    }
    if (request.generation.is_zero()) {
        return Reason{ReasonCode::InvalidArgument, "generations are 1-based"};
    }
    if (source->has_generation && request.generation <= source->generation) {
        return Reason{ReasonCode::GenerationRegressed,
                      "generation " + std::to_string(request.generation.value()) +
                          " does not advance the current generation " +
                          std::to_string(source->generation.value())};
    }
    if (request.evidence.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "publishing a generation requires the digest of the evidence it rests on"};
    }
    GenerationPublishedBody body;
    body.source = request.source;
    body.generation = request.generation;
    body.published_at = request.context.now;
    body.evidence = request.evidence;
    body.valid_until = request.valid_until;
    body.method = request.method;
    return commit(Record::make_generation_published(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.source.str());
}

Result<CommandOutcome> Ledger::attest_generation(const AttestGenerationRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.source.str(), encode_counter(request.generation.value()),
                                          encode_digest(request.evidence),
                                          encode_instant(request.valid_until), request.method};
    const Digest request_digest = digest_request("attest_generation", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    const SourceState* source = index_.find_source(request.source);
    if (source == nullptr) {
        return Reason{ReasonCode::UnknownSource, "source " + request.source.str() + " is not registered"};
    }
    if (!source->has_generation) {
        return Reason{ReasonCode::UnknownEntry,
                      "no generation has been published for source " + request.source.str()};
    }
    if (source->generation != request.generation) {
        return Reason{ReasonCode::StaleGeneration,
                      "attestation names generation " + std::to_string(request.generation.value()) +
                          " but the recorded generation is " +
                          std::to_string(source->generation.value())};
    }
    if (request.evidence.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "attesting a generation requires the digest of fresh evidence"};
    }
    GenerationAttestedBody body;
    body.source = request.source;
    body.generation = request.generation;
    body.attested_at = request.context.now;
    body.evidence = request.evidence;
    body.valid_until = request.valid_until;
    body.method = request.method;
    return commit(Record::make_generation_attested(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.source.str());
}

Result<CommandOutcome> Ledger::record_measurement(const RecordMeasurementRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{
        request.entry.str(),
        request.interval.to_string(),
        request.source.str(),
        encode_counter(request.generation.value()),
        encode_quantity(request.quantity),
        std::string(to_string(request.subject_kind)),
        request.subject,
        encode_digest(request.evidence),
        request.method,
        encode_bool(request.supersedes),
        request.supersedes_entry.str(),
        encode_digest(request.supersedes_digest),
        request.reason};
    const Digest request_digest = digest_request("record_measurement", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.entry.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a measurement entry identifier is required"};
    }
    if (index_.find_measurement(request.entry) != nullptr) {
        return Reason{ReasonCode::DuplicateEvidence,
                      "entry " + request.entry.str() + " already exists"};
    }
    const SourceState* source = index_.find_source(request.source);
    if (source == nullptr) {
        return Reason{ReasonCode::UnknownSource, "source " + request.source.str() + " is not registered"};
    }
    if (source->retired) {
        return Reason{ReasonCode::SourceRetired, "source " + request.source.str() + " is retired"};
    }
    if (!source->has_generation) {
        return Reason{ReasonCode::StaleGeneration,
                      "source " + request.source.str() + " has no published generation"};
    }
    if (request.generation != source->generation) {
        return Reason{ReasonCode::StaleGeneration,
                      "measurement cites generation " + std::to_string(request.generation.value()) +
                          " but the source is at generation " +
                          std::to_string(source->generation.value())};
    }
    if (!unit_is_registered(request.quantity.unit())) {
        return Reason{ReasonCode::UnknownUnit,
                      "the measurement unit is not registered by this runtime"};
    }
    if (request.quantity.is_negative() || request.quantity.is_zero()) {
        return Reason{ReasonCode::NonPositiveQuantity,
                      "a measurement must be strictly positive; it is " +
                          request.quantity.to_string()};
    }
    if (dimension_of(request.quantity.unit()) != dimension_of(source->unit)) {
        return Reason{ReasonCode::DimensionMismatch,
                      "measurement unit " + std::string(unit_symbol(request.quantity.unit())) +
                          " is not in the dimension registered for source " + request.source.str()};
    }
    if (request.evidence.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "recording a measurement requires the digest of the evidence it rests on"};
    }
    if (request.subject_kind == SubjectKind::Unattributed) {
        if (!request.subject.empty()) {
            return Reason{ReasonCode::InvalidArgument,
                          "an unattributed measurement cannot name a subject"};
        }
    } else if (!is_valid_identifier_text(request.subject)) {
        return Reason{ReasonCode::InvalidIdentifier,
                      "attributed subjects must be identifiers, got \"" + request.subject + "\""};
    }

    MutationMeta meta = build_meta(request.context, request_digest, store_.epoch());
    if (request.supersedes) {
        if (!request.supersedes_entry.valid()) {
            return Reason{ReasonCode::InvalidArgument, "a correction must name the entry it supersedes"};
        }
        const MeasurementState* target = index_.find_measurement(request.supersedes_entry);
        if (target == nullptr) {
            return Reason{ReasonCode::UnknownEntry,
                          "correction names unknown entry " + request.supersedes_entry.str()};
        }
        if (!target->superseded_by.empty() || target->voided) {
            return Reason{ReasonCode::AlreadySuperseded,
                          "entry " + request.supersedes_entry.str() + " is no longer live"};
        }
        if (request.supersedes_digest != target->record_digest) {
            return Reason{ReasonCode::DigestMismatch,
                          "correction cites digest " + request.supersedes_digest.to_hex() +
                              " but the live record is " + target->record_digest.to_hex()};
        }
        if (request.reason.empty()) {
            return Reason{ReasonCode::InvalidArgument, "a correction requires a reason"};
        }
        meta.supersedes = true;
        meta.superseded_id = request.supersedes_entry.str();
        meta.superseded_digest = request.supersedes_digest;
        meta.reason = request.reason;
    }

    for (const MeasurementState& existing : index_.measurements()) {
        if (existing.source != request.source || !existing.superseded_by.empty() || existing.voided) {
            continue;
        }
        if (request.supersedes && existing.id == request.supersedes_entry) {
            continue;
        }
        if (existing.interval.overlaps(request.interval)) {
            return Reason{ReasonCode::IntervalOverlap,
                          "interval " + request.interval.to_string() + " overlaps live entry " +
                              existing.id.str() + " (" + existing.interval.to_string() +
                              ") from the same source; correct the existing entry instead"};
        }
    }

    MeasurementRecordedBody body;
    body.entry = request.entry;
    body.interval = request.interval;
    body.source = request.source;
    body.generation = request.generation;
    body.quantity = request.quantity;
    body.subject_kind = request.subject_kind;
    body.subject = request.subject;
    body.evidence = request.evidence;
    body.recorded_at = request.context.now;
    body.method = request.method;
    return commit(Record::make_measurement(body, meta), request.entry.str());
}

Result<CommandOutcome> Ledger::record_classification(const RecordClassificationRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{
        request.allocation.str(),
        request.entry.str(),
        std::string(to_string(request.klass)),
        encode_quantity(request.quantity),
        std::string(to_string(request.subject_kind)),
        request.subject,
        encode_digest(request.evidence),
        request.method,
        encode_bool(request.supersedes),
        request.supersedes_allocation.str(),
        encode_digest(request.supersedes_digest),
        request.reason};
    const Digest request_digest = digest_request("record_classification", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.allocation.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a classification identifier is required"};
    }
    if (index_.find_allocation(request.allocation) != nullptr) {
        return Reason{ReasonCode::DuplicateEvidence,
                      "classification " + request.allocation.str() + " already exists"};
    }
    const MeasurementState* entry = index_.find_measurement(request.entry);
    if (entry == nullptr) {
        return Reason{ReasonCode::UnknownEntry,
                      "classification names unknown entry " + request.entry.str()};
    }
    if (!entry->superseded_by.empty() || entry->voided) {
        return Reason{ReasonCode::AlreadySuperseded,
                      "entry " + request.entry.str() + " is not live"};
    }
    if (!unit_is_registered(request.quantity.unit())) {
        return Reason{ReasonCode::UnknownUnit,
                      "the classification unit is not registered by this runtime"};
    }
    if (request.quantity.is_negative() || request.quantity.is_zero()) {
        return Reason{ReasonCode::NonPositiveQuantity,
                      "a classification must be strictly positive"};
    }
    if (request.evidence.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "recording a classification requires the digest of the evidence it rests on"};
    }
    if (request.subject_kind == SubjectKind::Unattributed) {
        if (!request.subject.empty()) {
            return Reason{ReasonCode::InvalidArgument, "an unattributed classification cannot name a subject"};
        }
    } else if (!is_valid_identifier_text(request.subject)) {
        return Reason{ReasonCode::InvalidIdentifier, "attributed subjects must be identifiers"};
    }

    MutationMeta meta = build_meta(request.context, request_digest, store_.epoch());
    Quantity baseline = entry->allocated;
    if (request.supersedes) {
        if (!request.supersedes_allocation.valid()) {
            return Reason{ReasonCode::InvalidArgument,
                          "a correction must name the classification it supersedes"};
        }
        const AllocationState* target = index_.find_allocation(request.supersedes_allocation);
        if (target == nullptr) {
            return Reason{ReasonCode::UnknownAllocation,
                          "correction names unknown classification " +
                              request.supersedes_allocation.str()};
        }
        if (!target->superseded_by.empty() || target->voided) {
            return Reason{ReasonCode::AlreadySuperseded,
                          "classification " + request.supersedes_allocation.str() +
                              " is no longer live"};
        }
        if (request.supersedes_digest != target->record_digest) {
            return Reason{ReasonCode::DigestMismatch,
                          "correction cites a digest that does not match the live record"};
        }
        if (request.reason.empty()) {
            return Reason{ReasonCode::InvalidArgument, "a correction requires a reason"};
        }
        meta.supersedes = true;
        meta.superseded_id = request.supersedes_allocation.str();
        meta.superseded_digest = request.supersedes_digest;
        meta.reason = request.reason;
        if (target->entry == request.entry) {
            const Result<Quantity> converted = target->quantity.convert_to(entry->quantity.unit());
            if (!converted.ok()) {
                return Reason{converted.code(), converted.reason().detail()};
            }
            const Result<Quantity> reduced = baseline.subtract(converted.value());
            if (!reduced.ok()) {
                return Reason{reduced.code(), reduced.reason().detail()};
            }
            baseline = reduced.value();
        }
    }

    const Result<Quantity> converted = request.quantity.convert_to(entry->quantity.unit());
    if (!converted.ok()) {
        return Reason{ReasonCode::DimensionMismatch,
                      "classification unit " + std::string(unit_symbol(request.quantity.unit())) +
                          " is not in the dimension of the measured quantity"};
    }
    const Result<Quantity> projected = baseline.add(converted.value());
    if (!projected.ok()) {
        return Reason{projected.code(), projected.reason().detail()};
    }
    const Result<int> order = projected.value().compare(entry->quantity);
    if (order.ok() && order.value() > 0) {
        return Reason{ReasonCode::OverAllocation,
                      "classifying " + request.quantity.to_string() + " would bring entry " +
                          request.entry.str() + " to " + projected.value().to_string() +
                          " against a measured " + entry->quantity.to_string()};
    }

    ClassificationRecordedBody body;
    body.allocation = request.allocation;
    body.entry = request.entry;
    body.klass = request.klass;
    body.quantity = request.quantity;
    body.subject_kind = request.subject_kind;
    body.subject = request.subject;
    body.evidence = request.evidence;
    body.recorded_at = request.context.now;
    body.method = request.method;
    return commit(Record::make_classification(body, meta), request.allocation.str());
}

Result<CommandOutcome> Ledger::record_residual(const RecordResidualRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{
        request.residual.str(),
        request.interval.to_string(),
        std::string(to_string(request.klass)),
        encode_bool(request.quantified),
        encode_quantity(request.quantity),
        encode_bool(request.bound_to_entry),
        request.entry.str(),
        std::string(to_string(request.subject_kind)),
        request.subject,
        encode_digest(request.evidence),
        request.basis,
        encode_bool(request.supersedes),
        request.supersedes_residual.str(),
        encode_digest(request.supersedes_digest),
        request.reason};
    const Digest request_digest = digest_request("record_residual", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.residual.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a residual identifier is required"};
    }
    if (index_.find_residual(request.residual) != nullptr) {
        return Reason{ReasonCode::DuplicateEvidence,
                      "residual " + request.residual.str() + " already exists"};
    }
    if (!is_residual_class(request.klass)) {
        return Reason{ReasonCode::InvalidArgument,
                      "a residual must be classified unknown or unmeasured, not " +
                          std::string(to_string(request.klass))};
    }
    if (!request.quantified && request.klass != ServiceClass::Unmeasured) {
        return Reason{ReasonCode::InvalidArgument,
                      "only an unmeasured residual may be left unquantified"};
    }
    if (!unit_is_registered(request.quantity.unit())) {
        return Reason{ReasonCode::UnknownUnit,
                      "a residual must name the dimension of the resource it describes"};
    }
    if (request.quantified && (request.quantity.is_zero() || request.quantity.is_negative())) {
        return Reason{ReasonCode::NonPositiveQuantity,
                      "a quantified residual must be strictly positive"};
    }
    if (request.bound_to_entry && !request.quantified) {
        return Reason{ReasonCode::NotQuantified,
                      "a residual bound to a measured entry must be quantified"};
    }
    if (!request.quantified && !request.quantity.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "an unquantified residual must not carry an amount"};
    }
    if (request.evidence.is_zero()) {
        return Reason{ReasonCode::InvalidArgument,
                      "recording a residual requires the digest of the evidence it rests on"};
    }
    if (request.basis.empty()) {
        return Reason{ReasonCode::InvalidArgument,
                      "recording a residual requires a stated basis"};
    }
    if (request.subject_kind == SubjectKind::Unattributed) {
        if (!request.subject.empty()) {
            return Reason{ReasonCode::InvalidArgument, "an unattributed residual cannot name a subject"};
        }
    } else if (!is_valid_identifier_text(request.subject)) {
        return Reason{ReasonCode::InvalidIdentifier, "attributed subjects must be identifiers"};
    }

    const MeasurementState* entry = nullptr;
    Quantity baseline{};
    if (request.bound_to_entry) {
        entry = index_.find_measurement(request.entry);
        if (entry == nullptr) {
            return Reason{ReasonCode::UnknownEntry,
                          "residual names unknown entry " + request.entry.str()};
        }
        if (!entry->superseded_by.empty() || entry->voided) {
            return Reason{ReasonCode::AlreadySuperseded,
                          "entry " + request.entry.str() + " is not live"};
        }
        if (!entry->interval.contains(request.interval)) {
            return Reason{ReasonCode::IntervalNotContained,
                          "bound residual interval " + request.interval.to_string() +
                              " is not contained in the measured interval " +
                              entry->interval.to_string()};
        }
        baseline = entry->allocated;
    }

    MutationMeta meta = build_meta(request.context, request_digest, store_.epoch());
    if (request.supersedes) {
        if (!request.supersedes_residual.valid()) {
            return Reason{ReasonCode::InvalidArgument, "a correction must name the residual it supersedes"};
        }
        const ResidualState* target = index_.find_residual(request.supersedes_residual);
        if (target == nullptr) {
            return Reason{ReasonCode::UnknownResidual,
                          "correction names unknown residual " + request.supersedes_residual.str()};
        }
        if (!target->superseded_by.empty() || target->voided) {
            return Reason{ReasonCode::AlreadySuperseded,
                          "residual " + request.supersedes_residual.str() + " is no longer live"};
        }
        if (request.supersedes_digest != target->record_digest) {
            return Reason{ReasonCode::DigestMismatch,
                          "correction cites a digest that does not match the live record"};
        }
        if (request.reason.empty()) {
            return Reason{ReasonCode::InvalidArgument, "a correction requires a reason"};
        }
        meta.supersedes = true;
        meta.superseded_id = request.supersedes_residual.str();
        meta.superseded_digest = request.supersedes_digest;
        meta.reason = request.reason;
        if (entry != nullptr && target->bound_to_entry && target->quantified &&
            target->entry == request.entry) {
            const Result<Quantity> converted = target->quantity.convert_to(entry->quantity.unit());
            if (!converted.ok()) {
                return Reason{converted.code(), converted.reason().detail()};
            }
            const Result<Quantity> reduced = baseline.subtract(converted.value());
            if (!reduced.ok()) {
                return Reason{reduced.code(), reduced.reason().detail()};
            }
            baseline = reduced.value();
        }
    }

    if (entry != nullptr) {
        const Result<Quantity> converted = request.quantity.convert_to(entry->quantity.unit());
        if (!converted.ok()) {
            return Reason{ReasonCode::DimensionMismatch,
                          "residual unit is not in the dimension of the measured quantity"};
        }
        const Result<Quantity> projected = baseline.add(converted.value());
        if (!projected.ok()) {
            return Reason{projected.code(), projected.reason().detail()};
        }
        const Result<int> order = projected.value().compare(entry->quantity);
        if (order.ok() && order.value() > 0) {
            return Reason{ReasonCode::OverAllocation,
                          "the bound residual would bring entry " + request.entry.str() + " to " +
                              projected.value().to_string() + " against a measured " +
                              entry->quantity.to_string()};
        }
    }

    ResidualRecordedBody body;
    body.residual = request.residual;
    body.interval = request.interval;
    body.klass = request.klass;
    body.quantified = request.quantified;
    body.quantity = request.quantity;
    body.bound_to_entry = request.bound_to_entry;
    body.entry = request.entry;
    body.subject_kind = request.subject_kind;
    body.subject = request.subject;
    body.evidence = request.evidence;
    body.recorded_at = request.context.now;
    body.basis = request.basis;
    return commit(Record::make_residual(body, meta), request.residual.str());
}

Result<CommandOutcome> Ledger::void_target(const VoidTargetRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.void_id.str(),
                                          std::string(to_string(request.target_kind)),
                                          request.target_id, encode_digest(request.target_digest),
                                          request.reason};
    const Digest request_digest = digest_request("void_target", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.void_id.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a void identifier is required"};
    }
    if (request.reason.empty()) {
        return Reason{ReasonCode::InvalidArgument, "voiding a record requires a reason"};
    }

    Digest live_digest{};
    switch (request.target_kind) {
        case TargetKind::Measurement: {
            const Result<EntryId> parsed = EntryId::parse(request.target_id);
            const MeasurementState* target =
                parsed.ok() ? index_.find_measurement(parsed.value()) : nullptr;
            if (target == nullptr) {
                return Reason{ReasonCode::UnknownEntry,
                              "void names unknown measurement " + request.target_id};
            }
            if (!target->superseded_by.empty() || target->voided) {
                return Reason{ReasonCode::AlreadySuperseded,
                              "measurement " + request.target_id + " is no longer live"};
            }
            live_digest = target->record_digest;
            break;
        }
        case TargetKind::Classification: {
            const Result<AllocationId> id = AllocationId::parse(request.target_id);
            const AllocationState* target =
                id.ok() ? index_.find_allocation(id.value()) : nullptr;
            if (target == nullptr) {
                return Reason{ReasonCode::UnknownAllocation,
                              "void names unknown classification " + request.target_id};
            }
            if (!target->superseded_by.empty() || target->voided) {
                return Reason{ReasonCode::AlreadySuperseded,
                              "classification " + request.target_id + " is no longer live"};
            }
            live_digest = target->record_digest;
            break;
        }
        case TargetKind::Residual: {
            const Result<ResidualId> id = ResidualId::parse(request.target_id);
            const ResidualState* target = id.ok() ? index_.find_residual(id.value()) : nullptr;
            if (target == nullptr) {
                return Reason{ReasonCode::UnknownResidual,
                              "void names unknown residual " + request.target_id};
            }
            if (!target->superseded_by.empty() || target->voided) {
                return Reason{ReasonCode::AlreadySuperseded,
                              "residual " + request.target_id + " is no longer live"};
            }
            live_digest = target->record_digest;
            break;
        }
        case TargetKind::SourceRegistration: {
            const Result<SourceId> id = SourceId::parse(request.target_id);
            const SourceState* target = id.ok() ? index_.find_source(id.value()) : nullptr;
            if (target == nullptr) {
                return Reason{ReasonCode::UnknownSource,
                              "void names unknown source " + request.target_id};
            }
            if (target->retired) {
                return Reason{ReasonCode::SourceRetired,
                              "source " + request.target_id + " is already retired"};
            }
            live_digest = target->registration_digest;
            break;
        }
    }
    if (request.target_digest != live_digest) {
        return Reason{ReasonCode::DigestMismatch,
                      "void cites digest " + request.target_digest.to_hex() +
                          " but the live record is " + live_digest.to_hex()};
    }

    TargetVoidedBody body;
    body.void_id = request.void_id;
    body.target_kind = request.target_kind;
    body.target_id = request.target_id;
    body.target_digest = request.target_digest;
    body.voided_at = request.context.now;
    body.reason = request.reason;
    return commit(Record::make_target_voided(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.void_id.str());
}

Result<CommandOutcome> Ledger::seal_interval(const SealIntervalRequest& request) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const std::vector<std::string> fields{request.seal.str(), request.interval.to_string(),
                                          request.note};
    const Digest request_digest = digest_request("seal_interval", fields);
    CommandOutcome replay;
    const Result<Precheck> pre = precheck(request.context, request_digest, replay);
    if (!pre.ok()) {
        return pre.reason();
    }
    if (pre.value() == Precheck::Replay) {
        return replay;
    }
    if (!request.seal.valid()) {
        return Reason{ReasonCode::InvalidIdentifier, "a seal identifier is required"};
    }
    ReconcileRequest reconcile_request;
    reconcile_request.interval = request.interval;
    reconcile_request.as_of = request.context.now;
    reconcile_request.basis = BasisPolicy::Historical;
    const Result<ReconciliationReport> report = reconcile_locked(reconcile_request);
    if (!report.ok()) {
        return report.reason();
    }
    if (!report.value().closed) {
        return Reason{ReasonCode::AccountingNotClosed,
                      "the interval does not reconcile exactly: " + report.value().explanation};
    }
    for (const SealState& existing : index_.seals()) {
        if (existing.interval == request.interval) {
            return Reason{ReasonCode::AlreadyExists,
                          "interval " + request.interval.to_string() +
                              " is already sealed by " + existing.id.str()};
        }
    }
    IntervalSealedBody body;
    body.seal = request.seal;
    body.interval = request.interval;
    body.revision_at_seal = store_.revision();
    body.closed = true;
    body.report_digest = report.value().report_digest;
    body.sealed_at = request.context.now;
    body.note = request.note;
    return commit(Record::make_interval_sealed(
                      body, build_meta(request.context, request_digest, store_.epoch())),
                  request.seal.str());
}

Result<ReconciliationReport> Ledger::reconcile_locked(const ReconcileRequest& request) const {
    Instant reference = request.as_of;
    if (reference.seconds() == 0 && reference.nanos() == 0) {
        reference = request.interval.end();
    }
    Result<LedgerView> folded = LedgerView::fold(store_.records(), reference);
    if (!folded.ok()) {
        return folded.reason();
    }
    ReconcileRequest effective = request;
    effective.as_of = reference;
    return fel::reconcile(folded.value(), effective);
}

Result<ReconciliationReport> Ledger::reconcile(const ReconcileRequest& request) const {
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return reconcile_locked(request);
}

Result<LedgerView> Ledger::view(const Instant& freshness_reference) const {
    std::shared_lock<std::shared_mutex> guard(mutex_);
    return LedgerView::fold(store_.records(), freshness_reference);
}

Result<std::uint64_t> Ledger::compact() {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    const Result<std::uint64_t> dropped = store_.compact();
    if (!dropped.ok()) {
        return dropped.reason();
    }
    Result<LedgerView> folded = LedgerView::fold(store_.records(), Instant{});
    if (!folded.ok()) {
        return folded.reason();
    }
    index_ = folded.take();
    return dropped.value();
}

}  // namespace fel
