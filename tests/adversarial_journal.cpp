#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

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

struct Boundary {
    std::uint64_t offset;
    std::uint64_t end;
};

// Record boundaries derived from the framing itself, so the test does not need
// to know anything about record contents.
std::vector<Boundary> record_boundaries(const std::vector<std::uint8_t>& bytes) {
    std::vector<Boundary> boundaries;
    std::uint64_t offset = 0;
    while (offset + kRecordHeaderSize <= bytes.size()) {
        const Result<FrameHeader> header = decode_frame_header(
            std::span<const std::uint8_t>{bytes.data() + offset, kRecordHeaderSize});
        if (!header.ok()) {
            break;
        }
        const std::uint64_t end = offset + kRecordOverhead + header.value().payload_length;
        if (end > bytes.size()) {
            break;
        }
        boundaries.push_back(Boundary{offset, end});
        offset = end;
    }
    return boundaries;
}

void build_small_ledger(feltest::Fixture& fixture) {
    Result<Ledger> ledger = fixture.reopen_writer();
    if (!ledger.ok()) {
        throw feltest::Failure{"cannot open the fixture: " + ledger.reason().message()};
    }
    const Instant base = feltest::at("2026-08-01T00:00:00Z");
    const SourceId source = feltest::add_source(ledger.value(), "meter-adv", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(
        ledger.value(), "e-adv", source,
        feltest::between("2026-08-01T00:00:00Z", "2026-08-01T01:00:00Z"), 10, Unit::KilowattHour, base);
    feltest::add_classification(ledger.value(), "a-adv", entry, ServiceClass::Useful, 10,
                                Unit::KilowattHour, base);
}

}  // namespace

FEL_TEST(adversarial, every_truncation_of_the_segment_is_recovered_exactly) {
    feltest::Fixture fixture("adv-truncate");
    build_small_ledger(fixture);
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);
    const std::vector<Boundary> boundaries = record_boundaries(original);
    REQUIRE(boundaries.size() >= 5U);
    CHECK(boundaries.front().offset == 0U);

    for (std::uint64_t length = 0; length <= original.size(); ++length) {
        feltest::write_bytes(segment, std::vector<std::uint8_t>(
                                          original.begin(),
                                          original.begin() + static_cast<std::ptrdiff_t>(length)));
        std::size_t complete = 0;
        for (const Boundary& boundary : boundaries) {
            if (boundary.end <= length) {
                complete += 1U;
            }
        }
        {
            // The write handle must be released before verification, because a
            // verifier takes a shared lock and a live writer holds it exclusively.
            const Result<Ledger> opened = fixture.reopen_writer();
            if (complete == 0U) {
                // Not one whole record survived, so there is nothing to recover.
                CHECK(!opened.ok());
                continue;
            }
            if (!opened.ok()) {
                throw feltest::Failure{"truncating to " + std::to_string(length) +
                                       " bytes with " + std::to_string(complete) +
                                       " whole records failed: " + opened.reason().message()};
            }
            CHECK(opened.value().store().records().size() >= complete);
            const bool exactly_on_boundary = boundaries[complete - 1U].end == length;
            CHECK(opened.value().store().recovery().recovered_torn_tail == !exactly_on_boundary);
            if (!exactly_on_boundary) {
                CHECK_EQ(opened.value().store().recovery().truncated_bytes,
                         length - boundaries[complete - 1U].end);
            }
        }
        // After recovery the writer has left the journal at a committed
        // boundary, so a verification finds no torn tail.
        const Result<IntegrityReport> integrity = verify_ledger_directory(fixture.root);
        REQUIRE(integrity.ok());
        CHECK(!integrity.value().torn_tail);
    }
    feltest::write_bytes(segment, original);
    const Result<Ledger> restored = fixture.reopen_writer();
    REQUIRE(restored.ok());
    CHECK(!restored.value().store().recovery().recovered_torn_tail);
}

FEL_TEST(adversarial, every_single_bit_flip_is_detected) {
    feltest::Fixture fixture("adv-bitflip");
    build_small_ledger(fixture);
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);
    REQUIRE(original.size() > 400U);
    REQUIRE(original.size() < 4000U);

    for (std::size_t offset = 0; offset < original.size(); ++offset) {
        std::vector<std::uint8_t> tampered = original;
        tampered[offset] ^= 0x01U;
        feltest::write_bytes(segment, tampered);
        const Result<Ledger> opened = fixture.reopen_writer();
        if (opened.ok()) {
            throw feltest::Failure{"a single-bit flip at offset " + std::to_string(offset) +
                                   " was not detected"};
        }
        CHECK(!opened.reason().message().empty());
    }
    feltest::write_bytes(segment, original);
    CHECK(fixture.reopen_writer().ok());
}

FEL_TEST(adversarial, a_removed_or_reordered_record_breaks_the_chain) {
    feltest::Fixture fixture("adv-removed");
    build_small_ledger(fixture);
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);
    const std::vector<Boundary> boundaries = record_boundaries(original);
    REQUIRE(boundaries.size() >= 4U);
    const std::uint64_t start = boundaries[2].offset;
    const std::uint64_t end = boundaries[3].offset;

    std::vector<std::uint8_t> spliced(original.begin(),
                                      original.begin() + static_cast<std::ptrdiff_t>(start));
    spliced.insert(spliced.end(), original.begin() + static_cast<std::ptrdiff_t>(end), original.end());
    feltest::write_bytes(segment, spliced);
    const Result<Ledger> removed = fixture.reopen_writer();
    CHECK(!removed.ok());
    const ReasonCode removed_code = removed.code();
    CHECK(removed_code == ReasonCode::ChainBroken ||
          removed_code == ReasonCode::InteriorCorruption ||
          removed_code == ReasonCode::RecordCorrupt || removed_code == ReasonCode::HeaderCorrupt);
    CHECK(feltest::read_bytes(segment) == spliced);

    std::vector<std::uint8_t> reordered(original.begin(),
                                        original.begin() + static_cast<std::ptrdiff_t>(start));
    reordered.insert(reordered.end(), original.begin() + static_cast<std::ptrdiff_t>(end),
                     original.end());
    reordered.insert(reordered.end(), original.begin() + static_cast<std::ptrdiff_t>(start),
                     original.begin() + static_cast<std::ptrdiff_t>(end));
    feltest::write_bytes(segment, reordered);
    CHECK(!fixture.reopen_writer().ok());

    feltest::write_bytes(segment, original);
    CHECK(fixture.reopen_writer().ok());
}

FEL_TEST(adversarial, a_corrupt_snapshot_is_refused_rather_than_half_read) {
    feltest::Fixture fixture("adv-snapshot");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-snap", Unit::KilowattHour,
                            feltest::at("2026-08-01T00:00:00Z"));
        REQUIRE(ledger.value().compact().ok());
        CHECK(!ledger.value().store().base_snapshot_name().empty());
    }
    const std::filesystem::path snapshot = fixture.root / "fel-snapshot-00000001.fel";
    REQUIRE(std::filesystem::exists(snapshot));
    const std::vector<std::uint8_t> original = feltest::read_bytes(snapshot);

    for (std::size_t offset = 0; offset < original.size(); offset += 7U) {
        std::vector<std::uint8_t> tampered = original;
        tampered[offset] ^= 0x80U;
        feltest::write_bytes(snapshot, tampered);
        const Result<Ledger> opened = fixture.reopen_reader();
        if (opened.ok()) {
            throw feltest::Failure{"a snapshot bit flip at offset " + std::to_string(offset) +
                                   " was not detected"};
        }
    }
    feltest::write_bytes(snapshot, original);
    CHECK(fixture.reopen_reader().ok());

    // A truncated snapshot is refused as corrupt rather than partially applied.
    feltest::write_bytes(snapshot, std::vector<std::uint8_t>(original.begin(), original.begin() + 20));
    const Result<Ledger> truncated = fixture.reopen_reader();
    CHECK(!truncated.ok());
    CHECK_EQ(truncated.code(), ReasonCode::SnapshotCorrupt);
    feltest::write_bytes(snapshot, original);
    CHECK(fixture.reopen_reader().ok());
}

FEL_TEST(adversarial, missing_files_are_reported_precisely) {
    feltest::Fixture fixture("adv-missing");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-missing", Unit::KilowattHour,
                            feltest::at("2026-08-01T00:00:00Z"));
    }
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::vector<std::uint8_t> segment_bytes = feltest::read_bytes(segment);
    std::filesystem::remove(segment);
    const Result<Ledger> missing_segment = fixture.reopen_reader();
    CHECK(!missing_segment.ok());
    CHECK_EQ(missing_segment.code(), ReasonCode::SegmentMissing);
    feltest::write_bytes(segment, segment_bytes);

    const std::filesystem::path manifest = fixture.root / "fel.manifest";
    const std::vector<std::uint8_t> manifest_bytes = feltest::read_bytes(manifest);
    std::filesystem::remove(manifest);
    const Result<Ledger> missing_manifest = fixture.reopen_reader();
    CHECK(!missing_manifest.ok());
    CHECK_EQ(missing_manifest.code(), ReasonCode::ManifestMissing);
    feltest::write_bytes(manifest, manifest_bytes);
    CHECK(fixture.reopen_reader().ok());
}

FEL_TEST(adversarial, an_empty_segment_is_refused) {
    feltest::Fixture fixture("adv-empty");
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
    }
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::vector<std::uint8_t> original = feltest::read_bytes(segment);
    feltest::write_bytes(segment, {});
    const Result<Ledger> opened = fixture.reopen_reader();
    CHECK(!opened.ok());
    CHECK_EQ(opened.code(), ReasonCode::InteriorCorruption);
    feltest::write_bytes(segment, original);
    CHECK(fixture.reopen_reader().ok());
}

FEL_TEST(adversarial, an_uncommitted_snapshot_is_ignored_and_collected) {
    feltest::Fixture fixture("adv-orphan-snapshot");
    std::uint64_t expected_revision = 0;
    {
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        feltest::add_source(ledger.value(), "meter-orphan", Unit::KilowattHour,
                            feltest::at("2026-08-01T00:00:00Z"));
        REQUIRE(ledger.value().compact().ok());
        expected_revision = ledger.value().store().revision().value();
    }
    // Two files that look like a completed compaction whose manifest publication
    // never happened.
    const std::filesystem::path snapshot = fixture.root / "fel-snapshot-00000001.fel";
    const std::filesystem::path segment = segment_path(fixture.root);
    const std::filesystem::path orphan_snapshot = fixture.root / "fel-snapshot-00000009.fel";
    const std::filesystem::path orphan_segment = fixture.root / "fel-segment-00000009.fel";
    std::filesystem::copy_file(snapshot, orphan_snapshot);
    std::filesystem::copy_file(segment, orphan_segment);

    Result<Ledger> reopened = fixture.reopen_writer();
    REQUIRE(reopened.ok());
    // The manifest is the commit point: an unpublished compaction is ignored, so
    // the accounting revision is exactly what it was before the orphan appeared.
    CHECK_EQ(reopened.value().store().revision().value(), expected_revision);
    CHECK(reopened.value().index().sources().size() == 1U);
    CHECK(reopened.value().store().recovery().orphan_files_removed >= 2U);
    CHECK(!std::filesystem::exists(orphan_snapshot));
    CHECK(!std::filesystem::exists(orphan_segment));
}
