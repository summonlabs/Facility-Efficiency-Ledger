#pragma once

// Exact rational arithmetic.
//
// Facility accounting must close exactly: a joule of consumption that is
// classified as useful work plus the joules classified elsewhere must equal the
// measured input with no rounding slack. Floating point cannot express that
// guarantee, so every quantity in this runtime is an exact rational with a
// 64-bit numerator and denominator, always stored normalized (denominator > 0,
// gcd(|numerator|, denominator) == 1).
//
// All arithmetic is checked. Intermediate products are computed in 128 bits so
// that operations on representable values do not fail spuriously; a result that
// cannot be represented is reported as ArithmeticOverflow rather than wrapped.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fel/status.hpp"

namespace fel {

class Rational {
public:
    constexpr Rational() noexcept = default;
    explicit constexpr Rational(std::int64_t integer) noexcept : num_(integer) {}

    // nullopt when the denominator is zero or the value cannot be represented.
    static std::optional<Rational> make(std::int64_t numerator, std::int64_t denominator) noexcept;

    // Accepts "n", "-n", "n/d", "-n/d" with decimal digits only.
    static std::optional<Rational> parse(std::string_view text) noexcept;

    constexpr std::int64_t numerator() const noexcept { return num_; }
    constexpr std::int64_t denominator() const noexcept { return den_; }
    constexpr bool is_zero() const noexcept { return num_ == 0; }
    constexpr bool is_integer() const noexcept { return den_ == 1; }
    constexpr bool is_negative() const noexcept { return num_ < 0; }
    constexpr int sign() const noexcept { return (num_ > 0) - (num_ < 0); }

    Result<Rational> add(const Rational& other) const noexcept;
    Result<Rational> subtract(const Rational& other) const noexcept;
    Result<Rational> multiply(const Rational& other) const noexcept;
    Result<Rational> divide(const Rational& other) const noexcept;
    Result<Rational> negate() const noexcept;

    // Total order. Never fails.
    int compare(const Rational& other) const noexcept;
    constexpr bool same_representation(const Rational& other) const noexcept {
        return num_ == other.num_ && den_ == other.den_;
    }

    // Canonical exact text: "n" when the denominator is one, otherwise "n/d".
    std::string to_string() const;

private:
    // Precondition: normalized (gcd == 1, denominator > 0).
    constexpr Rational(std::int64_t normalized_num, std::int64_t normalized_den,
                       int /*tag*/) noexcept
        : num_(normalized_num), den_(normalized_den) {}

    friend std::optional<Rational> make_normalized(std::int64_t, std::int64_t) noexcept;

    std::int64_t num_{0};
    std::int64_t den_{1};
};

// Throws nothing; returns nullopt when gcd is not one or denominator <= 0.
std::optional<Rational> make_normalized(std::int64_t numerator, std::int64_t denominator) noexcept;

inline bool operator==(const Rational& a, const Rational& b) noexcept {
    return a.same_representation(b);
}
inline bool operator!=(const Rational& a, const Rational& b) noexcept {
    return !a.same_representation(b);
}
inline bool operator<(const Rational& a, const Rational& b) noexcept { return a.compare(b) < 0; }
inline bool operator>(const Rational& a, const Rational& b) noexcept { return a.compare(b) > 0; }
inline bool operator<=(const Rational& a, const Rational& b) noexcept { return a.compare(b) <= 0; }
inline bool operator>=(const Rational& a, const Rational& b) noexcept { return a.compare(b) >= 0; }

}  // namespace fel
