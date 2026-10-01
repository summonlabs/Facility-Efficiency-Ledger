// Scripted child process used to prove real multiprocess behaviour: durable
// commits surviving process death, kernel-enforced single-writer locking, torn
// tails produced by a genuinely killed writer, and cross-process state equality.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fel/fel.hpp"

namespace {

using namespace fel;

Digest evidence_digest(const std::string& seed) { return Sha256::hash(seed); }

int write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return 1;
    }
    stream << text;
    stream.flush();
    return stream ? 0 : 1;
}

int command_hold_lock(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3) {
        return 2;
    }
    const std::filesystem::path directory = arguments[0];
    const std::filesystem::path ready = arguments[1];
    const std::filesystem::path release = arguments[2];
    StoreOptions options;
    options.mode = OpenMode::Writer;
    const Result<Store> store = Store::open(directory, options);
    if (!store.ok()) {
        if (write_text(ready, "refused " + std::string(to_string(store.code()))) != 0) {
            return 1;
        }
        return 0;
    }
    if (write_text(ready, "held") != 0) {
        return 1;
    }
    // Bounded wait for the parent's release marker. Exceeding the bound is a
    // defect, reported as a distinct exit code rather than hanging forever.
    constexpr int kMaxIterations = 60000;
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        std::error_code error;
        if (std::filesystem::exists(release, error) && !error) {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 3;
}

int command_try_open(const std::vector<std::string>& arguments, bool writer) {
    if (arguments.size() < 2) {
        return 2;
    }
    StoreOptions options;
    options.mode = writer ? OpenMode::Writer : OpenMode::Reader;
    const Result<Store> store = Store::open(arguments[0], options);
    if (!store.ok()) {
        return write_text(arguments[1], "refused " + std::string(to_string(store.code())));
    }
    return write_text(arguments[1], "opened");
}

int command_append_then_exit(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return 2;
    }
    const std::filesystem::path directory = arguments[0];
    const int count = std::stoi(arguments[1]);
    const std::string prefix = arguments.size() > 2 ? arguments[2] : std::string{"crash-source"};
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(directory, options);
    if (!ledger.ok()) {
        return 1;
    }
    for (int index = 0; index < count; ++index) {
        RegisterSourceRequest request;
        request.source = SourceId::parse(prefix + "-" + std::to_string(index)).value();
        request.kind = SourceKind::Meter;
        request.unit = Unit::KilowattHour;
        request.label = "written before an abrupt process death";
        request.context.now = platform::system_utc_now();
        const Result<CommandOutcome> outcome = ledger.value().register_source(request);
        if (!outcome.ok()) {
            return 1;
        }
    }
    std::cout.flush();
    // Terminate without unwinding: no destructors, no lock release by the
    // runtime, exactly like a killed process.
    std::_Exit(0);
}

// Simulates a writer that died midway through appending one record: a complete
// and valid record header followed by only part of its payload. This is exactly
// the shape a real torn write leaves behind.
int command_tear_tail(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return 2;
    }
    const std::filesystem::path directory = arguments[0];
    const std::size_t prefix_bytes = static_cast<std::size_t>(std::stoul(arguments[1]));
    std::error_code error;
    std::filesystem::path segment;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("fel-segment-", 0) == 0) {
            segment = entry.path();
        }
    }
    if (segment.empty()) {
        return 1;
    }

    MeasurementRecordedBody body;
    body.entry = EntryId::parse("e-torn-write").value();
    body.interval = Interval::make(Instant::parse_iso8601("2026-10-01T00:00:00Z").value(),
                                   Instant::parse_iso8601("2026-10-01T01:00:00Z").value())
                        .value();
    body.source = SourceId::parse("meter-torn-write").value();
    body.generation = Generation{1};
    body.quantity = Quantity{Rational{1}, Unit::KilowattHour};
    body.evidence = evidence_digest("torn write");
    body.recorded_at = Instant::parse_iso8601("2026-10-01T01:00:00Z").value();
    Record record = Record::make_measurement(body, MutationMeta{});
    record.set_sequence(Sequence{1000000});
    record.set_revision(Revision{0});
    record.set_epoch(Epoch{1});
    const EncodedFrame frame = encode_record_frame(record, Digest{});
    if (prefix_bytes == 0 || prefix_bytes >= frame.bytes.size()) {
        return 2;
    }

    std::ofstream stream(segment, std::ios::binary | std::ios::app);
    if (!stream) {
        return 1;
    }
    stream.write(reinterpret_cast<const char*>(frame.bytes.data()),
                 static_cast<std::streamsize>(prefix_bytes));
    stream.flush();
    if (!stream) {
        return 1;
    }
    std::_Exit(0);
}
int command_fold_digest(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return 2;
    }
    StoreOptions options;
    options.mode = OpenMode::Reader;
    const Result<Ledger> ledger = Ledger::open(arguments[0], options);
    if (!ledger.ok()) {
        return write_text(arguments[1], "refused " + std::string(to_string(ledger.code())));
    }
    const Result<LedgerView> view = ledger.value().view(Instant{});
    if (!view.ok()) {
        return write_text(arguments[1], "refused " + std::string(to_string(view.code())));
    }
    std::string text;
    text.append("revision=").append(std::to_string(view.value().revision().value()));
    text.append(" epoch=").append(std::to_string(view.value().epoch().value()));
    text.append(" sources=").append(std::to_string(view.value().sources().size()));
    text.append(" measurements=").append(std::to_string(view.value().measurements().size()));
    text.append(" allocations=").append(std::to_string(view.value().allocations().size()));
    text.append(" residuals=").append(std::to_string(view.value().residuals().size()));
    text.append(" seals=").append(std::to_string(view.value().seals().size()));
    text.append(" chain=").append(view.value().chain_head().to_hex());
    return write_text(arguments[1], text);
}

int command_populate(const std::vector<std::string>& arguments) {
    if (arguments.size() < 1) {
        return 2;
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    options.create_if_missing = true;
    options.ledger_id = LedgerId::parse("helper-ledger").value();
    options.runtime = "fel-test-helper";
    Result<Ledger> ledger = Ledger::open(arguments[0], options);
    if (!ledger.ok()) {
        return 1;
    }
    const Instant start = Instant::parse_iso8601("2026-03-01T00:00:00Z").value();
    const Instant end = Instant::parse_iso8601("2026-03-01T01:00:00Z").value();
    RegisterSourceRequest source;
    source.source = SourceId::parse("meter-shared").value();
    source.kind = SourceKind::Meter;
    source.unit = Unit::KilowattHour;
    source.label = "shared synthetic meter";
    source.context.now = start;
    if (!ledger.value().register_source(source).ok()) {
        return 1;
    }
    PublishGenerationRequest generation;
    generation.source = source.source;
    generation.generation = Generation{1};
    generation.evidence = evidence_digest("generation-1");
    generation.method = "synthetic";
    generation.context.now = start;
    if (!ledger.value().publish_generation(generation).ok()) {
        return 1;
    }
    RecordMeasurementRequest measurement;
    measurement.entry = EntryId::parse("e-shared").value();
    measurement.interval = Interval::make(start, end).value();
    measurement.source = source.source;
    measurement.generation = Generation{1};
    measurement.quantity = Quantity{Rational{100}, Unit::KilowattHour};
    measurement.evidence = evidence_digest("measurement-shared");
    measurement.context.now = start;
    if (!ledger.value().record_measurement(measurement).ok()) {
        return 1;
    }
    RecordClassificationRequest allocation;
    allocation.allocation = AllocationId::parse("a-shared").value();
    allocation.entry = measurement.entry;
    allocation.klass = ServiceClass::Useful;
    allocation.quantity = Quantity{Rational{100}, Unit::KilowattHour};
    allocation.evidence = evidence_digest("allocation-shared");
    allocation.context.now = start;
    if (!ledger.value().record_classification(allocation).ok()) {
        return 1;
    }
    SealIntervalRequest seal;
    seal.seal = SealId::parse("seal-shared").value();
    seal.interval = measurement.interval;
    seal.note = "sealed by the helper process";
    seal.context.now = end;
    if (!ledger.value().seal_interval(seal).ok()) {
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const std::string command = argv[1];
    std::vector<std::string> arguments;
    for (int index = 2; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    if (command == "hold-lock") {
        return command_hold_lock(arguments);
    }
    if (command == "try-writer-open") {
        return command_try_open(arguments, true);
    }
    if (command == "try-reader-open") {
        return command_try_open(arguments, false);
    }
    if (command == "append-then-exit") {
        return command_append_then_exit(arguments);
    }
    if (command == "tear-tail") {
        return command_tear_tail(arguments);
    }
    if (command == "fold-digest") {
        return command_fold_digest(arguments);
    }
    if (command == "populate") {
        return command_populate(arguments);
    }
    return 2;
}
