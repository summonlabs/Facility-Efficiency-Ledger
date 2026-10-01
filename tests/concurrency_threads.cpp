#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <atomic>
#include <latch>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fel;

// Concurrency tests for the ledger's thread-safe command surface.
//
// A note on construction: std::shared_mutex gives no fairness guarantee, so a
// writer that repeatedly takes the exclusive lock can delay readers. These tests
// therefore never assume that a reader thread has been scheduled by the time the
// writer finishes. Instead every reader publishes a rendezvous (a std::latch) after
// its first completed read, and the writer waits for that rendezvous before it
// starts. The assertions that follow are then facts about the library rather than
// facts about the scheduler.

namespace {

constexpr int kWriterThreads = 8;
constexpr int kCommandsPerThread = 25;
constexpr int kReaderThreads = 4;

struct Counters {
    std::atomic<int> committed{0};
    std::atomic<int> refused{0};
    std::atomic<int> reads{0};
    std::mutex errors_mutex;
    std::vector<std::string> errors;

    void record(const std::string& text) {
        std::lock_guard<std::mutex> guard(errors_mutex);
        errors.push_back(text);
    }

    void require_clean(const std::string& context) const {
        if (errors.empty()) {
            return;
        }
        throw feltest::Failure{context + ": " + errors.front()};
    }
};

// Publishes one rendezvous per reader after that reader has completed one read.
class FirstRead {
public:
    FirstRead() : latch_(static_cast<std::ptrdiff_t>(kReaderThreads)) {}

    void arrive() { latch_.count_down(); }
    void wait() const { latch_.wait(); }

private:
    std::latch latch_;
};

}  // namespace

FEL_TEST(concurrency, concurrent_commands_are_serialised_exactly_once) {
    feltest::TempDir directory("threads-commands");
    CHECK(Ledger::create(directory.path(), feltest::id<LedgerIdTag>("threaded-ledger"), "fel-tests").ok());
    Result<Ledger> ledger = Ledger::open(directory.path(),
                                         StoreOptions{OpenMode::Writer, false, {}, {}, {}});
    REQUIRE(ledger.ok());

    Counters counters;
    std::vector<std::thread> writers;
    writers.reserve(kWriterThreads);
    for (int thread_index = 0; thread_index < kWriterThreads; ++thread_index) {
        writers.emplace_back([&ledger, &counters, thread_index] {
            for (int command = 0; command < kCommandsPerThread; ++command) {
                RegisterSourceRequest request;
                request.source = feltest::id<SourceIdTag>("thread-" + std::to_string(thread_index) +
                                                          "-" + std::to_string(command));
                request.kind = SourceKind::Meter;
                request.unit = Unit::KilowattHour;
                request.label = "concurrently registered";
                request.context.now = feltest::at("2026-11-01T00:00:00Z");
                const Result<CommandOutcome> outcome = ledger.value().register_source(request);
                if (!outcome.ok()) {
                    counters.refused.fetch_add(1);
                    counters.record(outcome.reason().message());
                    continue;
                }
                if (outcome.value().applied) {
                    counters.committed.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    counters.require_clean("a concurrent command was refused");
    CHECK_EQ(counters.refused.load(), 0);
    CHECK_EQ(counters.committed.load(), kWriterThreads * kCommandsPerThread);
    // Every command advanced the revision by exactly one.
    CHECK_EQ(ledger.value().revision().value(),
             static_cast<std::uint64_t>(kWriterThreads * kCommandsPerThread));
    const Result<LedgerView> view = ledger.value().view(Instant{});
    REQUIRE(view.ok());
    CHECK_EQ(view.value().sources().size(),
             static_cast<std::size_t>(kWriterThreads * kCommandsPerThread));
    CHECK(view.value().issues().empty());
}

FEL_TEST(concurrency, readers_observe_consistent_snapshots_while_writers_run) {
    feltest::TempDir directory("threads-readers");
    CHECK(Ledger::create(directory.path(), feltest::id<LedgerIdTag>("threaded-readers"), "fel-tests").ok());
    Result<Ledger> ledger = Ledger::open(directory.path(),
                                         StoreOptions{OpenMode::Writer, false, {}, {}, {}});
    REQUIRE(ledger.ok());

    Counters counters;
    std::atomic<bool> stop{false};
    FirstRead first_read;
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    for (int index = 0; index < kReaderThreads; ++index) {
        readers.emplace_back([&ledger, &counters, &stop, &first_read] {
            // The latch must be counted down exactly once per reader:
            // std::latch::count_down requires a non-zero counter, so an
            // unconditional call inside the loop would be undefined behaviour.
            bool announced = false;
            const auto announce = [&announced, &first_read] {
                if (!announced) {
                    announced = true;
                    first_read.arrive();
                }
            };
            std::uint64_t previous_revision = 0;
            while (!stop.load()) {
                const Result<LedgerView> view = ledger.value().view(feltest::at("2026-11-01T00:00:00Z"));
                if (!view.ok()) {
                    counters.record(view.reason().message());
                    announce();
                    continue;
                }
                // Revisions observed by one reader never move backwards.
                if (view.value().revision().value() < previous_revision) {
                    counters.record("a read observed a revision that moved backwards");
                }
                previous_revision = view.value().revision().value();
                // The snapshot is internally consistent: no live measurement is
                // ever over-allocated in an observable state.
                for (const MeasurementState& measurement : view.value().measurements()) {
                    const Result<int> order = measurement.allocated.compare(measurement.quantity);
                    if (!order.ok() || order.value() > 0) {
                        counters.record("a read observed an over-allocated entry");
                    }
                }
                counters.reads.fetch_add(1);
                announce();
            }
            announce();
        });
    }
    first_read.wait();

    std::vector<std::thread> writers;
    writers.reserve(kWriterThreads);
    for (int thread_index = 0; thread_index < kWriterThreads; ++thread_index) {
        writers.emplace_back([&ledger, &counters, thread_index] {
            for (int command = 0; command < kCommandsPerThread; ++command) {
                const std::string name = "reader-writer-" + std::to_string(thread_index) + "-" +
                                         std::to_string(command);
                const SourceId source = feltest::id<SourceIdTag>(name);
                RegisterSourceRequest request;
                request.source = source;
                request.kind = SourceKind::Meter;
                request.unit = Unit::KilowattHour;
                request.context.now = feltest::at("2026-11-01T00:00:00Z");
                if (!ledger.value().register_source(request).ok()) {
                    counters.record("registration refused during a concurrent read");
                    continue;
                }
                PublishGenerationRequest generation;
                generation.source = source;
                generation.generation = Generation{1};
                generation.evidence = feltest::dig(name + "-generation");
                generation.context.now = feltest::at("2026-11-01T00:00:00Z");
                if (!ledger.value().publish_generation(generation).ok()) {
                    counters.record("generation refused during a concurrent read");
                }
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    counters.require_clean("concurrent read defect");
    CHECK(counters.reads.load() > 0);
}

FEL_TEST(concurrency, sealing_while_readers_run_takes_locks_in_one_direction) {
    // Sealing holds the exclusive lock and needs a reconciliation internally.
    // The reconciliation must not take a lock of its own: upgrading or
    // re-entering here would deadlock, and the test would hang rather than fail.
    feltest::TempDir directory("threads-seal");
    CHECK(Ledger::create(directory.path(), feltest::id<LedgerIdTag>("threaded-seal"), "fel-tests").ok());
    Result<Ledger> ledger = Ledger::open(directory.path(),
                                         StoreOptions{OpenMode::Writer, false, {}, {}, {}});
    REQUIRE(ledger.ok());
    const Interval interval = feltest::between("2026-11-01T00:00:00Z", "2026-11-01T01:00:00Z");
    const Instant base = feltest::at("2026-11-01T00:00:00Z");
    const SourceId source = feltest::add_source(ledger.value(), "meter-seal-thread", Unit::KilowattHour, base);
    const EntryId entry = feltest::add_measurement(ledger.value(), "e-seal-thread", source, interval, 10,
                                                   Unit::KilowattHour, base);
    feltest::add_classification(ledger.value(), "a-seal-thread", entry, ServiceClass::Useful, 10,
                                Unit::KilowattHour, base);

    Counters counters;
    std::atomic<bool> stop{false};
    FirstRead first_read;
    std::vector<std::thread> readers;
    for (int index = 0; index < kReaderThreads; ++index) {
        readers.emplace_back([&ledger, &counters, &stop, &interval, &first_read] {
            bool announced = false;
            const auto announce = [&announced, &first_read] {
                if (!announced) {
                    announced = true;
                    first_read.arrive();
                }
            };
            ReconcileRequest request;
            request.interval = interval;
            while (!stop.load()) {
                const Result<ReconciliationReport> report = ledger.value().reconcile(request);
                if (!report.ok()) {
                    counters.record(report.reason().message());
                    announce();
                    continue;
                }
                if (!report.value().closed) {
                    counters.record("a completed interval stopped reconciling");
                }
                counters.reads.fetch_add(1);
                announce();
            }
            announce();
        });
    }
    first_read.wait();

    SealIntervalRequest seal;
    seal.seal = feltest::id<SealIdTag>("seal-thread");
    seal.interval = interval;
    seal.context.now = feltest::at("2026-11-01T01:00:00Z");
    const Result<CommandOutcome> outcome = ledger.value().seal_interval(seal);
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE(outcome.ok());
    CHECK(outcome.value().applied);
    counters.require_clean("sealing while readers reconciled");
    CHECK(counters.reads.load() > 0);

    ReconcileRequest request;
    request.interval = interval;
    const Result<ReconciliationReport> report = ledger.value().reconcile(request);
    REQUIRE(report.ok());
    CHECK(report.value().sealed);
    CHECK_EQ(report.value().seal.str(), "seal-thread");
}

FEL_TEST(concurrency, compaction_is_exclusive_and_leaves_the_ledger_usable) {
    feltest::TempDir directory("threads-compact");
    CHECK(Ledger::create(directory.path(), feltest::id<LedgerIdTag>("threaded-compact"), "fel-tests").ok());
    Result<Ledger> ledger = Ledger::open(directory.path(),
                                         StoreOptions{OpenMode::Writer, false, {}, {}, {}});
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-11-01T00:00:00Z");
    for (int index = 0; index < 20; ++index) {
        RegisterSourceRequest request;
        request.source = feltest::id<SourceIdTag>("compact-" + std::to_string(index));
        request.kind = SourceKind::Meter;
        request.unit = Unit::KilowattHour;
        request.context.now = base;
        REQUIRE(ledger.value().register_source(request).ok());
    }

    Counters counters;
    std::atomic<bool> stop{false};
    FirstRead first_read;
    std::vector<std::thread> readers;
    for (int index = 0; index < kReaderThreads; ++index) {
        readers.emplace_back([&ledger, &counters, &stop, &first_read] {
            bool announced = false;
            const auto announce = [&announced, &first_read] {
                if (!announced) {
                    announced = true;
                    first_read.arrive();
                }
            };
            while (!stop.load()) {
                const Result<LedgerView> view = ledger.value().view(Instant{});
                if (!view.ok()) {
                    counters.record(view.reason().message());
                    announce();
                    continue;
                }
                if (view.value().sources().size() != 20U) {
                    counters.record("a read observed a partially applied compaction");
                }
                counters.reads.fetch_add(1);
                announce();
            }
            announce();
        });
    }
    first_read.wait();

    const Result<std::uint64_t> dropped = ledger.value().compact();
    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }
    REQUIRE(dropped.ok());
    counters.require_clean("compaction while readers folded");
    CHECK(counters.reads.load() > 0);
    const Result<LedgerView> view = ledger.value().view(Instant{});
    REQUIRE(view.ok());
    CHECK_EQ(view.value().sources().size(), 20U);
    CHECK(!ledger.value().store().base_snapshot_name().empty());
}
