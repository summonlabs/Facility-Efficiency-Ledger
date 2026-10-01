#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

namespace {

constexpr const char* kBase = "2026-02-01T00:00:00Z";
constexpr const char* kMid = "2026-02-01T00:30:00Z";
constexpr const char* kEnd = "2026-02-01T01:00:00Z";

}  // namespace

FEL_TEST(commands, idempotent_retry_replays_instead_of_duplicating) {
    feltest::Fixture fixture("commands-idempotency");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);

    RegisterSourceRequest request;
    request.source = feltest::id<SourceIdTag>("meter-idem");
    request.kind = SourceKind::Meter;
    request.unit = Unit::KilowattHour;
    request.label = "retry-safe meter";
    request.context.now = base;
    request.context.idempotency = feltest::id<IdempotencyKeyTag>("key-source-1");

    const Result<CommandOutcome> first = ledger.value().register_source(request);
    REQUIRE(first.ok());
    CHECK(first.value().applied);
    CHECK(!first.value().replayed);

    const Result<CommandOutcome> second = ledger.value().register_source(request);
    REQUIRE(second.ok());
    CHECK(!second.value().applied);
    CHECK(second.value().replayed);
    CHECK(second.value().record_digest == first.value().record_digest);
    CHECK(second.value().revision == first.value().revision);
    CHECK_EQ(ledger.value().index().sources().size(), 1U);
    CHECK_EQ(ledger.value().revision().value(), first.value().revision.value());

    // The same key with a different request is a conflict, not a replay.
    RegisterSourceRequest different = request;
    different.label = "a different request";
    const Result<CommandOutcome> conflict = ledger.value().register_source(different);
    CHECK(!conflict.ok());
    CHECK_EQ(conflict.code(), ReasonCode::IdempotencyConflict);
}

FEL_TEST(commands, stale_expectations_are_refused) {
    feltest::Fixture fixture("commands-stale");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);

    RegisterSourceRequest request;
    request.source = feltest::id<SourceIdTag>("meter-stale");
    request.kind = SourceKind::Meter;
    request.unit = Unit::KilowattHour;
    request.context.now = base;
    request.context.expected_revision = Revision{99};
    const Result<CommandOutcome> wrong_revision = ledger.value().register_source(request);
    CHECK(!wrong_revision.ok());
    CHECK_EQ(wrong_revision.code(), ReasonCode::StaleRevision);

    request.context.expected_revision = Revision{};
    request.context.expected_epoch = Epoch{ledger.value().store().epoch().value() + 5U};
    const Result<CommandOutcome> wrong_epoch = ledger.value().register_source(request);
    CHECK(!wrong_epoch.ok());
    CHECK_EQ(wrong_epoch.code(), ReasonCode::StaleEpoch);

    request.context.expected_epoch = Epoch{ledger.value().store().epoch().value()};
    request.context.expected_revision = ledger.value().store().revision();
    const Result<CommandOutcome> accepted = ledger.value().register_source(request);
    CHECK(accepted.ok());
}

FEL_TEST(commands, a_read_only_handle_cannot_write) {
    feltest::Fixture fixture("commands-readonly");
    {
        Result<Ledger> writer = fixture.reopen_writer();
        REQUIRE(writer.ok());
    }
    Result<Ledger> reader = fixture.reopen_reader();
    REQUIRE(reader.ok());
    RegisterSourceRequest request;
    request.source = feltest::id<SourceIdTag>("meter-readonly");
    request.kind = SourceKind::Meter;
    request.unit = Unit::KilowattHour;
    request.context.now = feltest::at(kBase);
    const Result<CommandOutcome> refused = reader.value().register_source(request);
    CHECK(!refused.ok());
    CHECK_EQ(refused.code(), ReasonCode::AuthorityNotOwned);
}

FEL_TEST(commands, measurements_require_a_current_generation_and_matching_units) {
    feltest::Fixture fixture("commands-measurement-guards");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-guard", Unit::KilowattHour, base);

    RecordMeasurementRequest request;
    request.entry = feltest::id<EntryIdTag>("e-guard");
    request.interval = feltest::between(kBase, kEnd);
    request.source = source;
    request.generation = Generation{1};
    request.quantity = feltest::qty(1, Unit::KilowattHour);
    request.evidence = feltest::dig("e-guard");
    request.context.now = base;

    RecordMeasurementRequest stale_generation = request;
    stale_generation.generation = Generation{2};
    CHECK(ledger.value().record_measurement(stale_generation).code() ==
          ReasonCode::StaleGeneration);

    RecordMeasurementRequest wrong_dimension = request;
    wrong_dimension.quantity = feltest::qty(1, Unit::Liter);
    CHECK(ledger.value().record_measurement(wrong_dimension).code() ==
          ReasonCode::DimensionMismatch);

    RecordMeasurementRequest no_evidence = request;
    no_evidence.evidence = Digest{};
    CHECK(ledger.value().record_measurement(no_evidence).code() == ReasonCode::InvalidArgument);

    RecordMeasurementRequest zero = request;
    zero.quantity = feltest::qty(0, Unit::KilowattHour);
    CHECK(ledger.value().record_measurement(zero).code() == ReasonCode::NonPositiveQuantity);

    RecordMeasurementRequest negative = request;
    negative.quantity = Quantity{Rational{-5}, Unit::KilowattHour};
    CHECK(ledger.value().record_measurement(negative).code() == ReasonCode::NonPositiveQuantity);

    RecordMeasurementRequest bad_subject = request;
    bad_subject.subject_kind = SubjectKind::Rack;
    bad_subject.subject = "not a valid identifier";
    CHECK(ledger.value().record_measurement(bad_subject).code() == ReasonCode::InvalidIdentifier);

    RecordMeasurementRequest stray_subject = request;
    stray_subject.subject = "rack-a1";
    CHECK(ledger.value().record_measurement(stray_subject).code() == ReasonCode::InvalidArgument);

    CHECK(ledger.value().record_measurement(request).ok());
    RecordMeasurementRequest duplicate = request;
    CHECK(ledger.value().record_measurement(duplicate).code() == ReasonCode::DuplicateEvidence);
}

FEL_TEST(commands, corrections_are_attributed_and_digest_checked) {
    feltest::Fixture fixture("commands-corrections");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-correction", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(ledger.value(), "e-corrected", source,
                                                   feltest::between(kBase, kEnd), 10,
                                                   Unit::KilowattHour, base);
    const MeasurementState* original = ledger.value().index().find_measurement(entry);
    REQUIRE(original != nullptr);

    RecordMeasurementRequest correction;
    correction.entry = feltest::id<EntryIdTag>("e-corrected-v2");
    correction.interval = feltest::between(kBase, kEnd);
    correction.source = source;
    correction.generation = Generation{1};
    correction.quantity = feltest::qty(12, Unit::KilowattHour);
    correction.evidence = feltest::dig("corrected evidence");
    correction.supersedes = true;
    correction.supersedes_entry = entry;
    correction.supersedes_digest = original->record_digest;
    correction.reason = "meter was read with a transposed digit";
    correction.context.now = feltest::at(kMid);

    RecordMeasurementRequest without_reason = correction;
    without_reason.reason.clear();
    CHECK(ledger.value().record_measurement(without_reason).code() == ReasonCode::InvalidArgument);

    RecordMeasurementRequest wrong_digest = correction;
    wrong_digest.supersedes_digest = feltest::dig("someone else's record");
    CHECK(ledger.value().record_measurement(wrong_digest).code() == ReasonCode::DigestMismatch);

    RecordMeasurementRequest unknown_target = correction;
    unknown_target.supersedes_entry = feltest::id<EntryIdTag>("e-never-existed");
    CHECK(ledger.value().record_measurement(unknown_target).code() == ReasonCode::UnknownEntry);

    REQUIRE(ledger.value().record_measurement(correction).ok());
    const MeasurementState* superseded = ledger.value().index().find_measurement(entry);
    REQUIRE(superseded != nullptr);
    CHECK(!superseded->superseded_by.empty());

    // A correction cannot be corrected twice.
    RecordMeasurementRequest twice = correction;
    twice.entry = feltest::id<EntryIdTag>("e-corrected-v3");
    CHECK(ledger.value().record_measurement(twice).code() == ReasonCode::AlreadySuperseded);
}

FEL_TEST(commands, voids_require_the_exact_live_digest) {
    feltest::Fixture fixture("commands-voids");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-void", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(ledger.value(), "e-voided", source,
                                                   feltest::between(kBase, kEnd), 10,
                                                   Unit::KilowattHour, base);
    const MeasurementState* state = ledger.value().index().find_measurement(entry);
    REQUIRE(state != nullptr);

    VoidTargetRequest request;
    request.void_id = feltest::id<VoidIdTag>("v-voided");
    request.target_kind = TargetKind::Measurement;
    request.target_id = "e-voided";
    request.target_digest = feltest::dig("wrong");
    request.reason = "duplicate meter export";
    request.context.now = base;
    CHECK(ledger.value().void_target(request).code() == ReasonCode::DigestMismatch);

    request.target_digest = state->record_digest;
    REQUIRE(ledger.value().void_target(request).ok());

    VoidTargetRequest again = request;
    again.void_id = feltest::id<VoidIdTag>("v-voided-2");
    CHECK(ledger.value().void_target(again).code() == ReasonCode::AlreadySuperseded);

    VoidTargetRequest unknown = request;
    unknown.void_id = feltest::id<VoidIdTag>("v-voided-3");
    unknown.target_id = "e-never-existed";
    CHECK(ledger.value().void_target(unknown).code() == ReasonCode::UnknownEntry);
}

FEL_TEST(commands, sources_cannot_be_registered_twice_or_retired_twice) {
    feltest::Fixture fixture("commands-sources");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-twice", Unit::KilowattHour, base);

    RegisterSourceRequest duplicate;
    duplicate.source = source;
    duplicate.kind = SourceKind::Meter;
    duplicate.unit = Unit::KilowattHour;
    duplicate.context.now = base;
    CHECK(ledger.value().register_source(duplicate).code() == ReasonCode::DuplicateSource);

    RetireSourceRequest retire;
    retire.source = source;
    retire.reason = "removed";
    retire.context.now = base;
    REQUIRE(ledger.value().retire_source(retire).ok());
    CHECK(ledger.value().retire_source(retire).code() == ReasonCode::SourceRetired);

    RecordMeasurementRequest after_retirement;
    after_retirement.entry = feltest::id<EntryIdTag>("e-after-retirement");
    after_retirement.interval = feltest::between(kBase, kEnd);
    after_retirement.source = source;
    after_retirement.generation = Generation{1};
    after_retirement.quantity = feltest::qty(1, Unit::KilowattHour);
    after_retirement.evidence = feltest::dig("e-after-retirement");
    after_retirement.context.now = base;
    CHECK(ledger.value().record_measurement(after_retirement).code() == ReasonCode::SourceRetired);
}

FEL_TEST(commands, generations_must_advance_and_attestations_must_match) {
    feltest::Fixture fixture("commands-generations");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-generation", Unit::KilowattHour, base);

    PublishGenerationRequest regressed;
    regressed.source = source;
    regressed.generation = Generation{1};
    regressed.evidence = feltest::dig("regressed");
    regressed.context.now = base;
    CHECK(ledger.value().publish_generation(regressed).code() == ReasonCode::GenerationRegressed);

    AttestGenerationRequest mismatched;
    mismatched.source = source;
    mismatched.generation = Generation{7};
    mismatched.evidence = feltest::dig("mismatched");
    mismatched.context.now = base;
    CHECK(ledger.value().attest_generation(mismatched).code() == ReasonCode::StaleGeneration);

    AttestGenerationRequest no_evidence;
    no_evidence.source = source;
    no_evidence.generation = Generation{1};
    no_evidence.context.now = base;
    CHECK(ledger.value().attest_generation(no_evidence).code() == ReasonCode::InvalidArgument);

    PublishGenerationRequest next;
    next.source = source;
    next.generation = Generation{2};
    next.evidence = feltest::dig("generation two");
    next.context.now = base;
    REQUIRE(ledger.value().publish_generation(next).ok());
    AttestGenerationRequest stale_attestation;
    stale_attestation.source = source;
    stale_attestation.generation = Generation{1};
    stale_attestation.evidence = feltest::dig("late attestation");
    stale_attestation.context.now = base;
    CHECK(ledger.value().attest_generation(stale_attestation).code() ==
          ReasonCode::StaleGeneration);
}

FEL_TEST(commands, sealing_requires_an_exactly_closed_interval) {
    feltest::Fixture fixture("commands-seal");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-seal", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(ledger.value(), "e-seal", source,
                                                   feltest::between(kBase, kEnd), 10,
                                                   Unit::KilowattHour, base);
    const Interval interval = feltest::between(kBase, kEnd);
    SealIntervalRequest seal;
    seal.seal = feltest::id<SealIdTag>("seal-1");
    seal.interval = interval;
    seal.context.now = feltest::at(kEnd);
    const Result<CommandOutcome> refused = ledger.value().seal_interval(seal);
    CHECK(!refused.ok());
    CHECK_EQ(refused.code(), ReasonCode::AccountingNotClosed);

    feltest::add_classification(ledger.value(), "a-seal", entry, ServiceClass::Useful, 10,
                                Unit::KilowattHour, base);
    REQUIRE(ledger.value().seal_interval(seal).ok());
    SealIntervalRequest duplicate = seal;
    duplicate.seal = feltest::id<SealIdTag>("seal-2");
    CHECK(ledger.value().seal_interval(duplicate).code() == ReasonCode::AlreadyExists);
}

FEL_TEST(commands, residual_guards_are_enforced) {
    feltest::Fixture fixture("commands-residuals");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at(kBase);
    const SourceId source = feltest::add_source(ledger.value(), "meter-residual-guard",
                                                Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(ledger.value(), "e-residual-guard", source,
                                                   feltest::between(kBase, kEnd), 10,
                                                   Unit::KilowattHour, base);

    RecordResidualRequest request;
    request.residual = feltest::id<ResidualIdTag>("r-guard");
    request.interval = feltest::between(kBase, kEnd);
    request.klass = ServiceClass::Unknown;
    request.quantified = true;
    request.quantity = feltest::qty(1, Unit::KilowattHour);
    request.evidence = feltest::dig("r-guard");
    request.basis = "coverage gap";
    request.context.now = base;

    RecordResidualRequest not_a_residual = request;
    not_a_residual.klass = ServiceClass::Useful;
    CHECK(ledger.value().record_residual(not_a_residual).code() == ReasonCode::InvalidArgument);

    RecordResidualRequest unquantified_unknown = request;
    unquantified_unknown.quantified = false;
    CHECK(ledger.value().record_residual(unquantified_unknown).code() ==
          ReasonCode::InvalidArgument);

    RecordResidualRequest unquantified_bound = request;
    unquantified_bound.quantified = false;
    unquantified_bound.klass = ServiceClass::Unmeasured;
    unquantified_bound.bound_to_entry = true;
    unquantified_bound.entry = entry;
    CHECK(ledger.value().record_residual(unquantified_bound).code() == ReasonCode::NotQuantified);

    RecordResidualRequest no_basis = request;
    no_basis.basis.clear();
    CHECK(ledger.value().record_residual(no_basis).code() == ReasonCode::InvalidArgument);

    RecordResidualRequest outside = request;
    outside.bound_to_entry = true;
    outside.entry = entry;
    outside.interval = feltest::between("2026-02-01T00:30:00Z", "2026-02-01T02:00:00Z");
    CHECK(ledger.value().record_residual(outside).code() == ReasonCode::IntervalNotContained);

    RecordResidualRequest too_large = request;
    too_large.bound_to_entry = true;
    too_large.entry = entry;
    too_large.quantity = feltest::qty(20, Unit::KilowattHour);
    CHECK(ledger.value().record_residual(too_large).code() == ReasonCode::OverAllocation);

    REQUIRE(ledger.value().record_residual(request).ok());
}
