#pragma once

// Conservation engine.
//
// For a bounded interval and an exact revision the engine compares the resource
// that entered the accounting boundary against the resource that was classified,
// per physical dimension, using exact rational arithmetic. The difference is
// reported, never absorbed: a non-zero remainder is a named, explained defect of
// classification rather than something folded into waste or useful work.

#include <cstdint>
#include <string>
#include <vector>

#include "fel/digest.hpp"
#include "fel/ids.hpp"
#include "fel/quantity.hpp"
#include "fel/status.hpp"
#include "fel/time.hpp"
#include "fel/view.hpp"

namespace fel {

// Whether the result is asserted as a current statement or as a recomputation of
// a past interval. Current assertions require current evidence.
enum class BasisPolicy : std::uint8_t {
    Current = 0,
    Historical = 1,
};

std::string_view to_string(BasisPolicy policy) noexcept;

struct ClosureIssue {
    ReasonCode code{ReasonCode::Ok};
    std::string subject{};
    std::string detail{};
    Digest record_digest{};
};

struct DimensionClosure {
    Dimension dimension{Dimension::Count};
    Unit base_unit{Unit::Each};

    Quantity measured_input{};    // live measurements fully inside the interval
    Quantity declared_input{};    // unbound quantified residuals: resource no meter captured
    Quantity input_total{};       // measured_input + declared_input
    Quantity classified_useful{};
    Quantity classified_avoidable{};
    Quantity classified_stranded{};
    Quantity classified_wasted{};
    Quantity classified_unknown{};
    Quantity classified_unmeasured{};
    Quantity classified_total{};
    Quantity unclassified{};      // input_total - classified_total, exactly
    Quantity closure_residual{};  // the reported residual, equal to unclassified

    std::uint64_t measurement_count{0};
    std::uint64_t allocation_count{0};
    std::uint64_t bound_residual_count{0};
    std::uint64_t declared_residual_count{0};
    std::uint64_t unquantified_residual_count{0};

    bool closed{false};
    ReasonCode code{ReasonCode::Ok};
    std::string explanation{};
    std::vector<ClosureIssue> issues{};
    std::vector<std::string> evidence{};
};

struct FreshnessDependency {
    SourceId source{};
    Generation generation{};
    FreshnessState state{FreshnessState::Unattested};
    Digest evidence{};
    Instant valid_until{};
    std::uint64_t measurement_count{0};
    std::string detail{};
};

struct ReconciliationReport {
    LedgerId ledger{};
    Interval interval{};
    Revision revision{};
    Epoch epoch{};
    Digest chain_head{};
    Instant generated_at{};
    BasisPolicy basis{BasisPolicy::Current};

    bool sealed{false};
    SealId seal{};
    Revision sealed_revision{};
    Digest sealed_report_digest{};
    bool changed_since_seal{false};

    bool closed{false};
    ReasonCode code{ReasonCode::Ok};
    std::string explanation{};

    std::vector<DimensionClosure> dimensions{};
    std::vector<FreshnessDependency> dependencies{};
    std::vector<ClosureIssue> issues{};

    std::uint64_t measurement_count{0};
    std::uint64_t allocation_count{0};
    std::uint64_t residual_count{0};
    std::uint64_t conflict_count{0};

    Digest report_digest{};
};

struct ReconcileRequest {
    Interval interval{};
    // Instant at which evidence validity is judged. Instant{} means "use the end
    // of the interval", which makes sealed historical reports reproducible.
    Instant as_of{};
    BasisPolicy basis{BasisPolicy::Current};
};

Result<ReconciliationReport> reconcile(const LedgerView& view, const ReconcileRequest& request);

// Canonical, deterministic JSON rendering of a report. Two reports that differ
// only by object key order render identically.
std::string render_json(const ReconciliationReport& report);

}  // namespace fel
