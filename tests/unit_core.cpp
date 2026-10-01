#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <limits>
#include <string>
#include <vector>

using namespace fel;
using namespace feltest;

FEL_TEST(exact, rational_normalizes_sign_and_gcd) {
    const auto value = Rational::make(6, -8);
    REQUIRE(value.has_value());
    CHECK_EQ(value->numerator(), -3);
    CHECK_EQ(value->denominator(), 4);
    CHECK(value->to_string() == "-3/4");
    CHECK(Rational::make(0, 5)->denominator() == 1);
    CHECK(!Rational::make(1, 0).has_value());
}

FEL_TEST(exact, rational_arithmetic_is_exact) {
    const Rational third = *Rational::make(1, 3);
    const Rational sixth = *Rational::make(1, 6);
    const Rational half = third.add(sixth).value();
    CHECK(half == *Rational::make(1, 2));
    CHECK(third.subtract(sixth).value() == *Rational::make(1, 6));
    CHECK(third.multiply(sixth).value() == *Rational::make(1, 18));
    CHECK(third.divide(sixth).value() == Rational{2});
    CHECK(third.divide(Rational{0}).code() == ReasonCode::DivisionByZero);
}

FEL_TEST(exact, rational_handles_extremes_without_wrapping) {
    const Rational maximum{std::numeric_limits<std::int64_t>::max()};
    const Rational minimum{std::numeric_limits<std::int64_t>::min()};
    CHECK(maximum.numerator() == std::numeric_limits<std::int64_t>::max());

    // INT64_MIN cannot be negated, and that refusal is explicit.
    CHECK(minimum.negate().code() == ReasonCode::ArithmeticOverflow);

    // A sum that cannot be represented is refused rather than wrapped.
    const Result<Rational> doubled = maximum.add(maximum);
    CHECK(doubled.code() == ReasonCode::ArithmeticOverflow);

    // exact - exact == 0 even for the extreme values.
    const Result<Rational> cancelled = maximum.add(minimum);
    REQUIRE(cancelled.ok());
    CHECK(cancelled.value() == Rational{-1});
}

FEL_TEST(exact, rational_comparison_is_total) {
    const Rational small = *Rational::make(-1, 3);
    const Rational large = *Rational::make(1, 1000000000);
    CHECK(small.compare(large) < 0);
    CHECK(large.compare(small) > 0);
    CHECK(small.compare(small) == 0);
    const Rational huge = *Rational::make(std::numeric_limits<std::int64_t>::max(), 2);
    CHECK(huge.compare(large) > 0);
}

FEL_TEST(exact, rational_parses_canonical_text_only) {
    CHECK(Rational::parse("7").value() == Rational{7});
    CHECK(Rational::parse("-7/9").value() == *Rational::make(-7, 9));
    CHECK(!Rational::parse("").has_value());
    CHECK(!Rational::parse("1/0").has_value());
    CHECK(!Rational::parse("1.5").has_value());
    CHECK(!Rational::parse("+3").has_value());
    CHECK(!Rational::parse("3 ").has_value());
    CHECK(!Rational::parse("1/2/3").has_value());
    CHECK(!Rational::parse("99999999999999999999").has_value());
}

FEL_TEST(units, conversion_is_exact_and_dimension_safe) {
    const Result<Rational> kwh_to_j = conversion_factor(Unit::KilowattHour, Unit::Joule);
    REQUIRE(kwh_to_j.ok());
    CHECK(kwh_to_j.value() == Rational{3600000});

    const Result<Rational> j_to_kwh = conversion_factor(Unit::Joule, Unit::KilowattHour);
    REQUIRE(j_to_kwh.ok());
    CHECK(j_to_kwh.value() == *Rational::make(1, 3600000));

    CHECK(conversion_factor(Unit::KilowattHour, Unit::Liter).code() ==
          ReasonCode::DimensionMismatch);
}

FEL_TEST(units, decimal_and_binary_data_units_stay_distinct) {
    const Result<Rational> kib_to_byte = conversion_factor(Unit::Kibibyte, Unit::Byte);
    REQUIRE(kib_to_byte.ok());
    CHECK(kib_to_byte.value() == Rational{1024});
    const Result<Rational> kb_to_byte = conversion_factor(Unit::Kilobyte, Unit::Byte);
    REQUIRE(kb_to_byte.ok());
    CHECK(kb_to_byte.value() == Rational{1000});
    const Result<Rational> gib_to_gb = conversion_factor(Unit::Gibibyte, Unit::Gigabyte);
    REQUIRE(gib_to_gb.ok());
    CHECK(gib_to_gb.value() == *Rational::make(1073741824, 1000000000));
}

FEL_TEST(units, symbols_round_trip) {
    for (std::uint8_t code = 0; code < 27; ++code) {
        const auto unit = unit_from_code(code);
        if (!unit.has_value()) {
            continue;
        }
        const auto parsed = unit_from_symbol(unit_symbol(unit.value()));
        REQUIRE(parsed.has_value());
        CHECK(parsed.value() == unit.value());
    }
    CHECK(!unit_from_symbol("fortnight").has_value());
}

FEL_TEST(quantities, addition_requires_an_identical_unit) {
    const Quantity kilowatt_hours = qty(1, Unit::KilowattHour);
    const Quantity joules = qty(3600000, Unit::Joule);
    CHECK(kilowatt_hours.add(joules).code() == ReasonCode::UnitMismatch);
    const Result<Quantity> converted = joules.convert_to(Unit::KilowattHour);
    REQUIRE(converted.ok());
    CHECK(converted.value() == qty(1, Unit::KilowattHour));
    CHECK(kilowatt_hours.add(converted.value()).value() == qty(2, Unit::KilowattHour));
}

FEL_TEST(time, iso8601_round_trips_exactly) {
    const char* samples[] = {"1970-01-01T00:00:00Z",   "2000-02-29T12:34:56Z",
                             "2026-01-01T00:00:00Z",   "9999-12-31T23:59:59Z",
                             "2024-03-01T01:02:03.000000004Z"};
    for (const char* sample : samples) {
        const Result<Instant> parsed = Instant::parse_iso8601(sample);
        REQUIRE(parsed.ok());
        CHECK(parsed.value().to_iso8601() == sample);
    }
}

FEL_TEST(time, iso8601_rejects_malformed_input) {
    const char* samples[] = {"",             "2026-01-01",          "2026-01-01T00:00:00",
                             "2026-01-01T00:00:00+01:00", "2026-13-01T00:00:00Z",
                             "2026-01-32T00:00:00Z", "2026-01-01T24:00:00Z",
                             "2026-01-01T00:00:60Z", "2026-01-01T00:00:00.Z",
                             "2026-01-01T00:00:00Zz"};
    for (const char* sample : samples) {
        CHECK(!Instant::parse_iso8601(sample).ok());
    }
}

FEL_TEST(time, leap_year_boundaries_are_civil_correct) {
    const std::int64_t days = days_from_civil(2024, 2, 29);
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);
    CHECK_EQ(year, 2024);
    CHECK_EQ(month, 2U);
    CHECK_EQ(day, 29U);

    // 1900 is not a leap year, 2000 is.
    CHECK(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28) == 1);
    CHECK(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) == 2);
}

FEL_TEST(time, instant_arithmetic_is_checked) {
    const Instant start = at("2026-01-01T00:00:00.500000000Z");
    CHECK(start.add_seconds(1).value() == at("2026-01-01T00:00:01.500000000Z"));
    CHECK(start.add_nanos(500000000).value() == at("2026-01-01T00:00:01Z"));
    CHECK(start.add_nanos(-1500000000).value() == at("2025-12-31T23:59:59Z"));
    const Instant maximum{std::numeric_limits<std::int64_t>::max(), 0};
    CHECK(maximum.add_seconds(1).code() == ReasonCode::ArithmeticOverflow);
    CHECK(Instant::from_unix_nanos(std::numeric_limits<std::int64_t>::min()).ok());
}

FEL_TEST(time, intervals_are_half_open_and_validated) {
    const Interval interval = between("2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z");
    CHECK(interval.contains(at("2026-01-01T00:00:00Z")));
    CHECK(!interval.contains(at("2026-01-01T01:00:00Z")));
    CHECK(interval.overlaps(between("2026-01-01T00:59:59Z", "2026-01-01T02:00:00Z")));
    CHECK(!interval.overlaps(between("2026-01-01T01:00:00Z", "2026-01-01T02:00:00Z")));
    CHECK(interval.duration_seconds().value() == Rational{3600});
    CHECK(Interval::make(at("2026-01-01T00:00:00Z"), at("2026-01-01T00:00:00Z")).code() ==
          ReasonCode::EmptyInterval);
    CHECK(Interval::make(at("2026-01-01T01:00:00Z"), at("2026-01-01T00:00:00Z")).code() ==
          ReasonCode::InvalidInterval);
}

FEL_TEST(identities, identifiers_are_validated) {
    CHECK(id<SourceIdTag>("meter-1").str() == "meter-1");
    CHECK(SourceId::parse("").code() == ReasonCode::EmptyIdentifier);
    CHECK(SourceId::parse("-leading").code() == ReasonCode::IdentifierCharset);
    CHECK(SourceId::parse("trailing-").code() == ReasonCode::IdentifierCharset);
    CHECK(SourceId::parse("has space").code() == ReasonCode::IdentifierCharset);
    CHECK(SourceId::parse(feltest::repeat('a', 97)).code() == ReasonCode::IdentifierTooLong);
    CHECK(SourceId::parse(feltest::repeat('a', 96)).ok());
    CHECK(!SourceId{}.valid());
}

FEL_TEST(identities, counters_refuse_to_wrap) {
    const Generation maximum{std::numeric_limits<std::uint64_t>::max()};
    CHECK(maximum.next().code() == ReasonCode::ArithmeticOverflow);
    CHECK(Generation{1}.next().value() == Generation{2});
}

FEL_TEST(digests, sha256_matches_published_vectors) {
    CHECK(Sha256::hash(std::string_view{""}).to_hex() ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hash(std::string_view{"abc"}).to_hex() ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hash(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"})
              .to_hex() ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A multi-block message exercises the buffering path.
    const std::string long_message = feltest::repeat('a', 1000000);
    CHECK(Sha256::hash(long_message).to_hex() ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

FEL_TEST(digests, sha256_incremental_matches_one_shot) {
    const std::string part_one = "the facility efficiency ledger ";
    const std::string part_two = "measures useful work and explainable residuals";
    Sha256 incremental;
    incremental.update(std::string_view{part_one});
    incremental.update(std::string_view{part_two});
    CHECK(incremental.finish() == Sha256::hash(part_one + part_two));
}

FEL_TEST(digests, crc32c_matches_the_reference_vector) {
    CHECK_EQ(crc32c(std::string_view{"123456789"}), 0xE3069283U);
    CHECK_EQ(crc32c(std::string_view{""}), 0U);
}

FEL_TEST(digests, hex_round_trips_and_rejects_malformed_text) {
    const Digest digest = dig("round trip");
    CHECK(Digest::from_hex(digest.to_hex()).value() == digest);
    CHECK(!Digest::from_hex("").has_value());
    CHECK(!Digest::from_hex(feltest::repeat('a', 63)).has_value());
    CHECK(!Digest::from_hex(feltest::repeat('z', 64)).has_value());
    CHECK(Digest{}.is_zero());
    CHECK(!digest.is_zero());
}

FEL_TEST(codec, round_trips_every_primitive) {
    Writer writer;
    writer.u8(0xABU);
    writer.u16(0x1234U);
    writer.u32(0xDEADBEEFU);
    writer.u64(0x0123456789ABCDEFULL);
    writer.i64(-42);
    writer.boolean(true);
    writer.string("payload");
    writer.rational(*Rational::make(-22, 7));
    writer.unit(Unit::KilowattHour);
    writer.quantity(qty(3, Unit::GigabyteHour));
    writer.instant(at("2026-05-05T05:05:05.000000005Z"));
    writer.interval(between("2026-05-05T00:00:00Z", "2026-05-05T06:00:00Z"));
    writer.digest(dig("codec"));

    Reader reader{writer.bytes()};
    CHECK_EQ(reader.u8().value(), 0xABU);
    CHECK_EQ(reader.u16().value(), 0x1234U);
    CHECK_EQ(reader.u32().value(), 0xDEADBEEFU);
    CHECK_EQ(reader.u64().value(), 0x0123456789ABCDEFULL);
    CHECK_EQ(reader.i64().value(), -42);
    CHECK(reader.boolean().value());
    CHECK(reader.string().value() == "payload");
    CHECK(reader.rational().value() == *Rational::make(-22, 7));
    CHECK(reader.unit().value() == Unit::KilowattHour);
    CHECK(reader.quantity().value() == qty(3, Unit::GigabyteHour));
    CHECK(reader.instant().value() == at("2026-05-05T05:05:05.000000005Z"));
    CHECK(reader.interval().value() == between("2026-05-05T00:00:00Z", "2026-05-05T06:00:00Z"));
    CHECK(reader.digest().value() == dig("codec"));
    CHECK(reader.at_end());
}

FEL_TEST(codec, refuses_truncated_and_oversized_input) {
    Writer writer;
    writer.u32(7);
    writer.string("text");
    for (std::size_t length = 0; length < writer.size(); ++length) {
        Reader reader{std::span<const std::uint8_t>{writer.bytes().data(), length}};
        const Result<std::uint32_t> value = reader.u32();
        if (length < 4) {
            CHECK(!value.ok());
            CHECK_EQ(value.code(), ReasonCode::RecordCorrupt);
            continue;
        }
        CHECK(value.ok());
        const Result<std::string_view> text = reader.string();
        CHECK(!text.ok() || text.value() == "text");
    }

    // A declared length beyond the permitted maximum is refused before any
    // allocation happens.
    Writer hostile;
    hostile.u32(0xFFFFFFFFU);
    Reader reader{hostile.bytes()};
    CHECK(reader.string().code() == ReasonCode::BoundsExceeded);
}

FEL_TEST(json, canonical_output_sorts_keys_and_escapes_text) {
    Json root = Json::object();
    root.set("zulu", Json::text("last"));
    root.set("alpha", Json::text("first"));
    root.set("quote", Json::text("he said \"hi\"\n"));
    root.set("exact", Json::exact(*Rational::make(-1, 3)));
    Json array = Json::array();
    array.push(Json::integer(1));
    array.push(Json::boolean(false));
    root.set("list", std::move(array));
    CHECK(root.dump() ==
          "{\"alpha\":\"first\",\"exact\":\"-1/3\",\"list\":[1,false],"
          "\"quote\":\"he said \\\"hi\\\"\\n\",\"zulu\":\"last\"}");

    // Insertion order must not affect the rendering.
    Json reordered = Json::object();
    reordered.set("zulu", Json::text("last"));
    reordered.set("list", [] {
        Json array_two = Json::array();
        array_two.push(Json::integer(1));
        array_two.push(Json::boolean(false));
        return array_two;
    }());
    reordered.set("exact", Json::exact(*Rational::make(-1, 3)));
    reordered.set("quote", Json::text("he said \"hi\"\n"));
    reordered.set("alpha", Json::text("first"));
    CHECK(reordered.dump() == root.dump());
}
