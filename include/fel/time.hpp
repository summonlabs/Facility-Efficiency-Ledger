#pragma once

// Discrete, exact time points and bounded half-open intervals.
//
// Time is an exact integer count of seconds plus nanoseconds since the Unix
// epoch in UTC. There is no floating point anywhere in the time model, so an
// interval boundary is reproducible byte for byte across runs and machines.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fel/rational.hpp"
#include "fel/status.hpp"

namespace fel {

inline constexpr std::int64_t kNanosPerSecond = 1000000000LL;
inline constexpr std::int64_t kSecondsPerDay = 86400LL;

class Instant {
public:
    Instant() noexcept = default;
    constexpr Instant(std::int64_t seconds, std::int32_t nanos) noexcept
        : seconds_(seconds), nanos_(nanos) {}

    static constexpr Instant from_unix_seconds(std::int64_t seconds) noexcept {
        return Instant{seconds, 0};
    }

    // Adds nanoseconds with overflow checking.
    static Result<Instant> from_unix_nanos(std::int64_t total_nanos);

    constexpr std::int64_t seconds() const noexcept { return seconds_; }
    constexpr std::int32_t nanos() const noexcept { return nanos_; }

    Result<Instant> add_nanos(std::int64_t delta) const;
    Result<Instant> add_seconds(std::int64_t delta) const;

    int compare(const Instant& other) const noexcept;
    bool is_before(const Instant& other) const noexcept { return compare(other) < 0; }

    // Strict "YYYY-MM-DDTHH:MM:SS[.fffffffff]Z" in UTC.
    std::string to_iso8601() const;
    static Result<Instant> parse_iso8601(std::string_view text);

    friend bool operator==(const Instant& a, const Instant& b) noexcept { return a.compare(b) == 0; }
    friend bool operator!=(const Instant& a, const Instant& b) noexcept { return a.compare(b) != 0; }
    friend bool operator<(const Instant& a, const Instant& b) noexcept { return a.compare(b) < 0; }
    friend bool operator>(const Instant& a, const Instant& b) noexcept { return a.compare(b) > 0; }
    friend bool operator<=(const Instant& a, const Instant& b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>=(const Instant& a, const Instant& b) noexcept { return a.compare(b) >= 0; }

private:
    std::int64_t seconds_{0};
    std::int32_t nanos_{0};
};

// Half-open interval [start, end). An interval is only constructible when
// start < end, so zero-length and inverted intervals are explicit refusals.
class Interval {
public:
    Interval() noexcept = default;

    static Result<Interval> make(Instant start, Instant end);

    constexpr Instant start() const noexcept { return start_; }
    constexpr Instant end() const noexcept { return end_; }

    bool contains(const Instant& instant) const noexcept;
    bool contains(const Interval& other) const noexcept;
    bool overlaps(const Interval& other) const noexcept;

    // Exact duration in seconds (nanoseconds expressed as a denominator).
    Result<Rational> duration_seconds() const;

    std::string to_string() const;

    friend bool operator==(const Interval& a, const Interval& b) noexcept {
        return a.start_ == b.start_ && a.end_ == b.end_;
    }
    friend bool operator!=(const Interval& a, const Interval& b) noexcept { return !(a == b); }

private:
    Instant start_{};
    Instant end_{};
};

// Civil calendar helpers shared by the parsers and formatters.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept;
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept;

}  // namespace fel
