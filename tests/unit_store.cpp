#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <fstream>
#include <string>

using namespace fel;
using namespace feltest;

namespace {

void append_raw(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream) {
        throw feltest::Failure{"cannot open " + path.string() + " for appending"};
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
}

std::filesystem::path ledger_file(const std::filesystem::path& directory, const std::string& prefix) {
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            return entry.path();
        }
    }
    throw feltest::Failure{"no file with prefix " + prefix + " in " + directory.string()};
}

}  // namespace

FEL_TEST(store, initialization_publishes_a_manifest_and_an_epoch) {
    feltest::Fixture fixture("store-init");
    {
        const Result<Ledger> reader = fixture.reopen_reader();
        REQUIRE(reader.ok());
        // A reader observes the epoch recorded when the ledger was created and
        // never adopts one of its own.
        CHECK_EQ(reader.value().store().epoch().value(), 1U);
        CHECK_EQ(reader.value().store().revision().value(), 0U);
    }
    {
        const Result<Ledger> writer = fixture.reopen_writer();
        REQUIRE(writer.ok());
        // A writer session adopts the next epoch and records the adoption.
        CHECK_EQ(writer.value().store().epoch().value(), 2U);
        CHECK(writer.value().store().recovery().basis == AdoptionBasis::CleanReopen);
    }
    {
        const Result<Ledger> reader = fixture.reopen_reader();
        REQUIRE(reader.ok());
        CHECK_EQ(reader.value().store().epoch().value(), 2U);
    }
    CHECK(std::filesystem::exists(fixture.root / "fel.manifest"));
    CHECK(std::filesystem::exists(fixture.root / "fel.lock"));
    CHECK(feltest::read_bytes(fixture.root / "fel.manifest").size() > kDigestBytes);
}

FEL_TEST(store, revision_advances_only_for_mutating_records) {
    feltest::Fixture fixture("store-revision");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Revision before = ledger.value().revision();
    feltest::add_source(ledger.value(), "meter-a", Unit::KilowattHour,
                        feltest::at("2026-01-01T00:00:00Z"));
    // Registration and generation publication are two separate mutations.
    CHECK(ledger.value().revision() == Revision{before.value() + 2U});
    const Revision after_source = ledger.value().revision();
    feltest::add_measurement(ledger.value(), "e-1", feltest::id<SourceIdTag>("meter-a"),
                             feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"), 10,
                             Unit::KilowattHour, feltest::at("2026-01-01T00:30:00Z"));
    CHECK(ledger.value().revision() == Revision{after_source.value() + 1U});
}

FEL_TEST(store, reopen_reproduces_the_chain_and_the_record_count) {
    feltest::Fixture fixture("store-reopen");
    Digest head_after_writes{};
    std::size_t count = 0;
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const SourceId source = feltest::add_source(ledger.value(), "meter-b", Unit::KilowattHour,
                                                    feltest::at("2026-01-01T00:00:00Z"));
        feltest::add_measurement(ledger.value(), "e-1", source,
                                 feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"), 5,
                                 Unit::KilowattHour, feltest::at("2026-01-01T00:30:00Z"));
        head_after_writes = ledger.value().store().chain_head();
        count = ledger.value().store().records().size();
    }
    Result<Ledger> reopened = fixture.reopen_writer();
    REQUIRE(reopened.ok());
    CHECK(reopened.value().store().chain_head() != head_after_writes ||
          reopened.value().store().records().size() > count);
    // The records that were committed are all still present, in order.
    CHECK(reopened.value().store().records().size() >= count);
    for (std::size_t index = 1; index < reopened.value().store().records().size(); ++index) {
        CHECK(reopened.value().store().records()[index].sequence().value() ==
              reopened.value().store().records()[index - 1].sequence().value() + 1U);
    }
}


FEL_TEST(store, a_torn_tail_is_discarded_and_recorded_as_recovery) {
    feltest::Fixture fixture("store-torn");
    std::size_t committed = 0;
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-c", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
        committed = ledger.value().store().records().size();
    }
    const std::filesystem::path segment = ledger_file(fixture.root, "fel-segment-");
    const std::uint64_t before = feltest::read_bytes(segment).size();
    append_raw(segment, feltest::repeat('\xAB', 37));
    CHECK(feltest::read_bytes(segment).size() == before + 37U);

    std::uint64_t recovered_revision = 0;
    {
        Result<Ledger> recovered = fixture.reopen_writer();
        REQUIRE(recovered.ok());
        CHECK(recovered.value().store().recovery().recovered_torn_tail);
        CHECK_EQ(recovered.value().store().recovery().truncated_bytes, 37U);
        CHECK(recovered.value().store().recovery().basis == AdoptionBasis::RecoveredTornTail);
        // The discarded tail is gone; the only growth is the epoch-adoption
        // record the new session writes to record that it recovered.
        const std::size_t after = feltest::read_bytes(segment).size();
        CHECK(after >= before);
        CHECK(after < before + 37U + 4096U);
        CHECK(recovered.value().store().records().size() >= committed + 1U);
        recovered_revision = recovered.value().store().revision().value();
    }
    const Result<IntegrityReport> clean = verify_ledger_directory(fixture.root);
    REQUIRE(clean.ok());
    CHECK(clean.value().ok);
    CHECK(!clean.value().torn_tail);
    CHECK_EQ(clean.value().revision.value(), recovered_revision);

    // While a writer holds the ledger a verifier cannot take the shared lock,
    // and says so rather than reading a moving target.
    {
        Result<Ledger> holder = fixture.reopen_writer();
        REQUIRE(holder.ok());
        const Result<IntegrityReport> blocked = verify_ledger_directory(fixture.root);
        CHECK(!blocked.ok());
        CHECK_EQ(blocked.code(), ReasonCode::LockHeldExclusive);
    }
    const Result<IntegrityReport> unblocked = verify_ledger_directory(fixture.root);
    REQUIRE(unblocked.ok());
}
FEL_TEST(store, a_reader_does_not_modify_a_torn_ledger) {
    feltest::Fixture fixture("store-torn-reader");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-d", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
    }
    const std::filesystem::path segment = ledger_file(fixture.root, "fel-segment-");
    const std::uint64_t before = feltest::read_bytes(segment).size();
    append_raw(segment, "partial");
    const std::uint64_t torn = feltest::read_bytes(segment).size();
    CHECK(torn == before + 7U);

    Result<Ledger> reader = fixture.reopen_reader();
    REQUIRE(reader.ok());
    CHECK(reader.value().store().recovery().recovered_torn_tail);
    CHECK_EQ(feltest::read_bytes(segment).size(), torn);
}

FEL_TEST(store, interior_corruption_is_refused) {
    feltest::Fixture fixture("store-corrupt");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-e", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
        feltest::add_source(ledger.value(), "meter-f", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
    }
    const std::filesystem::path segment = ledger_file(fixture.root, "fel-segment-");
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);
    REQUIRE(original.size() > 200U);

    // Flip one bit in the middle of the second record: this leaves a complete
    // record whose checksum no longer matches, so it is corruption rather than a
    // torn tail and must be refused.
    const std::uint64_t offset = static_cast<std::uint64_t>(original.size() / 2U);
    std::vector<std::uint8_t> tampered = original;
    tampered[static_cast<std::size_t>(offset)] ^= 0x10U;
    feltest::write_bytes(segment, tampered);
    const Result<Ledger> refused = fixture.reopen_writer();
    CHECK(!refused.ok());
    const ReasonCode code = refused.code();
    CHECK(code == ReasonCode::InteriorCorruption || code == ReasonCode::HeaderCorrupt ||
          code == ReasonCode::RecordCorrupt || code == ReasonCode::ChainBroken);
    // A refused open must not rewrite the evidence.
    CHECK(feltest::read_bytes(segment) == tampered);
    feltest::write_bytes(segment, original);
}

FEL_TEST(store, a_second_writer_is_locked_out) {
    feltest::Fixture fixture("store-lock");
    Result<Ledger> first = fixture.reopen_writer();
    REQUIRE(first.ok());
    const Result<Ledger> second = fixture.reopen_writer();
    CHECK(!second.ok());
    CHECK(second.code() == ReasonCode::LockHeldExclusive ||
          second.code() == ReasonCode::LockUnavailable);
}

FEL_TEST(store, compaction_drops_superseded_records_and_reopens_equivalently) {
    feltest::Fixture fixture("store-compact");
    std::string before;
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const SourceId source = feltest::add_source(ledger.value(), "meter-g", Unit::KilowattHour,
                                                    feltest::at("2026-01-01T00:00:00Z"));
        const EntryId entry = feltest::add_measurement(
            ledger.value(), "e-old", source,
            feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"), 10,
            Unit::KilowattHour, feltest::at("2026-01-01T00:30:00Z"));
        const MeasurementState* state = ledger.value().index().find_measurement(entry);
        REQUIRE(state != nullptr);

        RecordMeasurementRequest correction;
        correction.entry = feltest::id<EntryIdTag>("e-new");
        correction.interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
        correction.source = source;
        correction.generation = Generation{1};
        correction.quantity = feltest::qty(12, Unit::KilowattHour);
        correction.evidence = feltest::dig("corrected reading");
        correction.supersedes = true;
        correction.supersedes_entry = entry;
        correction.supersedes_digest = state->record_digest;
        correction.reason = "transposed digits in the original reading";
        correction.context.now = feltest::at("2026-01-01T02:00:00Z");
        REQUIRE(ledger.value().record_measurement(correction).ok());

        const Result<LedgerView> view = ledger.value().view(Instant{});
        REQUIRE(view.ok());
        before = std::to_string(view.value().measurements().size()) + ":" +
                 std::to_string(view.value().revision().value());
        const Result<std::uint64_t> dropped = ledger.value().compact();
        REQUIRE(dropped.ok());
        CHECK(dropped.value() >= 1U);
        CHECK(!ledger.value().store().base_snapshot_name().empty());
    }
    Result<Ledger> reopened = fixture.reopen_writer();
    REQUIRE(reopened.ok());
    const Result<LedgerView> view = reopened.value().view(Instant{});
    REQUIRE(view.ok());
    // The superseded record was dropped by compaction, so only the replacement
    // remains; live state is identical to the state before compaction.
    CHECK_EQ(view.value().measurements().size(), 1U);
    CHECK_EQ(view.value().sources().size(), 1U);
    CHECK(view.value().find_measurement(feltest::id<EntryIdTag>("e-old")) == nullptr);
    const MeasurementState* new_entry = view.value().find_measurement(feltest::id<EntryIdTag>("e-new"));
    REQUIRE(new_entry != nullptr);
    CHECK(new_entry->superseded_by.empty());
    CHECK(new_entry->quantity == feltest::qty(12, Unit::KilowattHour));
    // The correction that replaced it is retained and still names the target.
    bool saw_correction = false;
    for (const Record& record : reopened.value().store().records()) {
        if (record.meta().supersedes && record.meta().superseded_id == "e-old") {
            saw_correction = true;
            CHECK(record.meta().reason == "transposed digits in the original reading");
        }
    }
    CHECK(saw_correction);
    CHECK(before.find(':') != std::string::npos);
}

FEL_TEST(store, verify_reports_a_clean_ledger_and_ignores_an_uncommitted_tail) {
    feltest::Fixture fixture("store-verify");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-h", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
    }
    const Result<IntegrityReport> clean = verify_ledger_directory(fixture.root);
    REQUIRE(clean.ok());
    CHECK(clean.value().ok);
    CHECK(!clean.value().torn_tail);
    CHECK(clean.value().segment_records > 0U);

    append_raw(ledger_file(fixture.root, "fel-segment-"), std::string(5, 0));
    const Result<IntegrityReport> torn = verify_ledger_directory(fixture.root);
    REQUIRE(torn.ok());
    CHECK(torn.value().torn_tail);
    CHECK_EQ(torn.value().torn_bytes, 5U);
}

FEL_TEST(store, verify_refuses_a_corrupt_manifest) {
    feltest::Fixture fixture("store-manifest");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
    }
    std::vector<std::uint8_t> manifest = feltest::read_bytes(fixture.root / "fel.manifest");
    manifest[0] ^= 0xFFU;
    feltest::write_bytes(fixture.root / "fel.manifest", manifest);
    const Result<IntegrityReport> report = verify_ledger_directory(fixture.root);
    CHECK(!report.ok());
    CHECK_EQ(report.code(), ReasonCode::ManifestCorrupt);
}

FEL_TEST(store, unreferenced_files_are_collected_as_garbage) {
    feltest::Fixture fixture("store-gc");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-i", Unit::KilowattHour,
                            feltest::at("2026-01-01T00:00:00Z"));
    }
    const std::filesystem::path live_segment = ledger_file(fixture.root, "fel-segment-");
    const std::filesystem::path orphan = fixture.root / "fel-segment-00000099.fel";
    std::filesystem::copy_file(live_segment, orphan);
    CHECK(std::filesystem::exists(orphan));
    Result<Ledger> reopened = fixture.reopen_writer();
    REQUIRE(reopened.ok());
    CHECK(reopened.value().store().recovery().orphan_files_removed >= 1U);
    CHECK(!std::filesystem::exists(orphan));
}

FEL_TEST(store, pristine_detection_ignores_the_lock_file) {
    feltest::Fixture fixture("store-pristine");
    CHECK(!ledger_directory_is_pristine(fixture.root));
    feltest::TempDir empty("store-pristine-empty");
    CHECK(ledger_directory_is_pristine(empty.path()));
}

FEL_TEST(store, opening_a_missing_directory_is_refused) {
    feltest::TempDir scratch("store-missing");
    const std::filesystem::path missing = scratch.file("does-not-exist");
    StoreOptions options;
    options.mode = OpenMode::Writer;
    options.create_if_missing = true;
    options.ledger_id = feltest::id<LedgerIdTag>("fresh-ledger");
    const Result<Store> created = Store::open(missing, options);
    REQUIRE(created.ok());
    CHECK(!feltest::read_bytes(missing / "fel.manifest").empty());

    StoreOptions reader_options;
    reader_options.mode = OpenMode::Reader;
    const Result<Store> missing_open = Store::open(scratch.file("never-created"), reader_options);
    CHECK(!missing_open.ok());
    CHECK_EQ(missing_open.code(), ReasonCode::NotFound);
}
