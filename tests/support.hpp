#pragma once

// Shared helpers for the test suites. Header-only so every suite can use them
// without introducing another translation unit into the link.

#include <string>
#include <vector>

#include "fel/fel.hpp"
#include "harness.hpp"

namespace feltest {

using namespace fel;

inline Instant at(const char* text) {
    const Result<Instant> parsed = Instant::parse_iso8601(text);
    if (!parsed.ok()) {
        throw Failure{std::string{"bad test instant: "} + text};
    }
    return parsed.value();
}

inline Interval between(const char* start, const char* end) {
    const Result<Interval> interval = Interval::make(at(start), at(end));
    if (!interval.ok()) {
        throw Failure{std::string{"bad test interval: "} + start + " -> " + end};
    }
    return interval.value();
}

// Deterministic evidence digest derived from a label, so tests never hardcode
// 64-character literals.
inline Digest dig(const std::string& label) { return fel::Sha256::hash(label); }

template <class Tag>
inline fel::Identifier<Tag> id(const std::string& text) {
    const Result<fel::Identifier<Tag>> parsed = fel::Identifier<Tag>::parse(text);
    if (!parsed.ok()) {
        throw Failure{"bad test identifier: " + text};
    }
    return parsed.value();
}

inline Quantity qty(std::int64_t amount, fel::Unit unit) {
    return Quantity{Rational{amount}, unit};
}

inline Digest digest_of_record(const fel::Record& record) { return record.content_digest(); }

struct Fixture {
    explicit Fixture(const std::string& label) : dir(label) {
        StoreOptions options;
        options.mode = OpenMode::Writer;
        options.create_if_missing = true;
        options.ledger_id = id<LedgerIdTag>("test-ledger");
        options.runtime = "fel-tests";
        const Result<Ledger> created = Ledger::open(dir.path(), options);
        if (!created.ok()) {
            throw Failure{"fixture cannot create the ledger: " + created.reason().message()};
        }
        root = created.value().store().directory();
    }

    // Reopens the directory with a fresh writer handle, which is what a separate
    // process would do.
    Result<Ledger> reopen_writer() const {
        StoreOptions options;
        options.mode = OpenMode::Writer;
        return Ledger::open(root, options);
    }

    Result<Ledger> reopen_reader() const {
        StoreOptions options;
        options.mode = OpenMode::Reader;
        return Ledger::open(root, options);
    }

    TempDir dir;
    std::filesystem::path root;
};

// Registers a source, publishes generation 1, and returns the source id.
inline SourceId add_source(Ledger& ledger, const std::string& name, Unit unit,
                           const Instant& now) {
    RegisterSourceRequest request;
    request.source = id<SourceIdTag>(name);
    request.kind = SourceKind::Meter;
    request.unit = unit;
    request.label = name + " (synthetic test meter)";
    request.context.now = now;
    const Result<CommandOutcome> outcome = ledger.register_source(request);
    if (!outcome.ok()) {
        throw Failure{"add_source failed: " + outcome.reason().message()};
    }
    PublishGenerationRequest generation;
    generation.source = request.source;
    generation.generation = Generation{1};
    generation.evidence = dig(name + "-generation-1");
    generation.method = "synthetic";
    generation.context.now = now;
    const Result<CommandOutcome> published = ledger.publish_generation(generation);
    if (!published.ok()) {
        throw Failure{"publish_generation failed: " + published.reason().message()};
    }
    return request.source;
}

inline EntryId add_measurement(Ledger& ledger, const std::string& entry, const SourceId& source,
                               const Interval& interval, std::int64_t amount, Unit unit,
                               const Instant& now) {
    RecordMeasurementRequest request;
    request.entry = id<EntryIdTag>(entry);
    request.interval = interval;
    request.source = source;
    request.generation = Generation{1};
    request.quantity = qty(amount, unit);
    request.evidence = dig(entry);
    request.method = "synthetic";
    request.context.now = now;
    const Result<CommandOutcome> outcome = ledger.record_measurement(request);
    if (!outcome.ok()) {
        throw Failure{"add_measurement failed: " + outcome.reason().message()};
    }
    return request.entry;
}

inline void add_classification(Ledger& ledger, const std::string& allocation, const EntryId& entry,
                               ServiceClass klass, std::int64_t amount, Unit unit,
                               const Instant& now) {
    RecordClassificationRequest request;
    request.allocation = id<AllocationIdTag>(allocation);
    request.entry = entry;
    request.klass = klass;
    request.quantity = qty(amount, unit);
    request.evidence = dig(allocation);
    request.method = "synthetic";
    request.context.now = now;
    const Result<CommandOutcome> outcome = ledger.record_classification(request);
    if (!outcome.ok()) {
        throw Failure{"add_classification failed: " + outcome.reason().message()};
    }
}

}  // namespace feltest
