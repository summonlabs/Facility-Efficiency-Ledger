#include "fel/status.hpp"

#include <array>
#include <utility>

namespace fel {
namespace {

struct CodeName {
    ReasonCode code;
    std::string_view name;
};

// Order matters only for the lookup table; names are the stable public spelling.
constexpr CodeName kCodeNames[] = {
    {ReasonCode::Ok, "Ok"},
    {ReasonCode::InvalidIdentifier, "InvalidIdentifier"},
    {ReasonCode::EmptyIdentifier, "EmptyIdentifier"},
    {ReasonCode::IdentifierTooLong, "IdentifierTooLong"},
    {ReasonCode::IdentifierCharset, "IdentifierCharset"},
    {ReasonCode::ZeroDenominator, "ZeroDenominator"},
    {ReasonCode::ArithmeticOverflow, "ArithmeticOverflow"},
    {ReasonCode::ArithmeticUnderflow, "ArithmeticUnderflow"},
    {ReasonCode::DivisionByZero, "DivisionByZero"},
    {ReasonCode::MalformedNumber, "MalformedNumber"},
    {ReasonCode::UnknownUnit, "UnknownUnit"},
    {ReasonCode::DimensionMismatch, "DimensionMismatch"},
    {ReasonCode::UnitMismatch, "UnitMismatch"},
    {ReasonCode::NegativeQuantity, "NegativeQuantity"},
    {ReasonCode::NonPositiveQuantity, "NonPositiveQuantity"},
    {ReasonCode::NotQuantified, "NotQuantified"},
    {ReasonCode::InvalidInterval, "InvalidInterval"},
    {ReasonCode::EmptyInterval, "EmptyInterval"},
    {ReasonCode::IntervalNotContained, "IntervalNotContained"},
    {ReasonCode::UnknownSource, "UnknownSource"},
    {ReasonCode::DuplicateSource, "DuplicateSource"},
    {ReasonCode::UnknownEntry, "UnknownEntry"},
    {ReasonCode::UnknownAllocation, "UnknownAllocation"},
    {ReasonCode::UnknownResidual, "UnknownResidual"},
    {ReasonCode::UnknownCorrection, "UnknownCorrection"},
    {ReasonCode::GenerationRegressed, "GenerationRegressed"},
    {ReasonCode::StaleGeneration, "StaleGeneration"},
    {ReasonCode::EpochRegressed, "EpochRegressed"},
    {ReasonCode::StaleEpoch, "StaleEpoch"},
    {ReasonCode::AttemptReplay, "AttemptReplay"},
    {ReasonCode::SourceRetired, "SourceRetired"},
    {ReasonCode::IoError, "IoError"},
    {ReasonCode::NotFound, "NotFound"},
    {ReasonCode::AlreadyExists, "AlreadyExists"},
    {ReasonCode::NotADirectory, "NotADirectory"},
    {ReasonCode::LockUnavailable, "LockUnavailable"},
    {ReasonCode::LockHeldExclusive, "LockHeldExclusive"},
    {ReasonCode::FormatVersionUnsupported, "FormatVersionUnsupported"},
    {ReasonCode::RecordCorrupt, "RecordCorrupt"},
    {ReasonCode::HeaderCorrupt, "HeaderCorrupt"},
    {ReasonCode::InteriorCorruption, "InteriorCorruption"},
    {ReasonCode::TornTailRecovered, "TornTailRecovered"},
    {ReasonCode::DigestMismatch, "DigestMismatch"},
    {ReasonCode::ChainBroken, "ChainBroken"},
    {ReasonCode::ManifestCorrupt, "ManifestCorrupt"},
    {ReasonCode::SnapshotCorrupt, "SnapshotCorrupt"},
    {ReasonCode::ManifestMissing, "ManifestMissing"},
    {ReasonCode::SegmentMissing, "SegmentMissing"},
    {ReasonCode::AlreadySuperseded, "AlreadySuperseded"},
    {ReasonCode::NotSuperseded, "NotSuperseded"},
    {ReasonCode::IntervalSealed, "IntervalSealed"},
    {ReasonCode::IntervalNotSealed, "IntervalNotSealed"},
    {ReasonCode::RevisionMismatch, "RevisionMismatch"},
    {ReasonCode::StaleRevision, "StaleRevision"},
    {ReasonCode::IdempotencyConflict, "IdempotencyConflict"},
    {ReasonCode::DuplicateIdempotencyKey, "DuplicateIdempotencyKey"},
    {ReasonCode::ConflictingEvidence, "ConflictingEvidence"},
    {ReasonCode::DuplicateEvidence, "DuplicateEvidence"},
    {ReasonCode::IntervalOverlap, "IntervalOverlap"},
    {ReasonCode::OverAllocation, "OverAllocation"},
    {ReasonCode::UnderAllocation, "UnderAllocation"},
    {ReasonCode::UnclassifiedRemainder, "UnclassifiedRemainder"},
    {ReasonCode::AccountingNotClosed, "AccountingNotClosed"},
    {ReasonCode::EvidenceNotFresh, "EvidenceNotFresh"},
    {ReasonCode::RecoveredNotCurrent, "RecoveredNotCurrent"},
    {ReasonCode::BasisMismatch, "BasisMismatch"},
    {ReasonCode::NoMeasurements, "NoMeasurements"},
    {ReasonCode::AuthorityNotOwned, "AuthorityNotOwned"},
    {ReasonCode::AuthorityRefused, "AuthorityRefused"},
    {ReasonCode::AuthorityUnavailable, "AuthorityUnavailable"},
    {ReasonCode::AuthorityStale, "AuthorityStale"},
    {ReasonCode::AuthorityConflicting, "AuthorityConflicting"},
    {ReasonCode::Unsupported, "Unsupported"},
    {ReasonCode::Indeterminate, "Indeterminate"},
    {ReasonCode::Refused, "Refused"},
    {ReasonCode::InvalidArgument, "InvalidArgument"},
    {ReasonCode::InvalidState, "InvalidState"},
    {ReasonCode::BoundsExceeded, "BoundsExceeded"},
    {ReasonCode::InternalInvariant, "InternalInvariant"},
};

}  // namespace

std::string_view to_string(ReasonCode code) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "UnknownReasonCode";
}

std::optional<ReasonCode> reason_code_from_string(std::string_view name) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.name == name) {
            return entry.code;
        }
    }
    return std::nullopt;
}

Reason::Reason(ReasonCode code) noexcept : code_(code) {}

Reason::Reason(ReasonCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

std::string Reason::message() const {
    std::string out{to_string(code_)};
    if (!detail_.empty()) {
        out.append(": ");
        out.append(detail_);
    }
    return out;
}

std::string_view library_version_string() noexcept { return "1.0.0"; }

#ifndef FEL_GIT_COMMIT
#define FEL_GIT_COMMIT "unknown"
#endif

std::string_view library_commit_string() noexcept { return FEL_GIT_COMMIT; }

}  // namespace fel
