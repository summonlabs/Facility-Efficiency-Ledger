#include "fel/reconcile.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "fel/report.hpp"

namespace fel {
namespace {

Unit base_unit_of(Dimension dimension) noexcept {
    switch (dimension) {
        case Dimension::Count:
            return Unit::Each;
        case Dimension::Energy:
            return Unit::Joule;
        case Dimension::Power:
            return Unit::Watt;
        case Dimension::Volume:
            return Unit::Liter;
        case Dimension::Mass:
            return Unit::Kilogram;
        case Dimension::Data:
            return Unit::Byte;
        case Dimension::DataTime:
            return Unit::ByteHour;
        case Dimension::Time:
            return Unit::Second;
    }
    return Unit::Each;
}

struct Accumulator {
    Quantity total{};
    bool initialized{false};

    Status add(const Quantity& value) {
        if (!initialized) {
            total = value;
            initialized = true;
            return Status::ok();
        }
        const Result<Quantity> sum = total.add(value);
        if (!sum.ok()) {
            return Status{sum.reason()};
        }
        total = sum.value();
        return Status::ok();
    }
};

struct DimensionAccumulator {
    Dimension dimension{Dimension::Count};
    Unit base_unit{Unit::Each};
    Accumulator measured_input{};
    Accumulator declared_input{};
    Accumulator useful{};
    Accumulator avoidable{};
    Accumulator stranded{};
    Accumulator wasted{};
    Accumulator unknown{};
    Accumulator unmeasured{};
    std::uint64_t measurement_count{0};
    std::uint64_t allocation_count{0};
    std::uint64_t bound_residual_count{0};
    std::uint64_t declared_residual_count{0};
    std::uint64_t unquantified_residual_count{0};
    std::vector<ClosureIssue> issues{};
    std::vector<std::string> evidence{};
};

Result<Quantity> to_base(const Quantity& value, Unit base) {
    return value.convert_to(base);
}

}  // namespace

std::string_view to_string(BasisPolicy policy) noexcept {
    switch (policy) {
        case BasisPolicy::Current:
            return "current";
        case BasisPolicy::Historical:
            return "historical";
    }
    return "unknown";
}

Result<ReconciliationReport> reconcile(const LedgerView& view, const ReconcileRequest& request) {
    ReconciliationReport report;
    report.ledger = view.ledger();
    report.interval = request.interval;
    report.revision = view.revision();
    report.epoch = view.epoch();
    report.chain_head = view.chain_head();
    report.basis = request.basis;
    report.generated_at = request.as_of;

    // ---------------------------------------------------------------- scope
    std::vector<std::size_t> scoped;
    for (std::size_t i = 0; i < view.measurements().size(); ++i) {
        const MeasurementState& measurement = view.measurements()[i];
        if (!measurement.superseded_by.empty() || measurement.voided) {
            continue;
        }
        if (request.interval.contains(measurement.interval)) {
            scoped.push_back(i);
            continue;
        }
        if (request.interval.overlaps(measurement.interval)) {
            ClosureIssue issue;
            issue.code = ReasonCode::IntervalNotContained;
            issue.subject = measurement.id.str();
            issue.detail = "measurement interval " + measurement.interval.to_string() +
                           " crosses the accounting boundary " + request.interval.to_string() +
                           " and cannot be split by this runtime";
            issue.record_digest = measurement.record_digest;
            report.issues.push_back(issue);
        }
    }

    std::set<std::string> scoped_entries;
    for (std::size_t index : scoped) {
        scoped_entries.insert(view.measurements()[index].id.str());
    }

    // ------------------------------------------------------------ grouping
    std::vector<DimensionAccumulator> accumulators;
    const auto accumulator_for = [&accumulators](Dimension dimension) -> DimensionAccumulator& {
        for (DimensionAccumulator& candidate : accumulators) {
            if (candidate.dimension == dimension) {
                return candidate;
            }
        }
        DimensionAccumulator created;
        created.dimension = dimension;
        created.base_unit = base_unit_of(dimension);
        accumulators.push_back(std::move(created));
        return accumulators.back();
    };

    for (std::size_t index : scoped) {
        const MeasurementState& measurement = view.measurements()[index];
        const SourceState* source = view.find_source(measurement.source);
        if (source == nullptr) {
            ClosureIssue issue;
            issue.code = ReasonCode::UnknownSource;
            issue.subject = measurement.id.str();
            issue.detail = "measurement names source " + measurement.source.str() +
                           " which is not registered";
            issue.record_digest = measurement.record_digest;
            report.issues.push_back(issue);
            continue;
        }
        DimensionAccumulator& accumulator =
            accumulator_for(dimension_of(measurement.quantity.unit()));
        if (dimension_of(source->unit) != dimension_of(measurement.quantity.unit())) {
            ClosureIssue issue;
            issue.code = ReasonCode::DimensionMismatch;
            issue.subject = measurement.id.str();
            issue.detail = "measurement unit " +
                           std::string(unit_symbol(measurement.quantity.unit())) +
                           " is not in the registered dimension of source " + source->id.str();
            issue.record_digest = measurement.record_digest;
            accumulator.issues.push_back(issue);
            continue;
        }
        Result<Quantity> value = to_base(measurement.quantity, accumulator.base_unit);
        if (!value.ok()) {
            ClosureIssue issue;
            issue.code = value.code();
            issue.subject = measurement.id.str();
            issue.detail = "measurement cannot be expressed in " +
                           std::string(unit_symbol(accumulator.base_unit)) + ": " +
                           value.reason().message();
            issue.record_digest = measurement.record_digest;
            accumulator.issues.push_back(issue);
            continue;
        }
        const Status added = accumulator.measured_input.add(value.value());
        if (!added.is_ok()) {
            ClosureIssue issue;
            issue.code = added.code();
            issue.subject = measurement.id.str();
            issue.detail = "measured input total is not representable";
            accumulator.issues.push_back(issue);
            continue;
        }
        accumulator.measurement_count += 1U;
        accumulator.evidence.push_back(measurement.record_digest.to_hex());
    }

    // Every live classification whose entry is in scope.
    for (const AllocationState& allocation : view.allocations()) {
        if (!allocation.superseded_by.empty() || allocation.voided) {
            continue;
        }
        if (scoped_entries.count(allocation.entry.str()) == 0U) {
            continue;
        }
        DimensionAccumulator& accumulator =
            accumulator_for(dimension_of(allocation.quantity.unit()));
        Result<Quantity> value = to_base(allocation.quantity, accumulator.base_unit);
        if (!value.ok()) {
            ClosureIssue issue;
            issue.code = value.code();
            issue.subject = allocation.id.str();
            issue.detail = "classification cannot be expressed in " +
                           std::string(unit_symbol(accumulator.base_unit));
            issue.record_digest = allocation.record_digest;
            accumulator.issues.push_back(issue);
            continue;
        }
        Accumulator* target = nullptr;
        switch (allocation.klass) {
            case ServiceClass::Useful:
                target = &accumulator.useful;
                break;
            case ServiceClass::Avoidable:
                target = &accumulator.avoidable;
                break;
            case ServiceClass::Stranded:
                target = &accumulator.stranded;
                break;
            case ServiceClass::Wasted:
                target = &accumulator.wasted;
                break;
            case ServiceClass::Unknown:
                target = &accumulator.unknown;
                break;
            case ServiceClass::Unmeasured:
                target = &accumulator.unmeasured;
                break;
        }
        if (target == nullptr) {
            continue;
        }
        const Status added = target->add(value.value());
        if (!added.is_ok()) {
            ClosureIssue issue;
            issue.code = added.code();
            issue.subject = allocation.id.str();
            issue.detail = "classification total is not representable";
            accumulator.issues.push_back(issue);
            continue;
        }
        accumulator.allocation_count += 1U;
        accumulator.evidence.push_back(allocation.record_digest.to_hex());
    }

    // Residuals: bound residuals classify measured input, unbound residuals
    // declare input no meter captured.
    for (const ResidualState& residual : view.residuals()) {
        if (!residual.superseded_by.empty() || residual.voided) {
            continue;
        }
        if (!request.interval.contains(residual.interval)) {
            if (request.interval.overlaps(residual.interval)) {
                ClosureIssue issue;
                issue.code = ReasonCode::IntervalNotContained;
                issue.subject = residual.id.str();
                issue.detail = "residual interval crosses the accounting boundary";
                issue.record_digest = residual.record_digest;
                report.issues.push_back(issue);
            }
            continue;
        }
        if (residual.bound_to_entry && scoped_entries.count(residual.entry.str()) == 0U) {
            continue;
        }
        DimensionAccumulator& accumulator = accumulator_for(dimension_of(residual.quantity.unit()));
        if (!residual.quantified) {
            accumulator.unquantified_residual_count += 1U;
            accumulator.evidence.push_back(residual.record_digest.to_hex());
            continue;
        }
        Result<Quantity> value = to_base(residual.quantity, accumulator.base_unit);
        if (!value.ok()) {
            ClosureIssue issue;
            issue.code = value.code();
            issue.subject = residual.id.str();
            issue.detail = "residual cannot be expressed in " +
                           std::string(unit_symbol(accumulator.base_unit));
            issue.record_digest = residual.record_digest;
            accumulator.issues.push_back(issue);
            continue;
        }
        if (!residual.bound_to_entry) {
            const Status added = accumulator.declared_input.add(value.value());
            if (!added.is_ok()) {
                ClosureIssue issue;
                issue.code = added.code();
                issue.subject = residual.id.str();
                issue.detail = "declared input total is not representable";
                accumulator.issues.push_back(issue);
                continue;
            }
            accumulator.declared_residual_count += 1U;
        } else {
            accumulator.bound_residual_count += 1U;
        }
        Accumulator* target = residual.klass == ServiceClass::Unknown ? &accumulator.unknown
                                                                     : &accumulator.unmeasured;
        const Status added = target->add(value.value());
        if (!added.is_ok()) {
            ClosureIssue issue;
            issue.code = added.code();
            issue.subject = residual.id.str();
            issue.detail = "residual total is not representable";
            accumulator.issues.push_back(issue);
            continue;
        }
        accumulator.evidence.push_back(residual.record_digest.to_hex());
    }

    // ------------------------------------------------------- per-entry checks
    for (std::size_t index : scoped) {
        const MeasurementState& measurement = view.measurements()[index];
        const Result<int> order = measurement.allocated.compare(measurement.quantity);
        if (!order.ok()) {
            continue;
        }
        if (order.value() > 0) {
            ClosureIssue issue;
            issue.code = ReasonCode::OverAllocation;
            issue.subject = measurement.id.str();
            issue.detail = "classifications total " + measurement.allocated.to_string() +
                           " which exceeds the measured " + measurement.quantity.to_string();
            issue.record_digest = measurement.record_digest;
            report.issues.push_back(issue);
        }
    }

    // ---------------------------------------------- freshness dependencies
    std::vector<SourceId> sources_seen;
    for (std::size_t index : scoped) {
        const SourceId& source = view.measurements()[index].source;
        if (std::find(sources_seen.begin(), sources_seen.end(), source) == sources_seen.end()) {
            sources_seen.push_back(source);
        }
    }
    std::sort(sources_seen.begin(), sources_seen.end(),
              [](const SourceId& a, const SourceId& b) { return a.str() < b.str(); });
    for (const SourceId& source : sources_seen) {
        const SourceState* state = view.find_source(source);
        if (state == nullptr) {
            continue;
        }
        FreshnessDependency dependency;
        dependency.source = source;
        dependency.generation = state->generation;
        dependency.state = state->freshness;
        dependency.evidence = state->generation_evidence;
        dependency.valid_until = state->generation_valid_until;
        dependency.measurement_count = 0;
        for (std::size_t index : scoped) {
            if (view.measurements()[index].source == source) {
                dependency.measurement_count += 1U;
            }
        }
        switch (state->freshness) {
            case FreshnessState::Current:
                dependency.detail = "generation " + std::to_string(state->generation.value()) +
                                    " is current";
                break;
            case FreshnessState::Unattested:
                dependency.detail = "no generation has been published for this source";
                break;
            case FreshnessState::Expired:
                dependency.detail = "generation evidence expired at " +
                                    state->generation_valid_until.to_iso8601();
                break;
            case FreshnessState::Recovered:
                dependency.detail =
                    "generation evidence was attested before a recovery barrier and is not current "
                    "until it is re-attested";
                break;
            case FreshnessState::Retired:
                dependency.detail =
                    "source was retired; its measurements remain valid as history only";
                break;
        }
        report.dependencies.push_back(std::move(dependency));
    }

    // ------------------------------------------------------------ assembly
    for (DimensionAccumulator& accumulator : accumulators) {
        DimensionClosure closure;
        closure.dimension = accumulator.dimension;
        closure.base_unit = accumulator.base_unit;
        closure.measurement_count = accumulator.measurement_count;
        closure.allocation_count = accumulator.allocation_count;
        closure.bound_residual_count = accumulator.bound_residual_count;
        closure.declared_residual_count = accumulator.declared_residual_count;
        closure.unquantified_residual_count = accumulator.unquantified_residual_count;
        closure.issues = std::move(accumulator.issues);
        closure.evidence = std::move(accumulator.evidence);

        const Quantity zero{Rational{0}, accumulator.base_unit};
        closure.measured_input =
            accumulator.measured_input.initialized ? accumulator.measured_input.total : zero;
        closure.declared_input =
            accumulator.declared_input.initialized ? accumulator.declared_input.total : zero;
        closure.classified_useful = accumulator.useful.initialized ? accumulator.useful.total : zero;
        closure.classified_avoidable =
            accumulator.avoidable.initialized ? accumulator.avoidable.total : zero;
        closure.classified_stranded =
            accumulator.stranded.initialized ? accumulator.stranded.total : zero;
        closure.classified_wasted =
            accumulator.wasted.initialized ? accumulator.wasted.total : zero;
        closure.classified_unknown =
            accumulator.unknown.initialized ? accumulator.unknown.total : zero;
        closure.classified_unmeasured =
            accumulator.unmeasured.initialized ? accumulator.unmeasured.total : zero;

        const Result<Quantity> input_total = closure.measured_input.add(closure.declared_input);
        if (!input_total.ok()) {
            closure.code = input_total.code();
            closure.explanation = "input total is not representable";
            report.dimensions.push_back(std::move(closure));
            continue;
        }
        closure.input_total = input_total.value();

        Quantity classified = closure.classified_useful;
        bool classified_ok = true;
        const Quantity* parts[] = {&closure.classified_avoidable, &closure.classified_stranded,
                                   &closure.classified_wasted,      &closure.classified_unknown,
                                   &closure.classified_unmeasured};
        for (const Quantity* part : parts) {
            const Result<Quantity> sum = classified.add(*part);
            if (!sum.ok()) {
                closure.code = sum.code();
                closure.explanation = "classified total is not representable";
                classified_ok = false;
                break;
            }
            classified = sum.value();
        }
        if (!classified_ok) {
            report.dimensions.push_back(std::move(closure));
            continue;
        }
        closure.classified_total = classified;

        const Result<Quantity> remainder = closure.input_total.subtract(closure.classified_total);
        if (!remainder.ok()) {
            closure.code = remainder.code();
            closure.explanation = "closure remainder is not representable";
            report.dimensions.push_back(std::move(closure));
            continue;
        }
        closure.unclassified = remainder.value();
        closure.closure_residual = remainder.value();

        const bool blocking_issues = std::any_of(
            closure.issues.begin(), closure.issues.end(), [](const ClosureIssue& issue) {
                return issue.code != ReasonCode::Ok;
            });
        if (closure.unquantified_residual_count > 0U) {
            closure.closed = false;
            closure.code = ReasonCode::NotQuantified;
            closure.explanation =
                std::to_string(closure.unquantified_residual_count) +
                " residual declaration(s) in this dimension are explicitly not quantified, so the "
                "dimension cannot be closed exactly";
        } else if (blocking_issues) {
            closure.closed = false;
            closure.code = ReasonCode::Indeterminate;
            closure.explanation = "the dimension carries unresolved evidence defects";
        } else if (closure.unclassified.is_negative()) {
            closure.closed = false;
            closure.code = ReasonCode::OverAllocation;
            closure.explanation = "classifications exceed inputs by " + closure.unclassified.to_string();
        } else if (!closure.unclassified.is_zero()) {
            closure.closed = false;
            closure.code = ReasonCode::UnclassifiedRemainder;
            closure.explanation = closure.unclassified.to_string() +
                                  " of measured input has no classification decision";
        } else {
            closure.closed = true;
            closure.code = ReasonCode::Ok;
            closure.explanation = "inputs reconcile exactly to classified outputs and explicit residuals";
        }
        report.dimensions.push_back(std::move(closure));
    }

    std::sort(report.dimensions.begin(), report.dimensions.end(),
              [](const DimensionClosure& a, const DimensionClosure& b) {
                  return static_cast<int>(a.dimension) < static_cast<int>(b.dimension);
              });

    // --------------------------------------------------------------- seals
    for (const SealState& seal : view.seals()) {
        if (!(seal.interval == request.interval)) {
            continue;
        }
        report.sealed = true;
        report.seal = seal.id;
        // The seal record's own revision is the baseline for staleness: any later
        // mutation moves the ledger past it.
        report.sealed_revision = seal.seal_revision;
        report.sealed_report_digest = seal.report_digest;
    }
    if (report.sealed) {
        report.changed_since_seal = view.revision() > report.sealed_revision;
    }

    // ------------------------------------------------------------ verdicts
    for (const DimensionClosure& closure : report.dimensions) {
        report.measurement_count += closure.measurement_count;
        report.allocation_count += closure.allocation_count;
        report.residual_count += closure.bound_residual_count + closure.declared_residual_count +                       closure.unquantified_residual_count;
    }
    // Fold-level conflicts matter only when they touch identifiers in scope.
    for (const ViewIssue& issue : view.issues()) {
        const bool conflict_code = issue.code == ReasonCode::IntervalOverlap ||
                                   issue.code == ReasonCode::DuplicateEvidence ||
                                   issue.code == ReasonCode::DuplicateIdempotencyKey ||
                                   issue.code == ReasonCode::DimensionMismatch;
        if (!conflict_code) {
            continue;
        }
        const bool in_scope =
            scoped_entries.count(issue.subject) != 0U ||
            std::any_of(scoped.begin(), scoped.end(), [&view, &issue](std::size_t index) {
                return view.measurements()[index].source.str() == issue.subject;
            });
        if (!in_scope) {
            continue;
        }
        ClosureIssue closure_issue;
        closure_issue.code = issue.code;
        closure_issue.subject = issue.subject;
        closure_issue.detail = issue.detail;
        report.issues.push_back(closure_issue);
        report.conflict_count += 1U;
    }

    if (request.basis == BasisPolicy::Current) {
        for (const FreshnessDependency& dependency : report.dependencies) {
            if (dependency.state == FreshnessState::Current) {
                continue;
            }
            ClosureIssue issue;
            issue.subject = dependency.source.str();
            issue.detail = dependency.detail;
            switch (dependency.state) {
                case FreshnessState::Recovered:
                    issue.code = ReasonCode::RecoveredNotCurrent;
                    break;
                case FreshnessState::Expired:
                case FreshnessState::Unattested:
                    issue.code = ReasonCode::EvidenceNotFresh;
                    break;
                case FreshnessState::Retired:
                    issue.code = ReasonCode::AuthorityStale;
                    break;
                case FreshnessState::Current:
                    continue;
            }
            report.issues.push_back(issue);
        }
    }

    bool all_closed = !report.dimensions.empty();
    for (const DimensionClosure& closure : report.dimensions) {
        if (!closure.closed) {
            all_closed = false;
        }
    }
    const bool blocked = std::any_of(report.issues.begin(), report.issues.end(),
                                     [](const ClosureIssue& issue) {
                                         return issue.code == ReasonCode::RecoveredNotCurrent ||
                                                issue.code == ReasonCode::EvidenceNotFresh ||
                                                issue.code == ReasonCode::AuthorityStale ||
                                                issue.code == ReasonCode::IntervalNotContained ||
                                                issue.code == ReasonCode::OverAllocation ||
                                                issue.code == ReasonCode::IntervalOverlap ||
                                                issue.code == ReasonCode::DuplicateEvidence ||
                                                issue.code == ReasonCode::UnknownSource ||
                                                issue.code == ReasonCode::UnknownEntry ||
                                                issue.code == ReasonCode::DimensionMismatch ||
                                                issue.code == ReasonCode::UnitMismatch;
                                     });

    report.closed = all_closed && !blocked;
    if (report.closed) {
        report.code = ReasonCode::Ok;
        report.explanation = "every dimension reconciles exactly and all evidence is usable";
    } else {
        for (const ClosureIssue& issue : report.issues) {
            if (issue.code == ReasonCode::RecoveredNotCurrent) {
                report.code = ReasonCode::RecoveredNotCurrent;
                report.explanation = "recovered evidence is not current evidence";
                break;
            }
            if (issue.code == ReasonCode::EvidenceNotFresh) {
                report.code = ReasonCode::EvidenceNotFresh;
                report.explanation = "at least one source generation is not fresh";
                break;
            }
            if (issue.code == ReasonCode::AuthorityStale) {
                report.code = ReasonCode::AuthorityStale;
                report.explanation = "at least one contributing source is retired";
                break;
            }
            if (issue.code == ReasonCode::IntervalNotContained) {
                report.code = ReasonCode::IntervalNotContained;
                report.explanation = "an evidence interval crosses the accounting boundary";
                break;
            }
            if (issue.code == ReasonCode::OverAllocation) {
                report.code = ReasonCode::OverAllocation;
                report.explanation = "classifications exceed a measured quantity";
                break;
            }
            if (issue.code == ReasonCode::IntervalOverlap || issue.code == ReasonCode::DuplicateEvidence) {
                report.code = issue.code;
                report.explanation = "conflicting or duplicated evidence is present";
                break;
            }
            if (issue.code == ReasonCode::UnknownSource || issue.code == ReasonCode::UnknownEntry) {
                report.code = issue.code;
                report.explanation = "evidence references an unregistered source or entry";
                break;
            }
            if (issue.code == ReasonCode::DimensionMismatch || issue.code == ReasonCode::UnitMismatch) {
                report.code = issue.code;
                report.explanation = "a quantity cannot be expressed in its dimension";
                break;
            }
        }
        if (report.code == ReasonCode::Ok) {
            if (report.dimensions.empty()) {
                report.code = ReasonCode::NoMeasurements;
                report.explanation = "no live measurement falls inside the requested interval";
            } else {
                for (const DimensionClosure& closure : report.dimensions) {
                    if (!closure.closed) {
                        report.code = closure.code;
                        report.explanation = closure.explanation;
                        break;
                    }
                }
            }
        }
    }

    std::sort(report.issues.begin(), report.issues.end(),
              [](const ClosureIssue& a, const ClosureIssue& b) {
                  if (a.code != b.code) {
                      return static_cast<int>(a.code) < static_cast<int>(b.code);
                  }
                  if (a.subject != b.subject) {
                      return a.subject < b.subject;
                  }
                  return a.detail < b.detail;
              });

    // The digest covers the canonical rendering of everything above, so any
    // change to a contributing fact changes the published digest.
    report.report_digest = Sha256::hash(render_report_body(report));
    return report;
}

}  // namespace fel
