#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

FEL_TEST(view, folding_reproduces_the_recorded_state) {
    feltest::Fixture fixture("view-fold");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const SourceId source = feltest::add_source(ledger.value(), "meter-view", Unit::KilowattHour,
                                                feltest::at("2026-01-01T00:00:00Z"));
    const EntryId entry = feltest::add_measurement(
        ledger.value(), "e-view", source,
        feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"), 42, Unit::KilowattHour,
        feltest::at("2026-01-01T00:30:00Z"));
    feltest::add_classification(ledger.value(), "a-view", entry, ServiceClass::Useful, 40,
                                Unit::KilowattHour, feltest::at("2026-01-01T00:40:00Z"));

    const Result<LedgerView> folded = LedgerView::fold(
        ledger.value().store().records(), feltest::at("2026-01-01T01:00:00Z"));
    REQUIRE(folded.ok());
    const Result<LedgerView> live = ledger.value().view(feltest::at("2026-01-01T01:00:00Z"));
    REQUIRE(live.ok());

    CHECK_EQ(folded.value().sources().size(), live.value().sources().size());
    CHECK_EQ(folded.value().measurements().size(), live.value().measurements().size());
    CHECK_EQ(folded.value().allocations().size(), live.value().allocations().size());
    CHECK(folded.value().revision() == live.value().revision());
    for (std::size_t index = 0; index < folded.value().measurements().size(); ++index) {
        CHECK(folded.value().measurements()[index].id == live.value().measurements()[index].id);
        CHECK(folded.value().measurements()[index].allocated ==
              live.value().measurements()[index].allocated);
        CHECK(folded.value().measurements()[index].record_digest ==
              live.value().measurements()[index].record_digest);
    }
    const MeasurementState* state = folded.value().find_measurement(entry);
    REQUIRE(state != nullptr);
    CHECK(state->allocated == feltest::qty(40, Unit::KilowattHour));
    CHECK_EQ(state->allocation_count, 1U);
}

FEL_TEST(view, freshness_distinguishes_every_state) {
    feltest::Fixture fixture("view-freshness");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-01-01T00:00:00Z");
    feltest::add_source(ledger.value(), "meter-fresh", Unit::KilowattHour, base);

    RegisterSourceRequest ungenerated;
    ungenerated.source = feltest::id<SourceIdTag>("meter-ungenerated");
    ungenerated.kind = SourceKind::Meter;
    ungenerated.unit = Unit::KilowattHour;
    ungenerated.context.now = base;
    REQUIRE(ledger.value().register_source(ungenerated).ok());

    RegisterSourceRequest retired;
    retired.source = feltest::id<SourceIdTag>("meter-retired");
    retired.kind = SourceKind::Meter;
    retired.unit = Unit::KilowattHour;
    retired.context.now = base;
    REQUIRE(ledger.value().register_source(retired).ok());
    PublishGenerationRequest publish;
    publish.source = retired.source;
    publish.generation = Generation{1};
    publish.evidence = feltest::dig("retired generation");
    publish.context.now = base;
    REQUIRE(ledger.value().publish_generation(publish).ok());
    RetireSourceRequest retire;
    retire.source = retired.source;
    retire.reason = "end of life";
    retire.context.now = base;
    REQUIRE(ledger.value().retire_source(retire).ok());

    PublishGenerationRequest expiring;
    expiring.source = feltest::id<SourceIdTag>("meter-expiring");
    expiring.generation = Generation{1};
    expiring.evidence = feltest::dig("expiring generation");
    expiring.valid_until = feltest::at("2026-01-01T00:30:00Z");
    expiring.context.now = base;
    RegisterSourceRequest expiring_source;
    expiring_source.source = expiring.source;
    expiring_source.kind = SourceKind::Meter;
    expiring_source.unit = Unit::KilowattHour;
    expiring_source.context.now = base;
    REQUIRE(ledger.value().register_source(expiring_source).ok());
    REQUIRE(ledger.value().publish_generation(expiring).ok());

    const Result<LedgerView> early = ledger.value().view(feltest::at("2026-01-01T00:20:00Z"));
    REQUIRE(early.ok());
    CHECK(early.value().find_source(feltest::id<SourceIdTag>("meter-fresh"))->freshness ==
          FreshnessState::Current);
    CHECK(early.value().find_source(feltest::id<SourceIdTag>("meter-ungenerated"))->freshness ==
          FreshnessState::Unattested);
    CHECK(early.value().find_source(feltest::id<SourceIdTag>("meter-retired"))->freshness ==
          FreshnessState::Retired);
    CHECK(early.value().find_source(feltest::id<SourceIdTag>("meter-expiring"))->freshness ==
          FreshnessState::Current);

    const Result<LedgerView> late = ledger.value().view(feltest::at("2026-01-01T01:00:00Z"));
    REQUIRE(late.ok());
    CHECK(late.value().find_source(feltest::id<SourceIdTag>("meter-expiring"))->freshness ==
          FreshnessState::Expired);
}

FEL_TEST(view, supersession_and_void_retire_state_without_erasing_records) {
    feltest::Fixture fixture("view-retire");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-01-01T00:00:00Z");
    const SourceId source = feltest::add_source(ledger.value(), "meter-retire", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(
        ledger.value(), "e-retire", source, feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"),
        10, Unit::KilowattHour, base);
    feltest::add_classification(ledger.value(), "a-retire", entry, ServiceClass::Useful, 10,
                                Unit::KilowattHour, base);

    const MeasurementState* measurement = ledger.value().index().find_measurement(entry);
    REQUIRE(measurement != nullptr);
    const AllocationState* allocation =
        ledger.value().index().find_allocation(feltest::id<AllocationIdTag>("a-retire"));
    REQUIRE(allocation != nullptr);

    VoidTargetRequest void_request;
    void_request.void_id = feltest::id<VoidIdTag>("v-retire");
    void_request.target_kind = TargetKind::Classification;
    void_request.target_id = "a-retire";
    void_request.target_digest = allocation->record_digest;
    void_request.reason = "mis-keyed classification";
    void_request.context.now = base;
    REQUIRE(ledger.value().void_target(void_request).ok());

    const Result<LedgerView> view = ledger.value().view(base);
    REQUIRE(view.ok());
    const AllocationState* voided =
        view.value().find_allocation(feltest::id<AllocationIdTag>("a-retire"));
    REQUIRE(voided != nullptr);
    CHECK(voided->voided);
    CHECK_EQ(voided->voided_by, "v-retire");
    const MeasurementState* target = view.value().find_measurement(entry);
    REQUIRE(target != nullptr);
    CHECK(target->allocated.is_zero());
    // The record is still in the journal even though it left the live state.
    bool found_record = false;
    for (const Record& record : ledger.value().store().records()) {
        if (record.type() == RecordType::ClassificationRecorded) {
            found_record = true;
        }
    }
    CHECK(found_record);
    CHECK(measurement->record_digest == target->record_digest);
}

FEL_TEST(view, conflicting_evidence_is_reported_not_merged) {
    feltest::Fixture fixture("view-conflict");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-01-01T00:00:00Z");
    const SourceId source = feltest::add_source(ledger.value(), "meter-conflict", Unit::KilowattHour, base);
    feltest::add_measurement(ledger.value(), "e-first", source,
                             feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"), 10,
                             Unit::KilowattHour, base);

    // A second measurement from the same source that overlaps the first is
    // refused at the command boundary rather than silently double counted.
    RecordMeasurementRequest overlapping;
    overlapping.entry = feltest::id<EntryIdTag>("e-second");
    overlapping.interval = feltest::between("2026-01-01T00:30:00Z", "2026-01-01T01:30:00Z");
    overlapping.source = source;
    overlapping.generation = Generation{1};
    overlapping.quantity = feltest::qty(10, Unit::KilowattHour);
    overlapping.evidence = feltest::dig("second");
    overlapping.context.now = base;
    const Result<CommandOutcome> refused = ledger.value().record_measurement(overlapping);
    CHECK(!refused.ok());
    CHECK_EQ(refused.code(), ReasonCode::IntervalOverlap);

    // A journal that does contain overlapping evidence is reported as such.
    std::vector<Record> forged = ledger.value().store().records();
    bool copied = false;
    for (const Record& record : ledger.value().store().records()) {
        if (record.type() == RecordType::MeasurementRecorded) {
            MeasurementRecordedBody body = record.measurement();
            body.entry = feltest::id<EntryIdTag>("e-forged");
            forged.push_back(Record::make_measurement(body, MutationMeta{}));
            copied = true;
            break;
        }
    }
    CHECK(copied);
    const Result<LedgerView> view = LedgerView::fold(forged, base);
    REQUIRE(view.ok());
    bool saw_overlap = false;
    bool saw_duplicate = false;
    for (const ViewIssue& issue : view.value().issues()) {
        if (issue.code == ReasonCode::IntervalOverlap) {
            saw_overlap = true;
        }
        if (issue.code == ReasonCode::DuplicateEvidence) {
            saw_duplicate = true;
        }
    }
    CHECK(saw_overlap);
    CHECK(saw_duplicate);
}

FEL_TEST(view, unknown_references_are_reported) {
    feltest::Fixture fixture("view-unknown");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-01-01T00:00:00Z");
    std::vector<Record> records = ledger.value().store().records();

    ClassificationRecordedBody orphan;
    orphan.allocation = feltest::id<AllocationIdTag>("a-orphan");
    orphan.entry = feltest::id<EntryIdTag>("e-never-recorded");
    orphan.klass = ServiceClass::Useful;
    orphan.quantity = feltest::qty(1, Unit::KilowattHour);
    orphan.evidence = feltest::dig("orphan");
    records.push_back(Record::make_classification(orphan, MutationMeta{}));
    records.back().set_revision(Revision{100});

    const Result<LedgerView> view = LedgerView::fold(records, base);
    REQUIRE(view.ok());
    bool saw_unknown = false;
    for (const ViewIssue& issue : view.value().issues()) {
        if (issue.code == ReasonCode::UnknownEntry) {
            saw_unknown = true;
        }
    }
    CHECK(saw_unknown);
}
