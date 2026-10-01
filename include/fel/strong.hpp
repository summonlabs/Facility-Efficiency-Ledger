#pragma once

// Strongly typed identifiers and monotonically increasing counters.
//
// Identifiers are validated at construction; a default-constructed identifier
// is explicitly invalid rather than silently equal to some sentinel string.
// Counters wrap a 64-bit unsigned value and refuse to advance past the maximum
// representable value instead of wrapping.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "fel/status.hpp"

namespace fel {

inline constexpr std::size_t kMaxIdentifierLength = 96;

// True when the identifier text is well formed: 1..96 characters drawn from
// [A-Za-z0-9._:-], starting and ending with an alphanumeric character.
bool is_valid_identifier_text(std::string_view text) noexcept;

template <class Tag>
class Identifier {
public:
    Identifier() noexcept = default;

    static Result<Identifier> parse(std::string_view text) {
        if (text.empty()) {
            return Reason{ReasonCode::EmptyIdentifier, "identifier is empty"};
        }
        if (text.size() > kMaxIdentifierLength) {
            return Reason{ReasonCode::IdentifierTooLong,
                          "identifier length " + std::to_string(text.size()) + " exceeds " +
                              std::to_string(kMaxIdentifierLength)};
        }
        if (!is_valid_identifier_text(text)) {
            return Reason{ReasonCode::IdentifierCharset,
                          "identifier \"" + std::string(text) +
                              "\" contains characters outside [A-Za-z0-9._:-] or has a "
                              "non-alphanumeric boundary"};
        }
        Identifier out;
        out.text_.assign(text);
        return out;
    }

    bool valid() const noexcept { return !text_.empty(); }
    const std::string& str() const noexcept { return text_; }
    std::string_view view() const noexcept { return text_; }

    friend bool operator==(const Identifier& a, const Identifier& b) noexcept {
        return a.text_ == b.text_;
    }
    friend bool operator!=(const Identifier& a, const Identifier& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const Identifier& a, const Identifier& b) noexcept {
        return a.text_ < b.text_;
    }

private:
    std::string text_{};
};

template <class Tag>
class Counter {
public:
    using value_type = std::uint64_t;

    Counter() noexcept = default;
    explicit constexpr Counter(std::uint64_t raw) noexcept : value_(raw) {}

    static constexpr Counter from_raw(std::uint64_t raw) noexcept { return Counter{raw}; }

    // 1-based counters are the norm; zero means "unset" for generations/epochs.
    constexpr std::uint64_t value() const noexcept { return value_; }
    constexpr bool is_zero() const noexcept { return value_ == 0; }

    Result<Counter> next() const {
        if (value_ == UINT64_MAX) {
            return Reason{ReasonCode::ArithmeticOverflow, "counter exhausted at UINT64_MAX"};
        }
        return Counter{value_ + 1U};
    }

    friend constexpr bool operator==(Counter a, Counter b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(Counter a, Counter b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(Counter a, Counter b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(Counter a, Counter b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator<=(Counter a, Counter b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>=(Counter a, Counter b) noexcept { return a.value_ >= b.value_; }

private:
    std::uint64_t value_{0};
};

}  // namespace fel

namespace std {

template <class Tag>
struct hash<fel::Identifier<Tag>> {
    std::size_t operator()(const fel::Identifier<Tag>& value) const noexcept {
        return std::hash<std::string_view>{}(value.view());
    }
};

template <class Tag>
struct hash<fel::Counter<Tag>> {
    std::size_t operator()(fel::Counter<Tag> value) const noexcept {
        return std::hash<std::uint64_t>{}(value.value());
    }
};

}  // namespace std
