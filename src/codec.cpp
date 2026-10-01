#include "fel/codec.hpp"

#include <utility>

namespace fel {
namespace {

constexpr std::size_t kMaxNestedBytes = 16U * 1024U * 1024U;

}  // namespace

void Writer::u8(std::uint8_t value) { buffer_.push_back(value); }

void Writer::u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void Writer::u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Writer::boolean(bool value) { u8(value ? 1U : 0U); }

void Writer::raw(std::span<const std::uint8_t> data) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void Writer::string(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    const auto* begin = reinterpret_cast<const std::uint8_t*>(text.data());
    buffer_.insert(buffer_.end(), begin, begin + text.size());
}

void Writer::rational(const Rational& value) {
    i64(value.numerator());
    i64(value.denominator());
}

void Writer::unit(Unit value) { u8(unit_code(value)); }

void Writer::quantity(const Quantity& value) {
    rational(value.amount());
    unit(value.unit());
}

void Writer::instant(const Instant& value) {
    i64(value.seconds());
    i64(static_cast<std::int64_t>(value.nanos()));
}

void Writer::interval(const Interval& value) {
    instant(value.start());
    instant(value.end());
}

void Writer::digest(const Digest& value) { raw(value.bytes()); }

Result<bool> Reader::ensure(std::size_t count) {
    if (remaining() < count) {
        return Reason{ReasonCode::RecordCorrupt,
                      "truncated payload: needed " + std::to_string(count) + " bytes, " +
                          std::to_string(remaining()) + " remain"};
    }
    return true;
}

Result<std::uint8_t> Reader::u8() {
    const Result<bool> ok = ensure(1);
    if (!ok.ok()) {
        return ok.reason();
    }
    return data_[offset_++];
}

Result<std::uint16_t> Reader::u16() {
    const Result<bool> ok = ensure(2);
    if (!ok.ok()) {
        return ok.reason();
    }
    std::uint16_t value = 0;
    for (unsigned shift = 0; shift < 16U; shift += 8U) {
        value = static_cast<std::uint16_t>(value |
                                          static_cast<std::uint16_t>(data_[offset_++] << shift));
    }
    return value;
}

Result<std::uint32_t> Reader::u32() {
    const Result<bool> ok = ensure(4);
    if (!ok.ok()) {
        return ok.reason();
    }
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(data_[offset_++]) << shift;
    }
    return value;
}

Result<std::uint64_t> Reader::u64() {
    const Result<bool> ok = ensure(8);
    if (!ok.ok()) {
        return ok.reason();
    }
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(data_[offset_++]) << shift;
    }
    return value;
}

Result<std::int64_t> Reader::i64() {
    const Result<std::uint64_t> raw = u64();
    if (!raw.ok()) {
        return raw.reason();
    }
    return static_cast<std::int64_t>(raw.value());
}

Result<bool> Reader::boolean() {
    const Result<std::uint8_t> raw = u8();
    if (!raw.ok()) {
        return raw.reason();
    }
    if (raw.value() > 1U) {
        return Reason{ReasonCode::RecordCorrupt, "boolean field is not 0 or 1"};
    }
    return raw.value() == 1U;
}

Result<std::span<const std::uint8_t>> Reader::raw(std::size_t count) {
    const Result<bool> ok = ensure(count);
    if (!ok.ok()) {
        return ok.reason();
    }
    const std::span<const std::uint8_t> view = data_.subspan(offset_, count);
    offset_ += count;
    return view;
}

Result<std::string_view> Reader::string() {
    const Result<std::uint32_t> length = u32();
    if (!length.ok()) {
        return length.reason();
    }
    if (length.value() > kMaxEncodedStringLength) {
        return Reason{ReasonCode::BoundsExceeded,
                      "string field declares " + std::to_string(length.value()) +
                          " bytes which exceeds the permitted maximum"};
    }
    const Result<std::span<const std::uint8_t>> view = raw(length.value());
    if (!view.ok()) {
        return view.reason();
    }
    return std::string_view{reinterpret_cast<const char*>(view.value().data()), view.value().size()};
}

Result<Rational> Reader::rational() {
    const Result<std::int64_t> numerator = i64();
    if (!numerator.ok()) {
        return numerator.reason();
    }
    const Result<std::int64_t> denominator = i64();
    if (!denominator.ok()) {
        return denominator.reason();
    }
    const auto value = Rational::make(numerator.value(), denominator.value());
    if (!value.has_value()) {
        return Reason{ReasonCode::RecordCorrupt,
                      "encoded rational " + std::to_string(numerator.value()) + "/" +
                          std::to_string(denominator.value()) + " is not a valid exact value"};
    }
    return *value;
}

Result<Unit> Reader::unit() {
    const Result<std::uint8_t> code = u8();
    if (!code.ok()) {
        return code.reason();
    }
    const auto value = unit_from_code(code.value());
    if (!value.has_value()) {
        return Reason{ReasonCode::UnknownUnit,
                      "encoded unit code " + std::to_string(code.value()) + " is not registered"};
    }
    return *value;
}

Result<Quantity> Reader::quantity() {
    const Result<Rational> amount = rational();
    if (!amount.ok()) {
        return amount.reason();
    }
    const Result<Unit> unit = this->unit();
    if (!unit.ok()) {
        return unit.reason();
    }
    return Quantity{amount.value(), unit.value()};
}

Result<Instant> Reader::instant() {
    const Result<std::int64_t> seconds = i64();
    if (!seconds.ok()) {
        return seconds.reason();
    }
    const Result<std::int64_t> nanos = i64();
    if (!nanos.ok()) {
        return nanos.reason();
    }
    if (nanos.value() < 0 || nanos.value() >= kNanosPerSecond) {
        return Reason{ReasonCode::RecordCorrupt, "encoded nanosecond field is out of range"};
    }
    return Instant{seconds.value(), static_cast<std::int32_t>(nanos.value())};
}

Result<Interval> Reader::interval() {
    const Result<Instant> start = instant();
    if (!start.ok()) {
        return start.reason();
    }
    const Result<Instant> end = instant();
    if (!end.ok()) {
        return end.reason();
    }
    return Interval::make(start.value(), end.value());
}

Result<Digest> Reader::digest() {
    const Result<std::span<const std::uint8_t>> view = raw(kDigestBytes);
    if (!view.ok()) {
        return view.reason();
    }
    std::array<std::uint8_t, kDigestBytes> bytes{};
    for (std::size_t i = 0; i < kDigestBytes; ++i) {
        bytes[i] = view.value()[i];
    }
    return Digest{bytes};
}

Result<Reader> Reader::length_prefixed() {
    const Result<std::uint32_t> length = u32();
    if (!length.ok()) {
        return length.reason();
    }
    if (length.value() > kMaxNestedBytes) {
        return Reason{ReasonCode::BoundsExceeded,
                      "nested block declares " + std::to_string(length.value()) +
                          " bytes which exceeds the permitted maximum"};
    }
    const Result<std::span<const std::uint8_t>> view = raw(length.value());
    if (!view.ok()) {
        return view.reason();
    }
    return Reader{view.value()};
}

}  // namespace fel
