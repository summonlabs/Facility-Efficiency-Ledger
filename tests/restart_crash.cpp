#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <fstream>
#include <string>
#include <vector>

using namespace fel;
using namespace feltest;

namespace {

std::filesystem::path segment_path(const std::filesystem::path& directory) {
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().filename().string().rfind("fel-segment-", 0) == 0) {
            return entry.path();
        }
    }
    throw feltest::Failure{"no segment file in " + directory.string()};
}

void create_ledger(const std::filesystem::path& directory) {
    const Result<Ledger> created =
        Ledger::create(directory, feltest::id<LedgerIdTag>("restart-ledger"), "fel-tests");
    if (!created.ok()) {
        throw feltest::Failure{"cannot create the ledger: " + created.reason().message()};
    }
}

}  // namespace

FEL_TEST(restart, a_genuinely_killed_writer_leaves_a_recoverable_torn_tail) {
    feltest::TempDir directory("restart-torn");
    create_ledger(directory.path());
    const Interval interval = feltest::between("2026-10-01T00:00:00Z", "2026-10-01T01:00:00Z");
    {
        Result<Ledger> ledger = Ledger::open(directory.path(),
                                             StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-10-01T00:00:00Z");
        const SourceId source = feltest::add_source(ledger.value(), "meter-restart",
                                                    Unit::KilowattHour, base);
        const EntryId entry = feltest::add_measurement(ledger.value(), "e-restart", source, interval,
                                                       50, Unit::KilowattHour, base);
        feltest::add_classification(ledger.value(), "a-restart", entry, ServiceClass::Useful, 50,
                                    Unit::KilowattHour, base);
    }
    const std::filesystem::path segment = segment_path(directory.path());
    const std::uint64_t committed = feltest::read_bytes(segment).size();

    // A child process appends bytes that look like the start of a record and
    // then dies without unwinding.
    CHECK_EQ(feltest::run_process(feltest::helper_executable(),
                                  {"tear-tail", directory.path().string(), "211"}),
             0);
    CHECK(feltest::read_bytes(segment).size() == committed + 211U);

    {
        Result<Ledger> recovered =
            Ledger::open(directory.path(), StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(recovered.ok());
        CHECK(recovered.value().store().recovery().recovered_torn_tail);
        CHECK_EQ(recovered.value().store().recovery().truncated_bytes, 211U);
        CHECK_EQ(recovered.value().store().recovery().basis, AdoptionBasis::RecoveredTornTail);
        CHECK(!feltest::read_bytes(segment).empty());
    
        // Recovered dynamic evidence is not current: the interval reconciles as
        // history but a current assertion is refused.
        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> current = recovered.value().reconcile(request);
        REQUIRE(current.ok());
        CHECK(!current.value().closed);
        CHECK(current.value().code == ReasonCode::RecoveredNotCurrent);
        CHECK(current.value().dependencies.front().state == FreshnessState::Recovered);
    
        ReconcileRequest historical = request;
        historical.basis = BasisPolicy::Historical;
        const Result<ReconciliationReport> as_history = recovered.value().reconcile(historical);
        REQUIRE(as_history.ok());
        CHECK(as_history.value().closed);
    
        AttestGenerationRequest attest;
        attest.source = feltest::id<SourceIdTag>("meter-restart");
        attest.generation = Generation{1};
        attest.evidence = feltest::dig("post-recovery attestation");
        attest.method = "operator re-attestation after restart";
        attest.context.now = feltest::at("2026-10-01T02:00:00Z");
        REQUIRE(recovered.value().attest_generation(attest).ok());
        const Result<ReconciliationReport> after = recovered.value().reconcile(request);
        REQUIRE(after.ok());
        CHECK(after.value().closed);
        CHECK(after.value().dependencies.front().state == FreshnessState::Current);
    }

    // Compaction after recovery keeps the recovered semantics intact and the
    // ledger verifies.
    {
        Result<Ledger> compacted =
            Ledger::open(directory.path(), StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(compacted.ok());
        REQUIRE(compacted.value().compact().ok());
    }
    const Result<IntegrityReport> integrity = verify_ledger_directory(directory.path());
    REQUIRE(integrity.ok());
    CHECK(integrity.value().ok);
    CHECK(!integrity.value().torn_tail);
}

FEL_TEST(restart, a_torn_tail_never_truncates_a_committed_record) {
    feltest::TempDir directory("restart-truncate");
    create_ledger(directory.path());
    {
        Result<Ledger> ledger = Ledger::open(directory.path(),
                                             StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-truncate", Unit::KilowattHour,
                            feltest::at("2026-10-01T00:00:00Z"));
    }
    const std::filesystem::path segment = segment_path(directory.path());
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);

    // Garbage of many different lengths, including a full record's worth.
    const std::size_t lengths[] = {1U, 7U, 47U, 48U, 49U, 96U, 128U, 700U};
    for (std::size_t length : lengths) {
        feltest::write_bytes(segment, original);
        {
            std::ofstream stream(segment, std::ios::binary | std::ios::app);
            stream.write(feltest::repeat('\x5A', length).data(), static_cast<std::streamsize>(length));
            stream.flush();
        }
        Result<Ledger> reopened = Ledger::open(directory.path(),
                                               StoreOptions{OpenMode::Writer, false, {}, {}, {}});
        if (length >= kRecordOverhead) {
            // A complete record's worth of non-zero bytes with a broken header
            // checksum is corruption, not a torn tail.
            CHECK(!reopened.ok());
            continue;
        }
        REQUIRE(reopened.ok());
        CHECK(reopened.value().store().recovery().recovered_torn_tail);
        CHECK_EQ(reopened.value().store().recovery().truncated_bytes, length);
        CHECK_EQ(reopened.value().index().sources().size(), 1U);
    }
    feltest::write_bytes(segment, original);
    CHECK(feltest::run_process(feltest::helper_executable(),
                               {"fold-digest", directory.path().string(),
                                directory.file("state.txt").string()}) == 0);
    CHECK(feltest::read_text(directory.file("state.txt")).find("sources=1") != std::string::npos);
}
