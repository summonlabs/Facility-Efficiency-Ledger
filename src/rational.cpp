#include "fel/rational.hpp"

#include <array>
#include <charconv>
#include <numeric>
#include <string>

namespace fel {
namespace {

struct U128 {
    std::uint64_t hi{0};
    std::uint64_t lo{0};
};

constexpr bool u128_is_zero(U128 v) noexcept { return v.hi == 0 && v.lo == 0; }
constexpr bool u128_fits_u64(U128 v) noexcept { return v.hi == 0; }

constexpr int u128_cmp(U128 a, U128 b) noexcept {
    if (a.hi != b.hi) {
        return a.hi < b.hi ? -1 : 1;
    }
    if (a.lo != b.lo) {
        return a.lo < b.lo ? -1 : 1;
    }
    return 0;
}

U128 u128_mul(std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
    const std::uint64_t a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
    const std::uint64_t b_hi = b >> 32;

    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;

    const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    return U128{p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32), (mid << 32) | (p0 & 0xFFFFFFFFULL)};
}

U128 u128_add(U128 a, U128 b) noexcept {
    const std::uint64_t lo = a.lo + b.lo;
    const std::uint64_t carry = (lo < a.lo) ? 1ULL : 0ULL;
    return U128{a.hi + b.hi + carry, lo};
}

U128 u128_sub(U128 a, U128 b) noexcept {
    const std::uint64_t borrow = (a.lo < b.lo) ? 1ULL : 0ULL;
    return U128{a.hi - b.hi - borrow, a.lo - b.lo};
}

constexpr std::uint64_t u128_bit(U128 v, int index) noexcept {
    return (index >= 64) ? ((v.hi >> (index - 64)) & 1ULL) : ((v.lo >> index) & 1ULL);
}

constexpr void u128_set_bit(U128& v, int index) noexcept {
    if (index >= 64) {
        v.hi |= (1ULL << (index - 64));
    } else {
        v.lo |= (1ULL << index);
    }
}

// Long division by shift and subtract; 128 iterations of constant work.
void u128_divmod(U128 numerator, U128 divisor, U128& quotient, U128& remainder) noexcept {
    quotient = U128{};
    remainder = U128{};
    for (int i = 127; i >= 0; --i) {
        remainder.hi = (remainder.hi << 1) | (remainder.lo >> 63);
        remainder.lo = (remainder.lo << 1) | u128_bit(numerator, i);
        if (u128_cmp(remainder, divisor) >= 0) {
            remainder = u128_sub(remainder, divisor);
            u128_set_bit(quotient, i);
        }
    }
}

U128 u128_gcd(U128 a, U128 b) noexcept {
    while (!u128_is_zero(b)) {
        U128 quotient{};
        U128 remainder{};
        u128_divmod(a, b, quotient, remainder);
        a = b;
        b = remainder;
    }
    return a;
}

constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
    return value < 0 ? (0ULL - static_cast<std::uint64_t>(value))
                     : static_cast<std::uint64_t>(value);
}

bool from_magnitude(U128 value, bool negative, std::int64_t& out) noexcept {
    if (!u128_fits_u64(value)) {
        return false;
    }
    const std::uint64_t raw = value.lo;
    if (negative) {
        if (raw > 0x8000000000000000ULL) {
            return false;
        }
        if (raw == 0x8000000000000000ULL) {
            out = INT64_MIN;
            return true;
        }
        out = -static_cast<std::int64_t>(raw);
        return true;
    }
    if (raw > 0x7FFFFFFFFFFFFFFFULL) {
        return false;
    }
    out = static_cast<std::int64_t>(raw);
    return true;
}

// Reduce numerator/denominator by their gcd and materialize the signed result.
std::optional<Rational> normalize(U128 numerator_magnitude, bool negative,
                                  U128 denominator_magnitude) noexcept {
    if (u128_is_zero(denominator_magnitude)) {
        return std::nullopt;
    }
    if (u128_is_zero(numerator_magnitude)) {
        return Rational{0};
    }
    U128 divisor{};
    if (u128_fits_u64(numerator_magnitude) && u128_fits_u64(denominator_magnitude)) {
        divisor = U128{0, std::gcd(numerator_magnitude.lo, denominator_magnitude.lo)};
    } else {
        divisor = u128_gcd(numerator_magnitude, denominator_magnitude);
    }
    U128 reduced_num{};
    U128 reduced_den{};
    U128 remainder_num{};
    U128 remainder_den{};
    u128_divmod(numerator_magnitude, divisor, reduced_num, remainder_num);
    u128_divmod(denominator_magnitude, divisor, reduced_den, remainder_den);
    if (!u128_is_zero(remainder_num) || !u128_is_zero(remainder_den)) {
        return std::nullopt;
    }
    std::int64_t num = 0;
    std::int64_t den = 0;
    if (!from_magnitude(reduced_num, negative, num)) {
        return std::nullopt;
    }
    if (!from_magnitude(reduced_den, false, den)) {
        return std::nullopt;
    }
    return make_normalized(num, den);
}

}  // namespace

std::optional<Rational> make_normalized(std::int64_t numerator, std::int64_t denominator) noexcept {
    if (denominator <= 0) {
        return std::nullopt;
    }
    if (numerator == 0) {
        return Rational{0};
    }
    if (std::gcd(magnitude(numerator), static_cast<std::uint64_t>(denominator)) != 1ULL) {
        return std::nullopt;
    }
    return Rational{numerator, denominator, 0};
}

std::optional<Rational> Rational::make(std::int64_t numerator,
                                       std::int64_t denominator) noexcept {
    if (denominator == 0) {
        return std::nullopt;
    }
    return normalize(U128{0, magnitude(numerator)}, (numerator < 0) != (denominator < 0),
                     U128{0, magnitude(denominator)});
}

std::optional<Rational> Rational::parse(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64) {
        return std::nullopt;
    }
    const std::size_t slash = text.find('/');
    const std::string_view num_text = slash == std::string_view::npos ? text : text.substr(0, slash);
    const std::string_view den_text =
        slash == std::string_view::npos ? std::string_view{"1"} : text.substr(slash + 1);

    const auto parse_i64 = [](std::string_view piece, std::int64_t& out) noexcept {
        if (piece.empty()) {
            return false;
        }
        std::size_t index = 0;
        bool negative = false;
        if (piece.front() == '-') {
            negative = true;
            index = 1;
        } else if (piece.front() == '+') {
            return false;
        }
        if (index >= piece.size()) {
            return false;
        }
        for (std::size_t i = index; i < piece.size(); ++i) {
            if (piece[i] < '0' || piece[i] > '9') {
                return false;
            }
        }
        std::int64_t value = 0;
        const char* first = piece.data() + index;
        const char* last = piece.data() + piece.size();
        const auto result = std::from_chars(first, last, value, 10);
        if (result.ec != std::errc{} || result.ptr != last) {
            return false;
        }
        out = negative ? -value : value;
        return true;
    };

    std::int64_t numerator = 0;
    std::int64_t denominator = 0;
    if (!parse_i64(num_text, numerator) || !parse_i64(den_text, denominator)) {
        return std::nullopt;
    }
    return Rational::make(numerator, denominator);
}

Result<Rational> Rational::add(const Rational& other) const noexcept {
    if (is_zero()) {
        return other;
    }
    if (other.is_zero()) {
        return *this;
    }
    const bool left_negative = num_ < 0;
    const bool right_negative = other.num_ < 0;
    const U128 left = u128_mul(magnitude(num_), magnitude(other.den_));
    const U128 right = u128_mul(magnitude(other.num_), magnitude(den_));
    U128 sum{};
    bool result_negative = false;
    if (left_negative == right_negative) {
        sum = u128_add(left, right);
        result_negative = left_negative;
    } else {
        const int order = u128_cmp(left, right);
        if (order == 0) {
            return Rational{0};
        }
        if (order > 0) {
            sum = u128_sub(left, right);
            result_negative = left_negative;
        } else {
            sum = u128_sub(right, left);
            result_negative = right_negative;
        }
    }
    const U128 denominator = u128_mul(magnitude(den_), magnitude(other.den_));
    const auto normalized = normalize(sum, result_negative, denominator);
    if (!normalized.has_value()) {
        return Reason{ReasonCode::ArithmeticOverflow, "rational addition is not representable"};
    }
    return *normalized;
}

Result<Rational> Rational::subtract(const Rational& other) const noexcept {
    const Result<Rational> negated = other.negate();
    if (!negated.ok()) {
        return negated.reason();
    }
    return add(negated.value());
}

Result<Rational> Rational::multiply(const Rational& other) const noexcept {
    if (is_zero() || other.is_zero()) {
        return Rational{0};
    }
    const bool negative = (num_ < 0) != (other.num_ < 0);
    const U128 numerator = u128_mul(magnitude(num_), magnitude(other.num_));
    const U128 denominator = u128_mul(magnitude(den_), magnitude(other.den_));
    const auto normalized = normalize(numerator, negative, denominator);
    if (!normalized.has_value()) {
        return Reason{ReasonCode::ArithmeticOverflow,
                      "rational multiplication is not representable"};
    }
    return *normalized;
}

Result<Rational> Rational::divide(const Rational& other) const noexcept {
    if (other.is_zero()) {
        return Reason{ReasonCode::DivisionByZero, "rational division by zero"};
    }
    const bool negative = (num_ < 0) != (other.num_ < 0);
    const U128 numerator = u128_mul(magnitude(num_), magnitude(other.den_));
    const U128 denominator = u128_mul(magnitude(den_), magnitude(other.num_));
    const auto normalized = normalize(numerator, negative, denominator);
    if (!normalized.has_value()) {
        return Reason{ReasonCode::ArithmeticOverflow, "rational division is not representable"};
    }
    return *normalized;
}

Result<Rational> Rational::negate() const noexcept {
    if (num_ == INT64_MIN) {
        return Reason{ReasonCode::ArithmeticOverflow, "negation of INT64_MIN is not representable"};
    }
    if (num_ == 0) {
        return Rational{0};
    }
    return Rational{-num_, den_, 0};
}

int Rational::compare(const Rational& other) const noexcept {
    if (same_representation(other)) {
        return 0;
    }
    const int left_sign = sign();
    const int right_sign = other.sign();
    if (left_sign != right_sign) {
        return left_sign < right_sign ? -1 : 1;
    }
    if (left_sign == 0) {
        return 0;
    }
    const U128 left = u128_mul(magnitude(num_), magnitude(other.den_));
    const U128 right = u128_mul(magnitude(other.num_), magnitude(den_));
    const int magnitude_order = u128_cmp(left, right);
    return left_sign < 0 ? -magnitude_order : magnitude_order;
}

std::string Rational::to_string() const {
    std::string out = std::to_string(num_);
    if (den_ != 1) {
        out.push_back('/');
        out.append(std::to_string(den_));
    }
    return out;
}

}  // namespace fel
