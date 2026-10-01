#include "fel/journal.hpp"

#include <array>
#include <string>
#include <utility>

namespace fel {
namespace {

template <class Tag>
Result<Identifier<Tag>> identifier_or_empty(std::string_view text) {
    if (text.empty()) {
        return Identifier<Tag>{};
    }
    return Identifier<Tag>::parse(text);
}

Result<Unit> decode_unit(Reader& reader) { return reader.unit(); }

template <class Enum>
void write_enum(Writer& writer, Enum value) {
    writer.u8(static_cast<std::uint8_t>(value));
}

template <class Enum, std::size_t N>
Result<Enum> read_enum(Reader& reader, const std::array<Enum, N>& permitted, std::string_view what) {
    const Result<std::uint8_t> raw = reader.u8();
    if (!raw.ok()) {
        return raw.reason();
    }
    for (Enum candidate : permitted) {
        if (static_cast<std::uint8_t>(candidate) == raw.value()) {
            return candidate;
        }
    }
    return Reason{ReasonCode::RecordCorrupt,
                  "encoded " + std::string(what) + " value " + std::to_string(raw.value()) +
                      " is not a known enumerator"};
}

constexpr std::array<RecordType, 12> kRecordTypes{
    RecordType::SegmentHeader,       RecordType::LedgerOpened,
    RecordType::EpochAdopted,        RecordType::SourceRegistered,
    RecordType::SourceRetired,       RecordType::GenerationPublished,
    RecordType::GenerationAttested,  RecordType::MeasurementRecorded,
    RecordType::ClassificationRecorded, RecordType::ResidualRecorded,
    RecordType::TargetVoided,        RecordType::IntervalSealed};

constexpr std::array<SourceKind, 5> kSourceKinds{SourceKind::Meter, SourceKind::Submeter,
                                                 SourceKind::Estimate, SourceKind::Allocation,
                                                 SourceKind::ExternalAuthority};

constexpr std::array<ServiceClass, 6> kServiceClasses{
    ServiceClass::Useful,    ServiceClass::Avoidable, ServiceClass::Stranded,
    ServiceClass::Wasted,    ServiceClass::Unknown,   ServiceClass::Unmeasured};

constexpr std::array<SubjectKind, 9> kSubjectKinds{
    SubjectKind::Unattributed, SubjectKind::Facility, SubjectKind::Zone,     SubjectKind::Hall,
    SubjectKind::Row,          SubjectKind::Rack,     SubjectKind::Device,   SubjectKind::Tenant,
    SubjectKind::Workload};

constexpr std::array<TargetKind, 4> kTargetKinds{TargetKind::Measurement, TargetKind::Classification,
                                                 TargetKind::Residual,
                                                 TargetKind::SourceRegistration};

constexpr std::array<AdoptionBasis, 4> kAdoptionBases{
    AdoptionBasis::InitialCreation, AdoptionBasis::CleanReopen,
    AdoptionBasis::RecoveredTornTail, AdoptionBasis::Compaction};

void write_subject(Writer& writer, SubjectKind kind, std::string_view subject) {
    write_enum(writer, kind);
    writer.string(subject);
}

Result<std::pair<SubjectKind, std::string>> read_subject(Reader& reader) {
    const Result<SubjectKind> kind = read_enum(reader, kSubjectKinds, "subject kind");
    if (!kind.ok()) {
        return kind.reason();
    }
    const Result<std::string_view> subject = reader.string();
    if (!subject.ok()) {
        return subject.reason();
    }
    if (kind.value() == SubjectKind::Unattributed) {
        if (!subject.value().empty()) {
            return Reason{ReasonCode::RecordCorrupt,
                          "unattributed subject carries a subject name"};
        }
    } else if (!is_valid_identifier_text(subject.value())) {
        return Reason{ReasonCode::RecordCorrupt,
                      "attributed subject name is not a valid identifier"};
    }
    return std::pair<SubjectKind, std::string>{kind.value(), std::string{subject.value()}};
}

void write_meta(Writer& writer, const MutationMeta& meta) {
    writer.string(meta.idempotency.str());
    writer.digest(meta.request_digest);
    writer.boolean(meta.supersedes);
    writer.string(meta.superseded_id);
    writer.digest(meta.superseded_digest);
    writer.string(meta.reason);
    writer.u64(meta.caller_epoch.value());
    writer.u64(meta.expected_revision.value());
}

Result<MutationMeta> read_meta(Reader& reader) {
    MutationMeta meta;
    const Result<std::string_view> idempotency = reader.string();
    if (!idempotency.ok()) {
        return idempotency.reason();
    }
    const Result<IdempotencyKey> key = identifier_or_empty<IdempotencyKeyTag>(idempotency.value());
    if (!key.ok()) {
        return Reason{ReasonCode::RecordCorrupt, "idempotency key is not a valid identifier"};
    }
    meta.idempotency = key.value();

    const Result<Digest> request_digest = reader.digest();
    if (!request_digest.ok()) {
        return request_digest.reason();
    }
    meta.request_digest = request_digest.value();

    const Result<bool> supersedes = reader.boolean();
    if (!supersedes.ok()) {
        return supersedes.reason();
    }
    meta.supersedes = supersedes.value();

    const Result<std::string_view> superseded_id = reader.string();
    if (!superseded_id.ok()) {
        return superseded_id.reason();
    }
    meta.superseded_id.assign(superseded_id.value());

    const Result<Digest> superseded_digest = reader.digest();
    if (!superseded_digest.ok()) {
        return superseded_digest.reason();
    }
    meta.superseded_digest = superseded_digest.value();

    const Result<std::string_view> reason = reader.string();
    if (!reason.ok()) {
        return reason.reason();
    }
    meta.reason.assign(reason.value());

    const Result<std::uint64_t> caller_epoch = reader.u64();
    if (!caller_epoch.ok()) {
        return caller_epoch.reason();
    }
    meta.caller_epoch = Epoch{caller_epoch.value()};

    const Result<std::uint64_t> expected_revision = reader.u64();
    if (!expected_revision.ok()) {
        return expected_revision.reason();
    }
    meta.expected_revision = Revision{expected_revision.value()};

    if (meta.supersedes) {
        if (meta.superseded_id.empty() || !is_valid_identifier_text(meta.superseded_id)) {
            return Reason{ReasonCode::RecordCorrupt, "supersession names no valid target"};
        }
        if (meta.superseded_digest.is_zero()) {
            return Reason{ReasonCode::RecordCorrupt, "supersession carries no target digest"};
        }
        if (meta.reason.empty()) {
            return Reason{ReasonCode::RecordCorrupt, "supersession carries no reason"};
        }
    } else if (!meta.superseded_id.empty() || !meta.superseded_digest.is_zero() ||
               !meta.reason.empty()) {
        return Reason{ReasonCode::RecordCorrupt,
                      "non-superseding record carries supersession fields"};
    }
    return meta;
}

template <class Tag>
void write_identifier(Writer& writer, const Identifier<Tag>& value) { writer.string(value.str()); }

template <class Tag>
Result<Identifier<Tag>> read_identifier(Reader& reader, bool required, std::string_view what) {
    const Result<std::string_view> text = reader.string();
    if (!text.ok()) {
        return text.reason();
    }
    if (text.value().empty()) {
        if (required) {
            return Reason{ReasonCode::RecordCorrupt, std::string(what) + " is empty"};
        }
        return Identifier<Tag>{};
    }
    const Result<Identifier<Tag>> id = Identifier<Tag>::parse(text.value());
    if (!id.ok()) {
        return Reason{ReasonCode::RecordCorrupt, std::string(what) + " is not a valid identifier"};
    }
    return id.value();
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
    switch (type) {
        case RecordType::SegmentHeader: return "SegmentHeader";
        case RecordType::LedgerOpened: return "LedgerOpened";
        case RecordType::EpochAdopted: return "EpochAdopted";
        case RecordType::SourceRegistered: return "SourceRegistered";
        case RecordType::SourceRetired: return "SourceRetired";
        case RecordType::GenerationPublished: return "GenerationPublished";
        case RecordType::GenerationAttested: return "GenerationAttested";
        case RecordType::MeasurementRecorded: return "MeasurementRecorded";
        case RecordType::ClassificationRecorded: return "ClassificationRecorded";
        case RecordType::ResidualRecorded: return "ResidualRecorded";
        case RecordType::TargetVoided: return "TargetVoided";
        case RecordType::IntervalSealed: return "IntervalSealed";
    }
    return "UnknownRecordType";
}

std::optional<RecordType> record_type_from_string(std::string_view name) noexcept {
    for (RecordType type : kRecordTypes) {
        if (to_string(type) == name) {
            return type;
        }
    }
    return std::nullopt;
}

bool is_mutation_record(RecordType type) noexcept {
    switch (type) {
        case RecordType::SourceRegistered:
        case RecordType::SourceRetired:
        case RecordType::GenerationPublished:
        case RecordType::GenerationAttested:
        case RecordType::MeasurementRecorded:
        case RecordType::ClassificationRecorded:
        case RecordType::ResidualRecorded:
        case RecordType::TargetVoided:
        case RecordType::IntervalSealed:
            return true;
        case RecordType::SegmentHeader:
        case RecordType::LedgerOpened:
        case RecordType::EpochAdopted:
            return false;
    }
    return false;
}

std::string_view to_string(SourceKind kind) noexcept {
    switch (kind) {
        case SourceKind::Meter: return "meter";
        case SourceKind::Submeter: return "submeter";
        case SourceKind::Estimate: return "estimate";
        case SourceKind::Allocation: return "allocation";
        case SourceKind::ExternalAuthority: return "external_authority";
    }
    return "unknown";
}

std::optional<SourceKind> source_kind_from_string(std::string_view name) noexcept {
    for (SourceKind kind : kSourceKinds) {
        if (to_string(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view to_string(ServiceClass klass) noexcept {
    switch (klass) {
        case ServiceClass::Useful: return "useful";
        case ServiceClass::Avoidable: return "avoidable";
        case ServiceClass::Stranded: return "stranded";
        case ServiceClass::Wasted: return "wasted";
        case ServiceClass::Unknown: return "unknown";
        case ServiceClass::Unmeasured: return "unmeasured";
    }
    return "unknown_class";
}

std::optional<ServiceClass> service_class_from_string(std::string_view name) noexcept {
    for (ServiceClass klass : kServiceClasses) {
        if (to_string(klass) == name) {
            return klass;
        }
    }
    return std::nullopt;
}

bool is_residual_class(ServiceClass klass) noexcept {
    return klass == ServiceClass::Unknown || klass == ServiceClass::Unmeasured;
}

bool is_consumption_class(ServiceClass klass) noexcept {
    return klass == ServiceClass::Useful || klass == ServiceClass::Avoidable ||
           klass == ServiceClass::Stranded || klass == ServiceClass::Wasted;
}

std::string_view to_string(SubjectKind kind) noexcept {
    switch (kind) {
        case SubjectKind::Unattributed: return "unattributed";
        case SubjectKind::Facility: return "facility";
        case SubjectKind::Zone: return "zone";
        case SubjectKind::Hall: return "hall";
        case SubjectKind::Row: return "row";
        case SubjectKind::Rack: return "rack";
        case SubjectKind::Device: return "device";
        case SubjectKind::Tenant: return "tenant";
        case SubjectKind::Workload: return "workload";
    }
    return "unknown";
}

std::optional<SubjectKind> subject_kind_from_string(std::string_view name) noexcept {
    for (SubjectKind kind : kSubjectKinds) {
        if (to_string(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view to_string(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::Measurement: return "measurement";
        case TargetKind::Classification: return "classification";
        case TargetKind::Residual: return "residual";
        case TargetKind::SourceRegistration: return "source_registration";
    }
    return "unknown";
}

std::string_view to_string(AdoptionBasis basis) noexcept {
    switch (basis) {
        case AdoptionBasis::InitialCreation: return "initial_creation";
        case AdoptionBasis::CleanReopen: return "clean_reopen";
        case AdoptionBasis::RecoveredTornTail: return "recovered_torn_tail";
        case AdoptionBasis::Compaction: return "compaction";
    }
    return "unknown";
}

Record Record::make_segment_header(const SegmentHeaderBody& body) {
    Record record;
    record.type_ = RecordType::SegmentHeader;
    record.segment_header_ = body;
    return record;
}

Record Record::make_ledger_opened(const LedgerOpenedBody& body) {
    Record record;
    record.type_ = RecordType::LedgerOpened;
    record.ledger_opened_ = body;
    return record;
}

Record Record::make_epoch_adopted(const EpochAdoptedBody& body, Sequence sequence) {
    Record record;
    record.type_ = RecordType::EpochAdopted;
    record.epoch_adopted_ = body;
    record.sequence_ = sequence;
    return record;
}

Record Record::make_source_registered(const SourceRegisteredBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::SourceRegistered;
    record.source_registered_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_source_retired(const SourceRetiredBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::SourceRetired;
    record.source_retired_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_generation_published(const GenerationPublishedBody& body,
                                         const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::GenerationPublished;
    record.generation_published_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_generation_attested(const GenerationAttestedBody& body,
                                        const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::GenerationAttested;
    record.generation_attested_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_measurement(const MeasurementRecordedBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::MeasurementRecorded;
    record.measurement_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_classification(const ClassificationRecordedBody& body,
                                   const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::ClassificationRecorded;
    record.classification_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_residual(const ResidualRecordedBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::ResidualRecorded;
    record.residual_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_target_voided(const TargetVoidedBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::TargetVoided;
    record.target_voided_ = body;
    record.meta_ = meta;
    return record;
}

Record Record::make_interval_sealed(const IntervalSealedBody& body, const MutationMeta& meta) {
    Record record;
    record.type_ = RecordType::IntervalSealed;
    record.interval_sealed_ = body;
    record.meta_ = meta;
    return record;
}

Result<std::vector<std::uint8_t>> encode_record_payload(const Record& record) {
    Writer writer;
    switch (record.type_) {
        case RecordType::SegmentHeader: {
            const SegmentHeaderBody& body = record.segment_header_;
            write_identifier(writer, body.ledger_id);
            writer.u64(body.base_revision.value());
            writer.digest(body.base_chain_digest);
            writer.instant(body.created_at);
            break;
        }
        case RecordType::LedgerOpened: {
            const LedgerOpenedBody& body = record.ledger_opened_;
            write_identifier(writer, body.ledger_id);
            writer.instant(body.opened_at);
            writer.string(body.runtime);
            break;
        }
        case RecordType::EpochAdopted: {
            const EpochAdoptedBody& body = record.epoch_adopted_;
            writer.u64(body.epoch.value());
            writer.instant(body.adopted_at);
            write_enum(writer, body.basis);
            writer.digest(body.chain_head);
            writer.string(body.detail);
            break;
        }
        case RecordType::SourceRegistered: {
            const SourceRegisteredBody& body = record.source_registered_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.source);
            write_enum(writer, body.kind);
            writer.unit(body.unit);
            write_identifier(writer, body.authority);
            writer.string(body.label);
            writer.instant(body.registered_at);
            break;
        }
        case RecordType::SourceRetired: {
            const SourceRetiredBody& body = record.source_retired_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.source);
            writer.instant(body.retired_at);
            writer.string(body.reason);
            writer.digest(body.source_digest);
            break;
        }
        case RecordType::GenerationPublished: {
            const GenerationPublishedBody& body = record.generation_published_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.source);
            writer.u64(body.generation.value());
            writer.instant(body.published_at);
            writer.digest(body.evidence);
            writer.instant(body.valid_until);
            writer.string(body.method);
            break;
        }
        case RecordType::GenerationAttested: {
            const GenerationAttestedBody& body = record.generation_attested_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.source);
            writer.u64(body.generation.value());
            writer.instant(body.attested_at);
            writer.digest(body.evidence);
            writer.instant(body.valid_until);
            writer.string(body.method);
            break;
        }
        case RecordType::MeasurementRecorded: {
            const MeasurementRecordedBody& body = record.measurement_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.entry);
            writer.interval(body.interval);
            write_identifier(writer, body.source);
            writer.u64(body.generation.value());
            writer.quantity(body.quantity);
            write_subject(writer, body.subject_kind, body.subject);
            writer.digest(body.evidence);
            writer.instant(body.recorded_at);
            writer.string(body.method);
            break;
        }
        case RecordType::ClassificationRecorded: {
            const ClassificationRecordedBody& body = record.classification_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.allocation);
            write_identifier(writer, body.entry);
            write_enum(writer, body.klass);
            writer.quantity(body.quantity);
            write_subject(writer, body.subject_kind, body.subject);
            writer.digest(body.evidence);
            writer.instant(body.recorded_at);
            writer.string(body.method);
            break;
        }
        case RecordType::ResidualRecorded: {
            const ResidualRecordedBody& body = record.residual_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.residual);
            writer.interval(body.interval);
            write_enum(writer, body.klass);
            writer.boolean(body.quantified);
            writer.quantity(body.quantity);
            writer.boolean(body.bound_to_entry);
            write_identifier(writer, body.entry);
            write_subject(writer, body.subject_kind, body.subject);
            writer.digest(body.evidence);
            writer.instant(body.recorded_at);
            writer.string(body.basis);
            break;
        }
        case RecordType::TargetVoided: {
            const TargetVoidedBody& body = record.target_voided_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.void_id);
            write_enum(writer, body.target_kind);
            writer.string(body.target_id);
            writer.digest(body.target_digest);
            writer.instant(body.voided_at);
            writer.string(body.reason);
            break;
        }
        case RecordType::IntervalSealed: {
            const IntervalSealedBody& body = record.interval_sealed_;
            write_meta(writer, record.meta_);
            write_identifier(writer, body.seal);
            writer.interval(body.interval);
            writer.u64(body.revision_at_seal.value());
            writer.boolean(body.closed);
            writer.digest(body.report_digest);
            writer.instant(body.sealed_at);
            writer.string(body.note);
            break;
        }
    }
    return writer.take();
}

Result<Record> decode_record_payload(RecordType type, std::span<const std::uint8_t> payload) {
    Reader reader{payload};
    Record record;
    record.type_ = type;

    const auto finish = [&reader](Record& candidate) -> Result<Record> {
        if (!reader.at_end()) {
            return Reason{ReasonCode::RecordCorrupt,
                          "record payload has " + std::to_string(reader.remaining()) +
                              " trailing bytes"};
        }
        return candidate;
    };

    switch (type) {
        case RecordType::SegmentHeader: {
            SegmentHeaderBody body;
            const Result<LedgerId> ledger = read_identifier<LedgerIdTag>(reader, true, "ledger id");
            if (!ledger.ok()) return ledger.reason();
            body.ledger_id = ledger.value();
            const Result<std::uint64_t> base_revision = reader.u64();
            if (!base_revision.ok()) return base_revision.reason();
            body.base_revision = Revision{base_revision.value()};
            const Result<Digest> base_chain = reader.digest();
            if (!base_chain.ok()) return base_chain.reason();
            body.base_chain_digest = base_chain.value();
            const Result<Instant> created_at = reader.instant();
            if (!created_at.ok()) return created_at.reason();
            body.created_at = created_at.value();
            record.segment_header_ = body;
            return finish(record);
        }
        case RecordType::LedgerOpened: {
            LedgerOpenedBody body;
            const Result<LedgerId> ledger = read_identifier<LedgerIdTag>(reader, true, "ledger id");
            if (!ledger.ok()) return ledger.reason();
            body.ledger_id = ledger.value();
            const Result<Instant> opened_at = reader.instant();
            if (!opened_at.ok()) return opened_at.reason();
            body.opened_at = opened_at.value();
            const Result<std::string_view> runtime = reader.string();
            if (!runtime.ok()) return runtime.reason();
            body.runtime.assign(runtime.value());
            record.ledger_opened_ = body;
            return finish(record);
        }
        case RecordType::EpochAdopted: {
            EpochAdoptedBody body;
            const Result<std::uint64_t> epoch = reader.u64();
            if (!epoch.ok()) return epoch.reason();
            body.epoch = Epoch{epoch.value()};
            const Result<Instant> adopted_at = reader.instant();
            if (!adopted_at.ok()) return adopted_at.reason();
            body.adopted_at = adopted_at.value();
            const Result<AdoptionBasis> basis = read_enum(reader, kAdoptionBases, "adoption basis");
            if (!basis.ok()) return basis.reason();
            body.basis = basis.value();
            const Result<Digest> chain_head = reader.digest();
            if (!chain_head.ok()) return chain_head.reason();
            body.chain_head = chain_head.value();
            const Result<std::string_view> detail = reader.string();
            if (!detail.ok()) return detail.reason();
            body.detail.assign(detail.value());
            record.epoch_adopted_ = body;
            return finish(record);
        }
        case RecordType::SourceRegistered: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            SourceRegisteredBody body;
            const Result<SourceId> source = read_identifier<SourceIdTag>(reader, true, "source id");
            if (!source.ok()) return source.reason();
            body.source = source.value();
            const Result<SourceKind> kind = read_enum(reader, kSourceKinds, "source kind");
            if (!kind.ok()) return kind.reason();
            body.kind = kind.value();
            const Result<Unit> unit = decode_unit(reader);
            if (!unit.ok()) return unit.reason();
            body.unit = unit.value();
            const Result<AuthorityId> authority =
                read_identifier<AuthorityIdTag>(reader, false, "authority id");
            if (!authority.ok()) return authority.reason();
            body.authority = authority.value();
            const Result<std::string_view> label = reader.string();
            if (!label.ok()) return label.reason();
            body.label.assign(label.value());
            const Result<Instant> registered_at = reader.instant();
            if (!registered_at.ok()) return registered_at.reason();
            body.registered_at = registered_at.value();
            record.source_registered_ = body;
            return finish(record);
        }
        case RecordType::SourceRetired: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            SourceRetiredBody body;
            const Result<SourceId> source = read_identifier<SourceIdTag>(reader, true, "source id");
            if (!source.ok()) return source.reason();
            body.source = source.value();
            const Result<Instant> retired_at = reader.instant();
            if (!retired_at.ok()) return retired_at.reason();
            body.retired_at = retired_at.value();
            const Result<std::string_view> reason = reader.string();
            if (!reason.ok()) return reason.reason();
            body.reason.assign(reason.value());
            const Result<Digest> digest = reader.digest();
            if (!digest.ok()) return digest.reason();
            body.source_digest = digest.value();
            record.source_retired_ = body;
            return finish(record);
        }
        case RecordType::GenerationPublished:
        case RecordType::GenerationAttested: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            const Result<SourceId> source = read_identifier<SourceIdTag>(reader, true, "source id");
            if (!source.ok()) return source.reason();
            const Result<std::uint64_t> generation = reader.u64();
            if (!generation.ok()) return generation.reason();
            const Result<Instant> stamp = reader.instant();
            if (!stamp.ok()) return stamp.reason();
            const Result<Digest> evidence = reader.digest();
            if (!evidence.ok()) return evidence.reason();
            const Result<Instant> valid_until = reader.instant();
            if (!valid_until.ok()) return valid_until.reason();
            const Result<std::string_view> method = reader.string();
            if (!method.ok()) return method.reason();
            if (type == RecordType::GenerationPublished) {
                GenerationPublishedBody body;
                body.source = source.value();
                body.generation = Generation{generation.value()};
                body.published_at = stamp.value();
                body.evidence = evidence.value();
                body.valid_until = valid_until.value();
                body.method.assign(method.value());
                record.generation_published_ = body;
            } else {
                GenerationAttestedBody body;
                body.source = source.value();
                body.generation = Generation{generation.value()};
                body.attested_at = stamp.value();
                body.evidence = evidence.value();
                body.valid_until = valid_until.value();
                body.method.assign(method.value());
                record.generation_attested_ = body;
            }
            return finish(record);
        }
        case RecordType::MeasurementRecorded: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            MeasurementRecordedBody body;
            const Result<EntryId> entry = read_identifier<EntryIdTag>(reader, true, "entry id");
            if (!entry.ok()) return entry.reason();
            body.entry = entry.value();
            const Result<Interval> interval = reader.interval();
            if (!interval.ok()) return interval.reason();
            body.interval = interval.value();
            const Result<SourceId> source = read_identifier<SourceIdTag>(reader, true, "source id");
            if (!source.ok()) return source.reason();
            body.source = source.value();
            const Result<std::uint64_t> generation = reader.u64();
            if (!generation.ok()) return generation.reason();
            body.generation = Generation{generation.value()};
            const Result<Quantity> quantity = reader.quantity();
            if (!quantity.ok()) return quantity.reason();
            body.quantity = quantity.value();
            const Result<std::pair<SubjectKind, std::string>> subject = read_subject(reader);
            if (!subject.ok()) return subject.reason();
            body.subject_kind = subject.value().first;
            body.subject = subject.value().second;
            const Result<Digest> evidence = reader.digest();
            if (!evidence.ok()) return evidence.reason();
            body.evidence = evidence.value();
            const Result<Instant> recorded_at = reader.instant();
            if (!recorded_at.ok()) return recorded_at.reason();
            body.recorded_at = recorded_at.value();
            const Result<std::string_view> method = reader.string();
            if (!method.ok()) return method.reason();
            body.method.assign(method.value());
            record.measurement_ = body;
            return finish(record);
        }
        case RecordType::ClassificationRecorded: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            ClassificationRecordedBody body;
            const Result<AllocationId> allocation =
                read_identifier<AllocationIdTag>(reader, true, "allocation id");
            if (!allocation.ok()) return allocation.reason();
            body.allocation = allocation.value();
            const Result<EntryId> entry = read_identifier<EntryIdTag>(reader, true, "entry id");
            if (!entry.ok()) return entry.reason();
            body.entry = entry.value();
            const Result<ServiceClass> klass = read_enum(reader, kServiceClasses, "service class");
            if (!klass.ok()) return klass.reason();
            body.klass = klass.value();
            const Result<Quantity> quantity = reader.quantity();
            if (!quantity.ok()) return quantity.reason();
            body.quantity = quantity.value();
            const Result<std::pair<SubjectKind, std::string>> subject = read_subject(reader);
            if (!subject.ok()) return subject.reason();
            body.subject_kind = subject.value().first;
            body.subject = subject.value().second;
            const Result<Digest> evidence = reader.digest();
            if (!evidence.ok()) return evidence.reason();
            body.evidence = evidence.value();
            const Result<Instant> recorded_at = reader.instant();
            if (!recorded_at.ok()) return recorded_at.reason();
            body.recorded_at = recorded_at.value();
            const Result<std::string_view> method = reader.string();
            if (!method.ok()) return method.reason();
            body.method.assign(method.value());
            record.classification_ = body;
            return finish(record);
        }
        case RecordType::ResidualRecorded: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            ResidualRecordedBody body;
            const Result<ResidualId> residual =
                read_identifier<ResidualIdTag>(reader, true, "residual id");
            if (!residual.ok()) return residual.reason();
            body.residual = residual.value();
            const Result<Interval> interval = reader.interval();
            if (!interval.ok()) return interval.reason();
            body.interval = interval.value();
            const Result<ServiceClass> klass = read_enum(reader, kServiceClasses, "service class");
            if (!klass.ok()) return klass.reason();
            body.klass = klass.value();
            const Result<bool> quantified = reader.boolean();
            if (!quantified.ok()) return quantified.reason();
            body.quantified = quantified.value();
            const Result<Quantity> quantity = reader.quantity();
            if (!quantity.ok()) return quantity.reason();
            body.quantity = quantity.value();
            const Result<bool> bound = reader.boolean();
            if (!bound.ok()) return bound.reason();
            body.bound_to_entry = bound.value();
            const Result<EntryId> bound_entry =
                read_identifier<EntryIdTag>(reader, body.bound_to_entry, "bound entry id");
            if (!bound_entry.ok()) return bound_entry.reason();
            body.entry = bound_entry.value();
            const Result<std::pair<SubjectKind, std::string>> subject = read_subject(reader);
            if (!subject.ok()) return subject.reason();
            body.subject_kind = subject.value().first;
            body.subject = subject.value().second;
            const Result<Digest> evidence = reader.digest();
            if (!evidence.ok()) return evidence.reason();
            body.evidence = evidence.value();
            const Result<Instant> recorded_at = reader.instant();
            if (!recorded_at.ok()) return recorded_at.reason();
            body.recorded_at = recorded_at.value();
            const Result<std::string_view> basis = reader.string();
            if (!basis.ok()) return basis.reason();
            body.basis.assign(basis.value());
            record.residual_ = body;
            return finish(record);
        }
        case RecordType::TargetVoided: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            TargetVoidedBody body;
            const Result<VoidId> void_id = read_identifier<VoidIdTag>(reader, true, "void id");
            if (!void_id.ok()) return void_id.reason();
            body.void_id = void_id.value();
            const Result<TargetKind> target_kind = read_enum(reader, kTargetKinds, "target kind");
            if (!target_kind.ok()) return target_kind.reason();
            body.target_kind = target_kind.value();
            const Result<std::string_view> target_id = reader.string();
            if (!target_id.ok()) return target_id.reason();
            body.target_id.assign(target_id.value());
            const Result<Digest> target_digest = reader.digest();
            if (!target_digest.ok()) return target_digest.reason();
            body.target_digest = target_digest.value();
            const Result<Instant> voided_at = reader.instant();
            if (!voided_at.ok()) return voided_at.reason();
            body.voided_at = voided_at.value();
            const Result<std::string_view> reason = reader.string();
            if (!reason.ok()) return reason.reason();
            body.reason.assign(reason.value());
            if (!is_valid_identifier_text(body.target_id)) {
                return Reason{ReasonCode::RecordCorrupt, "void target is not a valid identifier"};
            }
            record.target_voided_ = body;
            return finish(record);
        }
        case RecordType::IntervalSealed: {
            const Result<MutationMeta> meta = read_meta(reader);
            if (!meta.ok()) return meta.reason();
            record.meta_ = meta.value();
            IntervalSealedBody body;
            const Result<SealId> seal = read_identifier<SealIdTag>(reader, true, "seal id");
            if (!seal.ok()) return seal.reason();
            body.seal = seal.value();
            const Result<Interval> interval = reader.interval();
            if (!interval.ok()) return interval.reason();
            body.interval = interval.value();
            const Result<std::uint64_t> revision = reader.u64();
            if (!revision.ok()) return revision.reason();
            body.revision_at_seal = Revision{revision.value()};
            const Result<bool> closed = reader.boolean();
            if (!closed.ok()) return closed.reason();
            body.closed = closed.value();
            const Result<Digest> report_digest = reader.digest();
            if (!report_digest.ok()) return report_digest.reason();
            body.report_digest = report_digest.value();
            const Result<Instant> sealed_at = reader.instant();
            if (!sealed_at.ok()) return sealed_at.reason();
            body.sealed_at = sealed_at.value();
            const Result<std::string_view> note = reader.string();
            if (!note.ok()) return note.reason();
            body.note.assign(note.value());
            record.interval_sealed_ = body;
            return finish(record);
        }
    }
    return Reason{ReasonCode::RecordCorrupt, "record type is not decodable"};
}

Digest Record::content_digest() const {
    const Result<std::vector<std::uint8_t>> payload = encode_record_payload(*this);
    if (!payload.ok()) {
        return Digest{};
    }
    return Sha256::hash(std::span<const std::uint8_t>{payload.value().data(), payload.value().size()});
}

std::string Record::describe() const {
    std::string out{to_string(type_)};
    out.append(" seq=");
    out.append(std::to_string(sequence_.value()));
    out.append(" rev=");
    out.append(std::to_string(revision_.value()));
    out.append(" epoch=");
    out.append(std::to_string(epoch_.value()));
    out.append(" digest=");
    out.append(content_digest().to_hex().substr(0, 16));
    return out;
}

std::array<std::uint8_t, kRecordHeaderSize> encode_frame_header(const FrameHeader& header) {
    Writer writer;
    writer.u32(kRecordMagic);
    writer.u16(header.format_version);
    writer.u16(static_cast<std::uint16_t>(header.type));
    writer.u32(header.flags);
    writer.u64(header.sequence.value());
    writer.u64(header.revision.value());
    writer.u64(header.epoch.value());
    writer.u32(header.payload_length);
    writer.u32(header.payload_crc);
    const std::uint32_t header_crc =
        crc32c(std::span<const std::uint8_t>{writer.bytes().data(), writer.bytes().size()});
    writer.u32(header_crc);

    std::array<std::uint8_t, kRecordHeaderSize> bytes{};
    const std::vector<std::uint8_t>& buffer = writer.bytes();
    for (std::size_t i = 0; i < kRecordHeaderSize && i < buffer.size(); ++i) {
        bytes[i] = buffer[i];
    }
    return bytes;
}

Result<FrameHeader> decode_frame_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kRecordHeaderSize) {
        return Reason{ReasonCode::HeaderCorrupt, "record header is not the expected size"};
    }
    const std::uint32_t computed_header_crc = crc32c(bytes.first(kRecordHeaderSize - 4));
    Reader reader{bytes};
    const Result<std::uint32_t> magic = reader.u32();
    if (!magic.ok()) return magic.reason();
    if (magic.value() != kRecordMagic) {
        return Reason{ReasonCode::HeaderCorrupt, "record magic does not match"};
    }
    FrameHeader header;
    const Result<std::uint16_t> format_version = reader.u16();
    if (!format_version.ok()) return format_version.reason();
    header.format_version = format_version.value();
    const Result<std::uint16_t> type = reader.u16();
    if (!type.ok()) return type.reason();
    bool known_type = false;
    for (RecordType candidate : kRecordTypes) {
        if (static_cast<std::uint16_t>(candidate) == type.value()) {
            header.type = candidate;
            known_type = true;
            break;
        }
    }
    if (!known_type) {
        return Reason{ReasonCode::FormatVersionUnsupported,
                      "record type " + std::to_string(type.value()) + " is not supported"};
    }
    const Result<std::uint32_t> flags = reader.u32();
    if (!flags.ok()) return flags.reason();
    header.flags = flags.value();
    const Result<std::uint64_t> sequence = reader.u64();
    if (!sequence.ok()) return sequence.reason();
    header.sequence = Sequence{sequence.value()};
    const Result<std::uint64_t> revision = reader.u64();
    if (!revision.ok()) return revision.reason();
    header.revision = Revision{revision.value()};
    const Result<std::uint64_t> epoch = reader.u64();
    if (!epoch.ok()) return epoch.reason();
    header.epoch = Epoch{epoch.value()};
    const Result<std::uint32_t> payload_length = reader.u32();
    if (!payload_length.ok()) return payload_length.reason();
    header.payload_length = payload_length.value();
    const Result<std::uint32_t> payload_crc = reader.u32();
    if (!payload_crc.ok()) return payload_crc.reason();
    header.payload_crc = payload_crc.value();
    const Result<std::uint32_t> stored_header_crc = reader.u32();
    if (!stored_header_crc.ok()) return stored_header_crc.reason();

    if (stored_header_crc.value() != computed_header_crc) {
        return Reason{ReasonCode::HeaderCorrupt, "record header checksum does not match"};
    }
    if (header.format_version != kJournalFormatVersion) {
        return Reason{ReasonCode::FormatVersionUnsupported,
                      "journal format version " + std::to_string(header.format_version) +
                          " is not supported by this build"};
    }
    if (header.payload_length > kMaxRecordPayloadBytes) {
        return Reason{ReasonCode::BoundsExceeded, "record payload length exceeds the permitted maximum"};
    }
    return header;
}

Digest compute_chain_digest(const Digest& previous, std::span<const std::uint8_t> header_bytes,
                            std::span<const std::uint8_t> payload) {
    Sha256 hasher;
    hasher.update(previous.bytes());
    hasher.update(header_bytes);
    hasher.update(payload);
    return hasher.finish();
}

}  // namespace fel
