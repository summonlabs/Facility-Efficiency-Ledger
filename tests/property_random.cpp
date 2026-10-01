#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace fel;
using namespace feltest;

namespace {

// Deterministic xorshift64* so a failing seed can always be replayed.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

private:
    std::uint64_t state_;
};

struct DimensionChoice {
    Unit unit;
    Unit alternate;
    const char* name;
};

const DimensionChoice kChoices[] = {
    {Unit::KilowattHour, Unit::Joule, "power"},
    {Unit::Liter, Unit::CubicMeter, "water"},
    {Unit::GigabyteHour, Unit::ByteHour, "storage"},
    {Unit::Kilogram, Unit::Kilogram, "mass"},
};

}  // namespace

FEL_TEST(property, randomized_journals_preserve_conservation) {
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        Random random(seed * 0x9E3779B9ULL + 17ULL);
        feltest::Fixture fixture("property-" + std::to_string(seed));
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-06-01T00:00:00Z");
        const Interval interval = feltest::between("2026-06-01T00:00:00Z", "2026-06-01T01:00:00Z");

        const std::size_t source_count = 1 + static_cast<std::size_t>(random.below(3));
        struct Plan {
            EntryId entry;
            Unit unit;
            std::int64_t measured;
        };
        std::vector<Plan> plans;
        for (std::size_t index = 0; index < source_count; ++index) {
            const DimensionChoice choice = kChoices[random.below(4)];
            const std::string name = "meter-" + std::to_string(seed) + "-" + std::to_string(index);
            const SourceId source = feltest::add_source(ledger.value(), name, choice.unit, base);
            const std::int64_t measured = 1 + static_cast<std::int64_t>(random.below(10000));
            const std::string entry_name = "e-" + std::to_string(seed) + "-" + std::to_string(index);
            const EntryId entry = feltest::add_measurement(
                ledger.value(), entry_name, source, interval, measured, choice.unit, base);
            Plan plan;
            plan.entry = entry;
            plan.unit = choice.unit;
            plan.measured = measured;
            std::int64_t remaining = measured;
            const std::size_t parts = 1 + static_cast<std::size_t>(random.below(4));
            for (std::size_t part = 0; part + 1 < parts; ++part) {
                const std::int64_t share =
                    static_cast<std::int64_t>(random.below(static_cast<std::uint64_t>(remaining) + 1U));
                remaining -= share;
                if (share == 0) {
                    continue;
                }
                const ServiceClass klass = static_cast<ServiceClass>(1 + random.below(4));
                feltest::add_classification(ledger.value(),
                                            "a-" + std::to_string(seed) + "-" + std::to_string(index) +
                                                "-" + std::to_string(part),
                                            entry, klass, share, choice.unit, base);
            }
            feltest::add_classification(ledger.value(),
                                        "a-" + std::to_string(seed) + "-" + std::to_string(index) +
                                            "-final",
                                        entry, ServiceClass::Useful, remaining, choice.unit, base);
            plans.push_back(plan);
        }

        for (const Plan& plan : plans) {
            const MeasurementState* state = ledger.value().index().find_measurement(plan.entry);
            REQUIRE(state != nullptr);
            const Result<int> order = state->allocated.compare(state->quantity);
            REQUIRE(order.ok());
            CHECK(order.value() == 0);
        }

        ReconcileRequest request;
        request.interval = interval;
        request.basis = BasisPolicy::Current;
        const Result<ReconciliationReport> report = ledger.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(report.value().closed);
        CHECK(report.value().code == ReasonCode::Ok);
        for (const DimensionClosure& closure : report.value().dimensions) {
            CHECK(closure.input_total == closure.classified_total);
            CHECK(closure.unclassified.is_zero());
            Quantity recomputed = closure.classified_useful;
            const Quantity parts[] = {closure.classified_avoidable, closure.classified_stranded,
                                      closure.classified_wasted, closure.classified_unknown,
                                      closure.classified_unmeasured};
            for (const Quantity& part : parts) {
                const Result<Quantity> sum = recomputed.add(part);
                REQUIRE(sum.ok());
                recomputed = sum.value();
            }
            CHECK(recomputed == closure.classified_total);
        }

        const Result<LedgerView> first = LedgerView::fold(ledger.value().store().records(), base);
        const Result<LedgerView> second = LedgerView::fold(ledger.value().store().records(), base);
        REQUIRE(first.ok());
        REQUIRE(second.ok());
        CHECK_EQ(first.value().measurements().size(), second.value().measurements().size());
        for (std::size_t index = 0; index < first.value().measurements().size(); ++index) {
            CHECK(first.value().measurements()[index].allocated ==
                  second.value().measurements()[index].allocated);
        }
    }
}

FEL_TEST(property, randomized_corrections_leave_the_fold_consistent) {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        Random random(seed * 0x2545F491ULL + 3ULL);
        feltest::Fixture fixture("property-corrections-" + std::to_string(seed));
        Result<Ledger> ledger = fixture.reopen_writer();
        REQUIRE(ledger.ok());
        const Instant base = feltest::at("2026-07-01T00:00:00Z");
        const Interval interval = feltest::between("2026-07-01T00:00:00Z", "2026-07-01T01:00:00Z");
        const SourceId source =
            feltest::add_source(ledger.value(), "meter-correctable", Unit::KilowattHour, base);

        const EntryId root = feltest::add_measurement(ledger.value(), "e-chain-root", source, interval,
                                                      100, Unit::KilowattHour, base);
        EntryId live = root;
        const int revisions = 1 + static_cast<int>(random.below(4));
        for (int step = 0; step < revisions; ++step) {
            const MeasurementState* state = ledger.value().index().find_measurement(live);
            REQUIRE(state != nullptr);
            CHECK(state->superseded_by.empty());
            RecordMeasurementRequest correction;
            correction.entry = feltest::id<EntryIdTag>("e-chain-" + std::to_string(seed) + "-" +
                                                       std::to_string(step));
            correction.interval = interval;
            correction.source = source;
            correction.generation = Generation{1};
            correction.quantity = feltest::qty(100 + step + 1, Unit::KilowattHour);
            correction.evidence = feltest::dig("correction-" + std::to_string(seed) + "-" +
                                               std::to_string(step));
            correction.supersedes = true;
            correction.supersedes_entry = live;
            correction.supersedes_digest = state->record_digest;
            correction.reason = "restated reading";
            correction.context.now = base;
            REQUIRE(ledger.value().record_measurement(correction).ok());
            live = correction.entry;
        }

        std::size_t live_count = 0;
        for (const MeasurementState& measurement : ledger.value().index().measurements()) {
            if (measurement.superseded_by.empty() && !measurement.voided) {
                ++live_count;
                CHECK(measurement.allocated.is_zero());
            }
        }
        CHECK_EQ(live_count, 1U);

        ReconcileRequest request;
        request.interval = interval;
        const Result<ReconciliationReport> report = ledger.value().reconcile(request);
        REQUIRE(report.ok());
        CHECK(!report.value().closed);
        CHECK(report.value().code == ReasonCode::UnclassifiedRemainder);
    }
}
