#pragma once

// Reason codes, status values, and result carriers.
//
// Every public entry point in this runtime reports failure with a stable,
// machine-readable ReasonCode plus a human-readable detail string. Reason codes
// are part of the observable contract and are never renumbered.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fel {

enum class ReasonCode : std::int32_t {
    Ok = 0,

    // -- identity / lexical -------------------------------------------------
    InvalidIdentifier,
    EmptyIdentifier,
    IdentifierTooLong,
    IdentifierCharset,

    // -- exact arithmetic ---------------------------------------------------
    ZeroDenominator,
    ArithmeticOverflow,
    ArithmeticUnderflow,
    DivisionByZero,
    MalformedNumber,

    // -- units and quantities ----------------------------------------------
    UnknownUnit,
    DimensionMismatch,
    UnitMismatch,
    NegativeQuantity,
    NonPositiveQuantity,
    NotQuantified,

    // -- time and intervals -------------------------------------------------
    InvalidInterval,
    EmptyInterval,
    IntervalNotContained,

    // -- sources, generations, epochs --------------------------------------
    UnknownSource,
    DuplicateSource,
    UnknownEntry,
    UnknownAllocation,
    UnknownResidual,
    UnknownCorrection,
    GenerationRegressed,
    StaleGeneration,
    EpochRegressed,
    StaleEpoch,
    AttemptReplay,
    SourceRetired,

    // -- durability and integrity ------------------------------------------
    IoError,
    NotFound,
    AlreadyExists,
    NotADirectory,
    LockUnavailable,
    LockHeldExclusive,
    FormatVersionUnsupported,
    RecordCorrupt,
    HeaderCorrupt,
    InteriorCorruption,
    TornTailRecovered,
    DigestMismatch,
    ChainBroken,
    ManifestCorrupt,
    SnapshotCorrupt,
    ManifestMissing,
    SegmentMissing,

    // -- ledger state machine ----------------------------------------------
    AlreadySuperseded,
    NotSuperseded,
    IntervalSealed,
    IntervalNotSealed,
    RevisionMismatch,
    StaleRevision,
    IdempotencyConflict,
    DuplicateIdempotencyKey,
    ConflictingEvidence,
    DuplicateEvidence,

    // -- accounting / reconciliation ---------------------------------------
    IntervalOverlap,
    OverAllocation,
    UnderAllocation,
    UnclassifiedRemainder,
    AccountingNotClosed,
    EvidenceNotFresh,
    RecoveredNotCurrent,
    BasisMismatch,
    NoMeasurements,

    // -- authority boundary -------------------------------------------------
    AuthorityNotOwned,
    AuthorityRefused,
    AuthorityUnavailable,
    AuthorityStale,
    AuthorityConflicting,

    // -- generic explicit states -------------------------------------------
    Unsupported,
    Indeterminate,
    Refused,
    InvalidArgument,
    InvalidState,
    BoundsExceeded,
    InternalInvariant,
};

std::string_view to_string(ReasonCode code) noexcept;
std::optional<ReasonCode> reason_code_from_string(std::string_view name) noexcept;

// A reason is always present on failure. ReasonCode::Ok is represented by
// Reason{} with an empty detail.
class Reason {
public:
    Reason() noexcept = default;
    explicit Reason(ReasonCode code) noexcept;
    Reason(ReasonCode code, std::string detail);

    static Reason ok() noexcept { return Reason{}; }

    ReasonCode code() const noexcept { return code_; }
    std::string_view name() const noexcept { return to_string(code_); }
    const std::string& detail() const noexcept { return detail_; }
    bool is_ok() const noexcept { return code_ == ReasonCode::Ok; }

    // "<Name>" or "<Name>: <detail>"
    std::string message() const;

private:
    ReasonCode code_{ReasonCode::Ok};
    std::string detail_;
};

class Status {
public:
    Status() noexcept = default;
    Status(Reason reason) noexcept : reason_(std::move(reason)) {}

    static Status ok() noexcept { return Status{}; }
    static Status fail(ReasonCode code) noexcept { return Status{Reason{code}}; }
    static Status fail(ReasonCode code, std::string detail) {
        return Status{Reason{code, std::move(detail)}};
    }

    bool is_ok() const noexcept { return reason_.is_ok(); }
    const Reason& reason() const noexcept { return reason_; }
    ReasonCode code() const noexcept { return reason_.code(); }
    std::string message() const { return reason_.message(); }

private:
    Reason reason_{};
};

// Result<T> carries either a value or a failure reason. Result<void> is not
// provided; use Status.
template <class T>
class Result {
public:
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::in_place, std::move(value)) {}
    Result(Reason reason) noexcept : reason_(std::move(reason)) {}
    Result(Status status) noexcept : reason_(status.reason()) {}

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Reason& reason() const noexcept { return reason_; }
    ReasonCode code() const noexcept { return reason_.code(); }

    const T& value() const noexcept { return *value_; }
    T& value() noexcept { return *value_; }
    T&& take() noexcept { return std::move(*value_); }

    const T& value_or(const T& fallback) const noexcept { return value_ ? *value_ : fallback; }
    T&& take_or(T&& fallback) noexcept { return value_ ? std::move(*value_) : std::move(fallback); }

private:
    std::optional<T> value_{};
    Reason reason_{};
};

}  // namespace fel
