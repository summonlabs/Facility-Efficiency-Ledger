#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

namespace {

constexpr const char* kStart = "2026-04-01T00:00:00Z";
constexpr const char* kEnd = "2026-04-01T01:00:00Z";

}  // namespace

FEL_TEST(lifecycle, a_full_accounting_cycle_survives_every_boundary) {
    feltest::TempDir directory("e2e");
    const std::filesystem::path root = directory.path();
    const Interval interval = feltest::between(kStart, kEnd);
    const Instant base = feltest::at(kStart);
    const Instant close = feltest::at(kEnd);

    // 1. Create the ledger.
    {
        const Result<Ledger> created =
            Ledger::create(root, feltest::id<LedgerIdTag>("e2e-ledger"), "fel-tests");
        REQUIRE(created.ok());
        CHECK_EQ(created.value().index().ledger().str(), "e2e-ledger");
    }
    CHECK(std::filesystem::exists(root / "fel.manifest"));

    // 2. Register two sources in different dimensions and publish generations.
    {
        Result<Ledger> ledger = Ledger::open(root, StoreOptions{OpenMode::Writer, false, {}, "fel-tests", {}});
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-power-a", Unit::KilowattHour, base);
        feltest::add_source(ledger.value(), "meter-water-a", Unit::Liter, base);
        const EntryId power = feltest::add_measurement(ledger.value(), "e-power", 
                                                       feltest::id<SourceIdTag>("meter-power-a"),
                                                       interval, 100, Unit::KilowattHour, base);
        const EntryId water = feltest::add_measurement(ledger.value(), "e-water",
                                                       feltest::id<SourceIdTag>("meter-water-a"),
                                                       interval, 400, Unit::Liter, base);
        // Classify 80 kWh useful, 20 kWh wasted; 350 L useful, 50 L unknown.
        feltest::add_classification(ledger.value(), "a-power-useful", power, ServiceClass::Useful, 80,
                                    Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-power-wasted", power, ServiceClass::Wasted, 20,
                                    Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-water-useful", water, ServiceClass::Useful, 350,
                                    Unit::Liter, base);
        RecordResidualRequest residual;
        residual.residual = feltest::id<ResidualIdTag>("r-water-unknown");
        residual.interval = interval;
        residual.klass = ServiceClass::Unknown;
        residual.quantified = true;
        residual.quantity = feltest::qty(50, Unit::Liter);
        residual.bound_to_entry = true;
        residual.entry = water;
        residual.basis = "no sub-metering on the secondary loop";
        residual.evidence = feltest::dig("r-water-unknown");
        residual.context.now = base;
        REQUIRE(ledger.value().record_residual(residual).ok());
    }

    // 3. A separate handle reconciles and finds the interval closed.
    Digest first_digest{};
    Digest first_historical_digest{};
    {
        Result<Ledger> reader = Ledger::open(root, StoreOptions{OpenMode::Reader, false, {}, {}, {}});
        REQUIRE(reader.ok());
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = reader.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().closed);
        CHECK_EQ(report.value().dimensions.size(), 2U);
        CHECK_EQ(report.value().measurement_count, 2U);
        CHECK_EQ(report.value().allocation_count, 3U);
        first_digest = report.value().report_digest;
        CHECK(!first_digest.is_zero());
        // A seal is recorded on the historical basis, and the basis is part of
        // what a report asserts, so the sealed digest is the historical one.
        ReconcileRequest historical = request;
        historical.basis = BasisPolicy::Historical;
        const Result<ReconciliationReport> historical_report =
            reader.value().reconcile(historical);
        REQUIRE(historical_report.ok());
        CHECK(historical_report.value().closed);
        first_historical_digest = historical_report.value().report_digest;
        CHECK(first_historical_digest != first_digest);
    }

    // 4. Seal the interval with the published report digest.
    {
        Result<Ledger> ledger = Ledger::open(root, StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(ledger.ok());
        SealIntervalRequest seal;
        seal.seal = feltest::id<SealIdTag>("seal-april");
        seal.interval = interval;
        seal.note = "monthly close";
        seal.context.now = close;
        REQUIRE(ledger.value().seal_interval(seal).ok());
    }

    // 5. A sealed interval reports its seal and is not stale yet.
    {
        Result<Ledger> reader = Ledger::open(root, StoreOptions{OpenMode::Reader, false, {}, {}, {}});
        REQUIRE(reader.ok());
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = reader.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().sealed);
        CHECK(!report.value().changed_since_seal);
        CHECK_EQ(report.value().sealed_report_digest.to_hex(), first_historical_digest.to_hex());
        CHECK(report.value().closed);
    }

    // 6. A post-seal correction makes the seal stale without erasing it.
    {
        Result<Ledger> ledger = Ledger::open(root, StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(ledger.ok());
        RecordResidualRequest correction;
        correction.residual = feltest::id<ResidualIdTag>("r-water-unknown-v2");
        correction.interval = interval;
        correction.klass = ServiceClass::Unknown;
        correction.quantified = true;
        correction.quantity = feltest::qty(50, Unit::Liter);
        correction.bound_to_entry = true;
        correction.entry = feltest::id<EntryIdTag>("e-water");
        correction.basis = "restated after meter inspection";
        correction.evidence = feltest::dig("r-water-unknown-v2");
        correction.supersedes = true;
        correction.supersedes_residual = feltest::id<ResidualIdTag>("r-water-unknown");
        const ResidualState* state =
            ledger.value().index().find_residual(feltest::id<ResidualIdTag>("r-water-unknown"));
        REQUIRE(state != nullptr);
        correction.supersedes_digest = state->record_digest;
        correction.reason = "same treatment, reassessed basis";
        correction.context.now = close;
        REQUIRE(ledger.value().record_residual(correction).ok());
    }
    {
        Result<Ledger> reader = Ledger::open(root, StoreOptions{OpenMode::Reader, false, {}, {}, {}});
        REQUIRE(reader.ok());
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = reader.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().sealed);
        CHECK(report.value().changed_since_seal);
        CHECK(report.value().closed);
        CHECK(report.value().report_digest != first_digest);
    }

    // 7. Compact, then prove the fold is unchanged and the ledger still verifies.
    Digest after_compaction{};
    {
        Result<Ledger> ledger = Ledger::open(root, StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(ledger.ok());
        const Result<std::uint64_t> dropped = ledger.value().compact();
        REQUIRE(dropped.ok());
        CHECK(dropped.value() >= 1U);
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = ledger.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().closed);
        after_compaction = report.value().report_digest;
    }
    {
        Result<Ledger> reader = Ledger::open(root, StoreOptions{OpenMode::Reader, false, {}, {}, {}});
        REQUIRE(reader.ok());
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = reader.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().report_digest == after_compaction);
        CHECK(report.value().closed);
    }
    const Result<IntegrityReport> integrity = verify_ledger_directory(root);
    REQUIRE(integrity.ok());
    CHECK(integrity.value().ok);
    CHECK(!integrity.value().torn_tail);
    CHECK(integrity.value().snapshot_records > 0U);

    // 8. The sealed interval still reconciles historically after compaction.
    {
        Result<Ledger> reader = Ledger::open(root, StoreOptions{OpenMode::Reader, false, {}, {}, {}});
        REQUIRE(reader.ok());
        ReconcileRequest request;
        request.interval = interval;
        request.basis = BasisPolicy::Historical;
        const Result<ReconciliationReport> report = reader.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().closed);
        CHECK(report.value().sealed);
    }
}

FEL_TEST(lifecycle, canonical_json_is_reproducible_across_processes) {
    feltest::TempDir directory("e2e-json");
    const std::filesystem::path root = directory.path();
    const std::filesystem::path dump_one = directory.file("one.txt");
    const std::filesystem::path dump_two = directory.file("two.txt");
    CHECK_EQ(feltest::run_process(feltest::helper_executable(), {"populate", root.string()}), 0);
    CHECK_EQ(feltest::run_process(feltest::helper_executable(), {"fold-digest", root.string(), dump_one.string()}), 0);
    CHECK_EQ(feltest::run_process(feltest::helper_executable(), {"fold-digest", root.string(), dump_two.string()}), 0);
    const std::string first = feltest::read_text(dump_one);
    CHECK(first == feltest::read_text(dump_two));
    CHECK(first.find("measurements=1") != std::string::npos);
    CHECK(first.find("seals=1") != std::string::npos);
}
