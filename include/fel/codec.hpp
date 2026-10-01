#pragma once

// Canonical little-endian binary encoding used by the durable formats.
//
// The encoding is fixed-width and self-describing enough for a reader to reject
// truncated or oversized input without allocating unbounded memory: every
// variable-length field carries a 32-bit length that is validated against an
// explicit maximum before use.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fel/digest.hpp"
#include "fel/quantity.hpp"
#include "fel/status.hpp"
#include "fel/time.hpp"

namespace fel {

inline constexpr std::uint32_t kMaxEncodedStringLength = 65536;
inline constexpr std::uint32_t kMaxCollectionItems = 65536;

class Writer {
public:
    Writer() = default;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value);
    void raw(std::span<const std::uint8_t> data);
    void string(std::string_view text);
    void rational(const Rational& value);
    void unit(Unit value);
    void quantity(const Quantity& value);
    void instant(const Instant& value);
    void interval(const Interval& value);
    void digest(const Digest& value);

    // Prefixes a nested block with its byte length so that a reader can skip or
    // bound it before decoding.
    template <class Body>
    void length_prefixed(Body&& body) {
        Writer nested;
        body(nested);
        u32(static_cast<std::uint32_t>(nested.size()));
        raw(nested.bytes());
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
    std::vector<std::uint8_t> take() { return std::move(buffer_); }
    std::size_t size() const noexcept { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_{};
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    Result<std::uint8_t> u8();
    Result<std::uint16_t> u16();
    Result<std::uint32_t> u32();
    Result<std::uint64_t> u64();
    Result<std::int64_t> i64();
    Result<bool> boolean();
    Result<std::span<const std::uint8_t>> raw(std::size_t count);
    Result<std::string_view> string();
    Result<Rational> rational();
    Result<Unit> unit();
    Result<Quantity> quantity();
    Result<Instant> instant();
    Result<Interval> interval();
    Result<Digest> digest();

    // Returns a sub-reader over the next length-prefixed block.
    Result<Reader> length_prefixed();

    bool at_end() const noexcept { return offset_ == data_.size(); }
    std::size_t remaining() const noexcept { return data_.size() - offset_; }
    std::size_t offset() const noexcept { return offset_; }
    std::span<const std::uint8_t> rest() const noexcept { return data_.subspan(offset_); }

private:
    Result<bool> ensure(std::size_t count);

    std::span<const std::uint8_t> data_{};
    std::size_t offset_{0};
};

}  // namespace fel
