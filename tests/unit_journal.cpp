#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

namespace {

Record sample_measurement() {
    MeasurementRecordedBody body;
    body.entry = feltest::id<EntryIdTag>("e-1");
    body.interval = feltest::between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    body.source = feltest::id<SourceIdTag>("meter-1");
    body.generation = Generation{3};
    body.quantity = Quantity{*Rational::make(-5, 2), Unit::KilowattHour};
    body.subject_kind = SubjectKind::Rack;
    body.subject = "rack-a1";
    body.evidence = feltest::dig("measurement evidence");
    body.recorded_at = feltest::at("2026-01-01T01:05:00Z");
    body.method = "synthetic";
    return Record::make_measurement(body, MutationMeta{});
}

}  // namespace

FEL_TEST(journal, measurement_payload_round_trips) {
    const Record original = sample_measurement();
    const Result<std::vector<std::uint8_t>> payload = encode_record_payload(original);
    REQUIRE(payload.ok());
    const Result<Record> decoded = decode_record_payload(RecordType::MeasurementRecorded, payload.value());
    REQUIRE(decoded.ok());
    const MeasurementRecordedBody& body = decoded.value().measurement();
    CHECK(body.entry == original.measurement().entry);
    CHECK(body.interval == original.measurement().interval);
    CHECK(body.source == original.measurement().source);
    CHECK(body.generation == Generation{3});
    CHECK(body.quantity == original.measurement().quantity);
    CHECK(body.subject_kind == SubjectKind::Rack);
    CHECK(body.subject == "rack-a1");
    CHECK(body.evidence == original.measurement().evidence);
    CHECK(body.method == "synthetic");
    CHECK(original.content_digest() == decoded.value().content_digest());
}

FEL_TEST(journal, every_record_type_round_trips) {
    SegmentHeaderBody segment;
    segment.ledger_id = feltest::id<LedgerIdTag>("ledger-1");
    segment.base_revision = Revision{9};
    segment.base_chain_digest = feltest::dig("base");
    segment.created_at = feltest::at("2026-01-01T00:00:00Z");

    LedgerOpenedBody opened;
    opened.ledger_id = segment.ledger_id;
    opened.opened_at = segment.created_at;
    opened.runtime = "fel-tests";

    EpochAdoptedBody adopted;
    adopted.epoch = Epoch{4};
    adopted.adopted_at = segment.created_at;
    adopted.basis = AdoptionBasis::RecoveredTornTail;
    adopted.chain_head = feltest::dig("chain");
    adopted.detail = "recovered";

    SourceRegisteredBody source;
    source.source = feltest::id<SourceIdTag>("meter-1");
    source.kind = SourceKind::Submeter;
    source.unit = Unit::CubicMeter;
    source.authority = feltest::id<AuthorityIdTag>("authority-dccp");
    source.label = "cooling water";
    source.registered_at = segment.created_at;

    SourceRetiredBody retired;
    retired.source = source.source;
    retired.retired_at = segment.created_at;
    retired.reason = "decommissioned";
    retired.source_digest = feltest::dig("registration");

    GenerationPublishedBody published;
    published.source = source.source;
    published.generation = Generation{2};
    published.published_at = segment.created_at;
    published.evidence = feltest::dig("generation");
    published.valid_until = feltest::at("2026-01-02T00:00:00Z");
    published.method = "synthetic";

    GenerationAttestedBody attested;
    attested.source = source.source;
    attested.generation = Generation{2};
    attested.attested_at = segment.created_at;
    attested.evidence = feltest::dig("attestation");
    attested.valid_until = feltest::at("2026-01-02T00:00:00Z");
    attested.method = "synthetic";

    MeasurementRecordedBody measurement = sample_measurement().measurement();

    ClassificationRecordedBody classification;
    classification.allocation = feltest::id<AllocationIdTag>("a-1");
    classification.entry = measurement.entry;
    classification.klass = ServiceClass::Stranded;
    classification.quantity = Quantity{Rational{7}, Unit::KilowattHour};
    classification.subject_kind = SubjectKind::Unattributed;
    classification.evidence = feltest::dig("classification");
    classification.recorded_at = segment.created_at;
    classification.method = "synthetic";

    ResidualRecordedBody residual;
    residual.residual = feltest::id<ResidualIdTag>("r-1");
    residual.interval = measurement.interval;
    residual.klass = ServiceClass::Unknown;
    residual.quantified = true;
    residual.quantity = Quantity{Rational{3}, Unit::KilowattHour};
    residual.bound_to_entry = true;
    residual.entry = measurement.entry;
    residual.evidence = feltest::dig("residual");
    residual.recorded_at = segment.created_at;
    residual.basis = "sub-meter coverage gap";

    TargetVoidedBody voided;
    voided.void_id = feltest::id<VoidIdTag>("v-1");
    voided.target_kind = TargetKind::Classification;
    voided.target_id = "a-1";
    voided.target_digest = feltest::dig("classification");
    voided.voided_at = segment.created_at;
    voided.reason = "duplicate";

    IntervalSealedBody sealed;
    sealed.seal = feltest::id<SealIdTag>("s-1");
    sealed.interval = measurement.interval;
    sealed.revision_at_seal = Revision{12};
    sealed.closed = true;
    sealed.report_digest = feltest::dig("report");
    sealed.sealed_at = segment.created_at;
    sealed.note = "closed";

    MutationMeta meta;
    meta.idempotency = feltest::id<IdempotencyKeyTag>("key-1");
    meta.request_digest = feltest::dig("request");
    meta.caller_epoch = Epoch{4};
    meta.expected_revision = Revision{3};

    const Record records[] = {
        Record::make_segment_header(segment),
        Record::make_ledger_opened(opened),
        Record::make_epoch_adopted(adopted, Sequence{7}),
        Record::make_source_registered(source, meta),
        Record::make_source_retired(retired, meta),
        Record::make_generation_published(published, meta),
        Record::make_generation_attested(attested, meta),
        Record::make_measurement(measurement, meta),
        Record::make_classification(classification, meta),
        Record::make_residual(residual, meta),
        Record::make_target_voided(voided, meta),
        Record::make_interval_sealed(sealed, meta),
    };
    for (const Record& record : records) {
        const Result<std::vector<std::uint8_t>> payload = encode_record_payload(record);
        REQUIRE(payload.ok());
        const Result<Record> decoded = decode_record_payload(record.type(), payload.value());
        REQUIRE(decoded.ok());
        CHECK(decoded.value().type() == record.type());
        CHECK(decoded.value().content_digest() == record.content_digest());
        if (is_mutation_record(record.type())) {
            CHECK(decoded.value().meta().idempotency == meta.idempotency);
            CHECK(decoded.value().meta().caller_epoch == Epoch{4});
            CHECK(decoded.value().meta().expected_revision == Revision{3});
        } else {
            CHECK(!decoded.value().meta().idempotency.valid());
        }
    }
}

FEL_TEST(journal, supersession_requires_a_complete_declaration) {
    MutationMeta meta;
    meta.supersedes = true;
    const Result<std::vector<std::uint8_t>> missing_target =
        encode_record_payload(Record::make_measurement(sample_measurement().measurement(), meta));
    REQUIRE(missing_target.ok());
    CHECK(decode_record_payload(RecordType::MeasurementRecorded, missing_target.value()).code() ==
          ReasonCode::RecordCorrupt);

    meta.superseded_id = "e-1";
    meta.superseded_digest = feltest::dig("target");
    const Result<std::vector<std::uint8_t>> missing_reason =
        encode_record_payload(Record::make_measurement(sample_measurement().measurement(), meta));
    REQUIRE(missing_reason.ok());
    CHECK(decode_record_payload(RecordType::MeasurementRecorded, missing_reason.value()).code() ==
          ReasonCode::RecordCorrupt);

    // A record that does not supersede anything must not carry supersession
    // fields either.
    MutationMeta stray;
    stray.reason = "not a correction";
    const Result<std::vector<std::uint8_t>> stray_payload =
        encode_record_payload(Record::make_measurement(sample_measurement().measurement(), stray));
    REQUIRE(stray_payload.ok());
    CHECK(decode_record_payload(RecordType::MeasurementRecorded, stray_payload.value()).code() ==
          ReasonCode::RecordCorrupt);
}

FEL_TEST(journal, unknown_enumerators_are_refused) {
    Record record = sample_measurement();
    const Result<std::vector<std::uint8_t>> payload = encode_record_payload(record);
    REQUIRE(payload.ok());
    std::vector<std::uint8_t> bytes = payload.value();
    // The service class is the only enum in the measurement payload; corrupt the
    // subject kind, which appears after the quantity.
    const std::string subject_text = "rack-a1";
    std::size_t position = std::string::npos;
    for (std::size_t index = 0; index + subject_text.size() <= bytes.size(); ++index) {
        if (std::memcmp(bytes.data() + index, subject_text.data(), subject_text.size()) == 0) {
            position = index;
            break;
        }
    }
    REQUIRE(position != std::string::npos);
    // The subject kind is one byte, written immediately before the
    // length-prefixed subject text.
    REQUIRE(position >= 5U);
    CHECK_EQ(bytes[position - 5U], static_cast<std::uint8_t>(SubjectKind::Rack));
    bytes[position - 5U] = 200U;
    CHECK(decode_record_payload(RecordType::MeasurementRecorded, bytes).code() ==
          ReasonCode::RecordCorrupt);
}

FEL_TEST(journal, frame_headers_are_checksummed) {
    Record record = sample_measurement();
    record.set_sequence(Sequence{5});
    record.set_revision(Revision{2});
    record.set_epoch(Epoch{1});
    const EncodedFrame frame = encode_record_frame(record, feltest::dig("previous"));
    CHECK(frame.bytes.size() == kRecordOverhead + frame.header.payload_length);

    const Result<FrameHeader> decoded =
        decode_frame_header(std::span<const std::uint8_t>{frame.bytes.data(), kRecordHeaderSize});
    REQUIRE(decoded.ok());
    CHECK(decoded.value().sequence == Sequence{5});
    CHECK(decoded.value().revision == Revision{2});
    CHECK(decoded.value().epoch == Epoch{1});
    CHECK(decoded.value().type == RecordType::MeasurementRecorded);
    CHECK_EQ(decoded.value().payload_crc, crc32c(std::span<const std::uint8_t>{
                                              frame.bytes.data() + kRecordHeaderSize,
                                              frame.header.payload_length}));

    // Every single-byte change inside the header is detected.
    for (std::size_t index = 0; index < kRecordHeaderSize; ++index) {
        std::vector<std::uint8_t> tampered(frame.bytes.begin(), frame.bytes.begin() + kRecordHeaderSize);
        tampered[index] ^= 0x01U;
        CHECK(!decode_frame_header(tampered).ok());
    }
    // Magic and format version are validated explicitly.
    std::vector<std::uint8_t> bad_magic(frame.bytes.begin(),
                                        frame.bytes.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize));
    bad_magic[0] ^= 0xFFU;
    CHECK(!decode_frame_header(bad_magic).ok());
}

FEL_TEST(journal, chain_digest_covers_the_predecessor_and_the_payload) {
    Record record = sample_measurement();
    record.set_sequence(Sequence{1});
    const EncodedFrame first = encode_record_frame(record, Digest{});
    const EncodedFrame second = encode_record_frame(record, feltest::dig("other"));
    CHECK(first.chain_digest != second.chain_digest);

    MeasurementRecordedBody altered_body = sample_measurement().measurement();
    altered_body.quantity = Quantity{Rational{999}, Unit::KilowattHour};
    const Record altered = Record::make_measurement(altered_body, MutationMeta{});
    const EncodedFrame third = encode_record_frame(altered, Digest{});
    CHECK(third.chain_digest != first.chain_digest);
}

FEL_TEST(journal, mutation_classification_is_explicit) {
    CHECK(is_mutation_record(RecordType::MeasurementRecorded));
    CHECK(is_mutation_record(RecordType::ClassificationRecorded));
    CHECK(is_mutation_record(RecordType::ResidualRecorded));
    CHECK(is_mutation_record(RecordType::IntervalSealed));
    CHECK(is_mutation_record(RecordType::TargetVoided));
    CHECK(!is_mutation_record(RecordType::SegmentHeader));
    CHECK(!is_mutation_record(RecordType::LedgerOpened));
    CHECK(!is_mutation_record(RecordType::EpochAdopted));
}

FEL_TEST(journal, reason_code_names_round_trip) {
    const ReasonCode codes[] = {ReasonCode::Ok,
                                ReasonCode::InteriorCorruption,
                                ReasonCode::RecoveredNotCurrent,
                                ReasonCode::AccountingNotClosed,
                                ReasonCode::AuthorityConflicting};
    for (ReasonCode code : codes) {
        const auto parsed = reason_code_from_string(to_string(code));
        REQUIRE(parsed.has_value());
        CHECK(parsed.value() == code);
    }
    const Reason reason{ReasonCode::OverAllocation, "classifications exceed the measurement"};
    CHECK(reason.message() == "OverAllocation: classifications exceed the measurement");
}
