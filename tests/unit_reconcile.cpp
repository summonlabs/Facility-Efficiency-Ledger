#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <fstream>
#include <string>

using namespace fel;
using namespace feltest;

namespace {

ReconciliationReport reconcile_fixture(feltest::Fixture& fixture, const Interval& interval,
                                       BasisPolicy basis) {
    Result<Ledger> ledger = fixture.reopen_reader();
    if (!ledger.ok()) {
        throw feltest::Failure{"cannot open: " + ledger.reason().message()};
    }
    ReconcileRequest request;
    request.interval = interval;
    request.basis = basis;
    const Result<ReconciliationReport> report = ledger.value().reconcile(request);
    if (!report.ok()) {
        throw feltest::Failure{"reconcile failed: " + report.reason().message()};
    }
    return report.value();
}

const DimensionClosure* find_dimension(const ReconciliationReport& report, Dimension dimension) {
    for (const DimensionClosure& closure : report.dimensions) {
        if (closure.dimension == dimension) {
            return &closure;
        }
    }
    return nullptr;
}

}  // namespace

FEL_TEST(conservation, inputs_must_reconcile_to_classified_outputs) {
    feltest::Fixture fixture("reconcile-closure");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-energy", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-energy", source, interval,
                                                       100, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-useful", entry, ServiceClass::Useful, 60,
                                    Unit::KilowattHour, base);
    }
    const ReconciliationReport partial = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK(!partial.closed);
    CHECK(partial.code == ReasonCode::UnclassifiedRemainder);
    const DimensionClosure* energy = find_dimension(partial, Dimension::Energy);
    REQUIRE(energy != nullptr);
    CHECK(energy->measured_input == feltest::qty(360000000, Unit::Joule));
    CHECK(energy->classified_useful == feltest::qty(216000000, Unit::Joule));
    CHECK(energy->unclassified == feltest::qty(144000000, Unit::Joule));
    CHECK(energy->closure_residual == energy->unclassified);

    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_classification(ledger.value(), "a-rest", feltest::id<EntryIdTag>("e-energy"),
                                    ServiceClass::Wasted, 40, Unit::KilowattHour,
                                    feltest::at("2026-01-01T00:10:00Z"));
    }
    const ReconciliationReport closed = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK(closed.closed);
    CHECK(closed.code == ReasonCode::Ok);
    const DimensionClosure* closed_energy = find_dimension(closed, Dimension::Energy);
    REQUIRE(closed_energy != nullptr);
    CHECK(closed_energy->input_total == closed_energy->classified_total);
    CHECK(closed_energy->unclassified.is_zero());
}

FEL_TEST(conservation, mixed_units_within_a_dimension_are_converted_exactly) {
    feltest::Fixture fixture("reconcile-units");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-mixed", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-mixed", source, interval, 1,
                                                       Unit::KilowattHour, base);
        // One third of the measured energy, expressed in joules.
        RecordClassificationRequest request;
        request.allocation = feltest::id<AllocationIdTag>("a-mixed");
        request.entry = entry;
        request.klass = ServiceClass::Useful;
        request.quantity = Quantity{*Rational::make(1200000, 1), Unit::Joule};
        request.evidence = feltest::dig("a-mixed");
        request.context.now = base;
        REQUIRE(ledger.value().record_classification(request).ok());
    }
    const ReconciliationReport report = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    const DimensionClosure* energy = find_dimension(report, Dimension::Energy);
    REQUIRE(energy != nullptr);
    CHECK(energy->classified_useful == feltest::qty(1200000, Unit::Joule));
    CHECK(energy->unclassified == feltest::qty(2400000, Unit::Joule));
}

FEL_TEST(conservation, residuals_are_never_absorbed_into_waste_or_useful_work) {
    feltest::Fixture fixture("reconcile-residual");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-residual", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-residual", source, interval,
                                                       100, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-residual", entry, ServiceClass::Useful, 100,
                                    Unit::KilowattHour, base);

        // Consumption that no meter captured at all, declared explicitly.
        RecordResidualRequest declared;
        declared.residual = feltest::id<ResidualIdTag>("r-declared");
        declared.interval = interval;
        declared.klass = ServiceClass::Unknown;
        declared.quantified = true;
        declared.quantity = feltest::qty(5, Unit::KilowattHour);
        declared.evidence = feltest::dig("r-declared");
        declared.basis = "unmetered plant auxiliary load";
        declared.context.now = base;
        REQUIRE(ledger.value().record_residual(declared).ok());
    }
    const ReconciliationReport report = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    const DimensionClosure* energy = find_dimension(report, Dimension::Energy);
    REQUIRE(energy != nullptr);
    CHECK(energy->measured_input == feltest::qty(360000000, Unit::Joule));
    CHECK(energy->declared_input == feltest::qty(18000000, Unit::Joule));
    CHECK(energy->input_total == feltest::qty(378000000, Unit::Joule));
    CHECK(energy->classified_unknown == feltest::qty(18000000, Unit::Joule));
    CHECK(energy->classified_useful == feltest::qty(360000000, Unit::Joule));
    CHECK(energy->closed);
    CHECK(energy->classified_wasted.is_zero());
    CHECK(energy->classified_avoidable.is_zero());
}

FEL_TEST(conservation, an_unquantified_residual_makes_closure_indeterminate) {
    feltest::Fixture fixture("reconcile-unmeasured");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-unmeasured", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-unmeasured", source, interval,
                                                       10, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-unmeasured", entry, ServiceClass::Useful, 10,
                                    Unit::KilowattHour, base);
        RecordResidualRequest unmeasured;
        unmeasured.residual = feltest::id<ResidualIdTag>("r-unmeasured");
        unmeasured.interval = interval;
        unmeasured.klass = ServiceClass::Unmeasured;
        unmeasured.quantified = false;
        unmeasured.quantity = Quantity{Rational{0}, Unit::KilowattHour};
        unmeasured.basis = "no sub-metering exists for this circuit";
        unmeasured.evidence = feltest::dig("r-unmeasured");
        unmeasured.context.now = base;
        REQUIRE(ledger.value().record_residual(unmeasured).ok());
    }
    const ReconciliationReport report = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK(!report.closed);
    CHECK(report.code == ReasonCode::NotQuantified);
    const DimensionClosure* energy = find_dimension(report, Dimension::Energy);
    REQUIRE(energy != nullptr);
    CHECK_EQ(energy->unquantified_residual_count, 1U);
    CHECK(energy->unclassified.is_zero());
    CHECK(!energy->closed);
}

FEL_TEST(conservation, bound_residuals_classify_measured_input) {
    feltest::Fixture fixture("reconcile-bound");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-bound", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-bound", source, interval, 100,
                                                       Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-bound", entry, ServiceClass::Useful, 70,
                                    Unit::KilowattHour, base);
        RecordResidualRequest bound;
        bound.residual = feltest::id<ResidualIdTag>("r-bound");
        bound.interval = interval;
        bound.klass = ServiceClass::Unknown;
        bound.quantified = true;
        bound.quantity = feltest::qty(30, Unit::KilowattHour);
        bound.bound_to_entry = true;
        bound.entry = entry;
        bound.basis = "the sub-meter cannot separate these two loads";
        bound.evidence = feltest::dig("r-bound");
        bound.context.now = base;
        REQUIRE(ledger.value().record_residual(bound).ok());
    }
    const ReconciliationReport report = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK(report.closed);
    const DimensionClosure* energy = find_dimension(report, Dimension::Energy);
    REQUIRE(energy != nullptr);
    CHECK(energy->measured_input == feltest::qty(360000000, Unit::Joule));
    CHECK(energy->declared_input.is_zero());
    CHECK(energy->classified_unknown == feltest::qty(108000000, Unit::Joule));
    CHECK_EQ(energy->bound_residual_count, 1U);
}

FEL_TEST(conservation, evidence_that_crosses_the_boundary_is_reported) {
    feltest::Fixture fixture("reconcile-boundary");
    const Instant base = feltest::at("2026-01-01T00:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const SourceId source = feltest::add_source(ledger.value(), "meter-cross", Unit::KilowattHour, base);
        feltest::add_measurement(ledger.value(), "e-cross", source,
                                 feltest::between("2026-01-01T00:30:00Z", "2026-01-01T01:30:00Z"), 10,
                                 Unit::KilowattHour, base);
    }
    Result<Ledger> ledger = fixture.reopen_reader();
    REQUIRE(ledger.ok());
    ReconcileRequest request;
    request.interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    const Result<ReconciliationReport> report = ledger.value().reconcile(request);
    REQUIRE(report.ok());
    CHECK(!report.value().closed);
    CHECK(report.value().code == ReasonCode::IntervalNotContained);
    CHECK(report.value().dimensions.empty());
}

FEL_TEST(conservation, dimensions_are_reconciled_independently) {
    feltest::Fixture fixture("reconcile-dimensions");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId energy = feltest::add_source(ledger.value(), "meter-power", Unit::KilowattHour, base);
        const EntryId energy_entry = feltest::add_measurement(ledger.value(), "e-power", energy, interval,
                                                              100, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-power", energy_entry, ServiceClass::Useful, 100,
                                    Unit::KilowattHour, base);
        const SourceId water = feltest::add_source(ledger.value(), "meter-water", Unit::Liter, base);
        const EntryId water_entry = feltest::add_measurement(ledger.value(), "e-water", water, interval,
                                                             500, Unit::Liter, base);
        feltest::add_classification(ledger.value(), "a-water", water_entry, ServiceClass::Useful, 100,
                                    Unit::Liter, base);
    }
    const ReconciliationReport report = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK_EQ(report.dimensions.size(), 2U);
    CHECK(!report.closed);
    const DimensionClosure* energy = find_dimension(report, Dimension::Energy);
    const DimensionClosure* water = find_dimension(report, Dimension::Volume);
    REQUIRE(energy != nullptr);
    REQUIRE(water != nullptr);
    CHECK(energy->closed);
    CHECK(!water->closed);
    CHECK(water->unclassified == feltest::qty(400, Unit::Liter));
    CHECK(report.code == ReasonCode::UnclassifiedRemainder);
}

FEL_TEST(conservation, current_basis_refuses_recovered_evidence_until_it_is_attested) {
    feltest::Fixture fixture("reconcile-recovered");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-recovery", Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-recovery", source, interval, 10,
                                                       Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-recovery", entry, ServiceClass::Useful, 10,
                                    Unit::KilowattHour, base);
    }
    const std::filesystem::path segment = [&] {
        std::error_code error;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(fixture.root, error)) {
            if (entry.path().filename().string().rfind("fel-segment-", 0) == 0) {
                return entry.path();
            }
        }
        throw feltest::Failure{"no segment file"};
    }();
    {
        std::ofstream stream(segment, std::ios::binary | std::ios::app);
        stream << "torn";
        stream.flush();
    }

    Result<Ledger> recovered = fixture.reopen_writer();
    REQUIRE(recovered.ok());
    CHECK(recovered.value().store().recovery().recovered_torn_tail);

    ReconcileRequest current_request;
    current_request.interval = interval;
    current_request.basis = BasisPolicy::Current;
    const Result<ReconciliationReport> current = recovered.value().reconcile(current_request);
    REQUIRE(current.ok());
    CHECK(!current.value().closed);
    CHECK(current.value().code == ReasonCode::RecoveredNotCurrent);
    REQUIRE(!current.value().dependencies.empty());
    CHECK(current.value().dependencies.front().state == FreshnessState::Recovered);

    ReconcileRequest historical_request;
    historical_request.interval = interval;
    historical_request.basis = BasisPolicy::Historical;
    const Result<ReconciliationReport> historical = recovered.value().reconcile(historical_request);
    REQUIRE(historical.ok());
    CHECK(historical.value().closed);
    CHECK(historical.value().dependencies.front().state == FreshnessState::Recovered);

    AttestGenerationRequest attest;
    attest.source = feltest::id<SourceIdTag>("meter-recovery");
    attest.generation = Generation{1};
    attest.evidence = feltest::dig("fresh attestation");
    attest.method = "operator re-attestation";
    attest.context.now = feltest::at("2026-01-01T02:00:00Z");
    REQUIRE(recovered.value().attest_generation(attest).ok());
    const Result<ReconciliationReport> after = recovered.value().reconcile(current_request);
    REQUIRE(after.ok());
    CHECK(after.value().closed);
    CHECK(after.value().dependencies.front().state == FreshnessState::Current);
}

FEL_TEST(conservation, reports_are_deterministic_and_digest_bound) {
    feltest::Fixture fixture("reconcile-determinism");
    const Interval interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-01-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-determinism",
                                                    Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-determinism", source, interval,
                                                       10, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-determinism", entry, ServiceClass::Useful, 10,
                                    Unit::KilowattHour, base);
    }
    const ReconciliationReport first = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    const ReconciliationReport second = reconcile_fixture(fixture, interval, BasisPolicy::Current);
    CHECK(first.report_digest == second.report_digest);
    CHECK(render_json(first) == render_json(second));
    CHECK(render_json(first).find(first.report_digest.to_hex()) != std::string::npos);
    CHECK(!first.report_digest.is_zero());
}
