#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <limits>
#include <string>

using namespace fel;
using namespace feltest;

FEL_TEST(bounds, extreme_quantities_are_refused_rather_than_wrapped) {
    const Rational maximum{std::numeric_limits<std::int64_t>::max()};
    const Quantity huge{maximum, Unit::Joule};
    const Result<Quantity> doubled = huge.add(huge);
    CHECK(!doubled.ok());
    CHECK_EQ(doubled.code(), ReasonCode::ArithmeticOverflow);

    const Quantity huge_energy{maximum, Unit::MegawattHour};
    const Result<Quantity> converted = huge_energy.convert_to(Unit::Joule);
    CHECK(!converted.ok());
    CHECK_EQ(converted.code(), ReasonCode::ArithmeticOverflow);

    const Quantity exact{*Rational::make(1, 1), Unit::MegawattHour};
    CHECK(exact.convert_to(Unit::KilowattHour).value() == feltest::qty(1000, Unit::KilowattHour));
}

FEL_TEST(bounds, oversized_labels_and_identifiers_are_refused) {
    feltest::Fixture fixture("bounds-labels");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    RegisterSourceRequest request;
    request.source = feltest::id<SourceIdTag>("meter-label");
    request.kind = SourceKind::Meter;
    request.unit = Unit::KilowattHour;
    request.label = feltest::repeat('x', 257);
    request.context.now = feltest::at("2026-09-01T00:00:00Z");
    CHECK(ledger.value().register_source(request).code() == ReasonCode::BoundsExceeded);
    request.label = feltest::repeat('x', 256);
    CHECK(ledger.value().register_source(request).ok());
}

FEL_TEST(bounds, unknown_units_and_mismatched_dimensions_are_refused) {
    feltest::Fixture fixture("bounds-units");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-09-01T00:00:00Z");
    const SourceId source = feltest::add_source(ledger.value(), "meter-bounds", Unit::Kilogram, base);

    RecordMeasurementRequest request;
    request.entry = feltest::id<EntryIdTag>("e-bounds");
    request.interval = feltest::between("2026-09-01T00:00:00Z", "2026-09-01T01:00:00Z");
    request.source = source;
    request.generation = Generation{1};
    request.quantity = feltest::qty(5, Unit::GigabyteHour);
    request.evidence = feltest::dig("e-bounds");
    request.context.now = base;
    CHECK(ledger.value().record_measurement(request).code() == ReasonCode::DimensionMismatch);

    request.quantity = Quantity{Rational{5}, static_cast<Unit>(200)};
    CHECK(ledger.value().record_measurement(request).code() == ReasonCode::UnknownUnit);
    CHECK(dimension_of(static_cast<Unit>(200)) == Dimension::Count);
    CHECK(unit_symbol(static_cast<Unit>(200)) == "?");
    CHECK(!unit_from_code(200).has_value());
}

FEL_TEST(bounds, codec_limits_are_enforced_before_allocating) {
    Writer hostile;
    hostile.u32(0xFFFFFFFFU);
    Reader reader{hostile.bytes()};
    CHECK(reader.string().code() == ReasonCode::BoundsExceeded);

    Writer nested;
    nested.u32(0x7FFFFFFFU);
    Reader nested_reader{nested.bytes()};
    CHECK(nested_reader.length_prefixed().code() == ReasonCode::BoundsExceeded);

    Writer empty;
    Reader empty_reader{empty.bytes()};
    CHECK(!empty_reader.u8().ok());
    CHECK(empty_reader.at_end());
}

FEL_TEST(bounds, json_escapes_control_characters_deterministically) {
    Json json = Json::object();
    std::string text;
    text.push_back(static_cast<char>(0x01));
    text.push_back('\t');
    text.push_back('"');
    text.push_back('\\');
    text.push_back(static_cast<char>(0x7F));
    json.set("text", Json::text(text));
    CHECK(json.dump() ==
          "{\"text\":\"\\u0001\\t\\\"\\\\\x7f\"}");
}

FEL_TEST(bounds, interval_duration_is_exact_at_nanosecond_resolution) {
    const Interval interval =
        feltest::between("2026-01-01T00:00:00.000000001Z", "2026-01-01T00:00:01.000000002Z");
    const Result<Rational> duration = interval.duration_seconds();
    REQUIRE(duration.ok());
    CHECK(duration.value() == *Rational::make(1000000001, 1000000000));
}

FEL_TEST(bounds, an_empty_scope_reports_no_measurements_rather_than_closing) {
    feltest::Fixture fixture("bounds-huge-interval");
    Result<Ledger> ledger = fixture.reopen_reader();
    REQUIRE(ledger.ok());
    ReconcileRequest request;
    request.interval = feltest::between("1970-01-01T00:00:00Z", "2100-01-01T00:00:00Z");
    const Result<ReconciliationReport> report = ledger.value().reconcile(request);
    REQUIRE(report.ok());
    CHECK(!report.value().closed);
    CHECK(report.value().code == ReasonCode::NoMeasurements);
    CHECK(report.value().dimensions.empty());
}

FEL_TEST(bounds, rational_denominator_extremes_are_handled) {
    // 1 / INT64_MIN would normalize to -1 / 2^63, and 2^63 is not a
    // representable positive denominator, so the value is refused rather than
    // silently approximated.
    const auto minimum_denominator =
        Rational::make(1, std::numeric_limits<std::int64_t>::min());
    CHECK(!minimum_denominator.has_value());

    // The largest representable negative numerator is still handled exactly.
    const Rational smallest = *Rational::make(std::numeric_limits<std::int64_t>::min() + 1, 3);
    CHECK(smallest.is_negative());
    CHECK(smallest.subtract(smallest).value().is_zero());
    const Result<Rational> restored = smallest.multiply(Rational{3});
    REQUIRE(restored.ok());
    CHECK(restored.value() == Rational{std::numeric_limits<std::int64_t>::min() + 1});
    // Doubling it would exceed the representable range, and that is refused.
    CHECK(!smallest.add(smallest).ok());
}

FEL_TEST(bounds, empty_evidence_and_empty_reasons_are_refused) {
    feltest::Fixture fixture("bounds-evidence");
    Result<Ledger> ledger = fixture.reopen_writer();
    REQUIRE(ledger.ok());
    const Instant base = feltest::at("2026-09-01T00:00:00Z");
    RegisterSourceRequest source;
    source.source = feltest::id<SourceIdTag>("meter-evidence");
    source.kind = SourceKind::Meter;
    source.unit = Unit::KilowattHour;
    source.context.now = base;
    REQUIRE(ledger.value().register_source(source).ok());

    PublishGenerationRequest generation;
    generation.source = source.source;
    generation.generation = Generation{1};
    generation.context.now = base;
    CHECK(ledger.value().publish_generation(generation).code() == ReasonCode::InvalidArgument);
    generation.evidence = feltest::dig("generation");
    REQUIRE(ledger.value().publish_generation(generation).ok());

    RetireSourceRequest retire;
    retire.source = source.source;
    retire.context.now = base;
    CHECK(ledger.value().retire_source(retire).code() == ReasonCode::InvalidArgument);

    VoidTargetRequest void_request;
    void_request.void_id = feltest::id<VoidIdTag>("v-empty-reason");
    void_request.target_kind = TargetKind::SourceRegistration;
    void_request.target_id = "meter-evidence";
    const SourceState* state = ledger.value().index().find_source(source.source);
    REQUIRE(state != nullptr);
    void_request.target_digest = state->registration_digest;
    void_request.context.now = base;
    CHECK(ledger.value().void_target(void_request).code() == ReasonCode::InvalidArgument);
}
