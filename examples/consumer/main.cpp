// Independent consumer of the installed Facility Efficiency Ledger package.
//
// It uses only the public headers and the exported CMake target, performs a
// complete accounting cycle against a real ledger directory, and fails if the
// installed runtime does not close the interval exactly.
//
// The ledger handle is deliberately scoped: on Windows a directory cannot be
// removed while a handle still has files open inside it, so the handle is released
// before the scratch directory is cleaned up.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <fel/fel.hpp>

namespace {

fel::Digest evidence(const std::string& label) { return fel::Sha256::hash(label); }

}  // namespace

int main(int argc, char** argv) {
    using namespace fel;

    std::cout << "consumer linked against " << kRuntimeIdentity << " " << library_version_string()
              << " (commit " << library_commit_string() << ")\n";

    std::filesystem::path directory;
    bool keep = false;
    if (argc > 1) {
        directory = argv[1];
        keep = true;
    } else {
        directory = std::filesystem::temp_directory_path() / "fel-consumer-ledger";
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    error.clear();

    const Instant start = Instant::parse_iso8601("2026-01-01T00:00:00Z").value();
    const Instant end = Instant::parse_iso8601("2026-01-01T01:00:00Z").value();
    const Interval interval = Interval::make(start, end).value();

    std::string report_json;
    std::string report_digest;
    {
        Result<Ledger> created =
            Ledger::create(directory, LedgerId::parse("consumer-ledger").value(), "fel-consumer");
        if (!created.ok()) {
            std::cerr << "cannot create the ledger: " << created.reason().message() << "\n";
            return 1;
        }
        Ledger& ledger = created.value();

        RegisterSourceRequest source;
        source.source = SourceId::parse("consumer-meter").value();
        source.kind = SourceKind::Meter;
        source.unit = Unit::KilowattHour;
        source.label = "consumer smoke meter";
        source.context.now = start;
        if (!ledger.register_source(source).ok()) {
            std::cerr << "source registration failed\n";
            return 1;
        }

        PublishGenerationRequest generation;
        generation.source = source.source;
        generation.generation = Generation{1};
        generation.evidence = evidence("generation-1");
        generation.context.now = start;
        if (!ledger.publish_generation(generation).ok()) {
            std::cerr << "generation publication failed\n";
            return 1;
        }

        RecordMeasurementRequest measurement;
        measurement.entry = EntryId::parse("consumer-entry").value();
        measurement.interval = interval;
        measurement.source = source.source;
        measurement.generation = Generation{1};
        measurement.quantity = Quantity{Rational{250}, Unit::KilowattHour};
        measurement.evidence = evidence("measurement");
        measurement.context.now = start;
        if (!ledger.record_measurement(measurement).ok()) {
            std::cerr << "measurement failed\n";
            return 1;
        }

        RecordClassificationRequest useful;
        useful.allocation = AllocationId::parse("consumer-useful").value();
        useful.entry = measurement.entry;
        useful.klass = ServiceClass::Useful;
        useful.quantity = Quantity{Rational{200}, Unit::KilowattHour};
        useful.evidence = evidence("useful");
        useful.context.now = start;
        if (!ledger.record_classification(useful).ok()) {
            std::cerr << "classification failed\n";
            return 1;
        }

        ReconcileRequest request;
        request.interval = interval;
        Result<ReconciliationReport> report = ledger.reconcile(request);
        if (!report.ok()) {
            std::cerr << "reconcile failed: " << report.reason().message() << "\n";
            return 1;
        }
        if (report.value().closed) {
            std::cerr << "an incompletely classified interval reported closure\n";
            return 1;
        }

        RecordResidualRequest residual;
        residual.residual = ResidualId::parse("consumer-unknown").value();
        residual.interval = interval;
        residual.klass = ServiceClass::Unknown;
        residual.quantified = true;
        residual.quantity = Quantity{Rational{50}, Unit::KilowattHour};
        residual.bound_to_entry = true;
        residual.entry = measurement.entry;
        residual.basis = "the consumer cannot separate two loads behind one meter";
        residual.evidence = evidence("unknown");
        residual.context.now = start;
        if (!ledger.record_residual(residual).ok()) {
            std::cerr << "residual failed\n";
            return 1;
        }

        report = ledger.reconcile(request);
        if (!report.ok() || !report.value().closed) {
            std::cerr << "the interval did not reconcile after classification\n";
            return 1;
        }
        report_json = render_json(report.value());
        report_digest = report.value().report_digest.to_hex();
    }

    std::cout << report_json << "\n";
    std::cout << "consumer verification succeeded; report digest " << report_digest << "\n";

    if (!keep) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        if (cleanup_error) {
            std::cerr << "cannot remove the scratch ledger at " << directory.string() << ": "
                      << cleanup_error.message() << "\n";
            return 1;
        }
    }
    return 0;
}
