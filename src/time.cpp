#include "fel/time.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace fel {
namespace {

constexpr std::int64_t kMaxSeconds = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMinSeconds = std::numeric_limits<std::int64_t>::min();

}  // namespace

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);            // [0, 399]
    const unsigned shifted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned doy = (153U * shifted_month + 2U) / 5U + day - 1U;        // [0, 365]
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;           // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month,
                     unsigned& day) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);          // [0, 146096]
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);          // [0, 365]
    const unsigned mp = (5U * doy + 2U) / 153U;                               // [0, 11]
    day = doy - (153U * mp + 2U) / 5U + 1U;                                   // [1, 31]
    month = mp < 10U ? mp + 3U : mp - 9U;                                     // [1, 12]
    year = y + (month <= 2 ? 1 : 0);
}

Result<Instant> Instant::from_unix_nanos(std::int64_t total_nanos) {
    std::int64_t seconds = total_nanos / kNanosPerSecond;
    std::int64_t nanos = total_nanos % kNanosPerSecond;
    if (nanos < 0) {
        nanos += kNanosPerSecond;
        if (seconds == kMinSeconds) {
            return Reason{ReasonCode::ArithmeticOverflow, "instant nanos below representable range"};
        }
        seconds -= 1;
    }
    return Instant{seconds, static_cast<std::int32_t>(nanos)};
}

Result<Instant> Instant::add_nanos(std::int64_t delta) const {
    std::int64_t seconds_delta = delta / kNanosPerSecond;
    const std::int64_t nanos_delta = delta % kNanosPerSecond;
    std::int64_t nanos = static_cast<std::int64_t>(nanos_) + nanos_delta;
    if (nanos >= kNanosPerSecond) {
        nanos -= kNanosPerSecond;
        seconds_delta += 1;
    } else if (nanos < 0) {
        nanos += kNanosPerSecond;
        seconds_delta -= 1;
    }
    const Result<Instant> shifted = add_seconds(seconds_delta);
    if (!shifted.ok()) {
        return shifted.reason();
    }
    return Instant{shifted.value().seconds(), static_cast<std::int32_t>(nanos)};
}

Result<Instant> Instant::add_seconds(std::int64_t delta) const {
    if ((delta > 0 && seconds_ > kMaxSeconds - delta) ||
        (delta < 0 && seconds_ < kMinSeconds - delta)) {
        return Reason{ReasonCode::ArithmeticOverflow,
                      "instant addition exceeds the representable range"};
    }
    return Instant{seconds_ + delta, nanos_};
}

int Instant::compare(const Instant& other) const noexcept {
    if (seconds_ != other.seconds_) {
        return seconds_ < other.seconds_ ? -1 : 1;
    }
    if (nanos_ != other.nanos_) {
        return nanos_ < other.nanos_ ? -1 : 1;
    }
    return 0;
}

std::string Instant::to_iso8601() const {
    const std::int64_t days = seconds_ / kSecondsPerDay;
    std::int64_t remainder = seconds_ % kSecondsPerDay;
    if (remainder < 0) {
        remainder += kSecondsPerDay;
    }
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);

    const unsigned hour = static_cast<unsigned>(remainder / 3600);
    const unsigned minute = static_cast<unsigned>((remainder % 3600) / 60);
    const unsigned second = static_cast<unsigned>(remainder % 60);

    std::array<char, 64> buffer{};
    int written = 0;
    if (nanos_ == 0) {
        written = std::snprintf(buffer.data(), buffer.size(),
                                "%04lld-%02u-%02uT%02u:%02u:%02uZ",
                                static_cast<long long>(year), month, day, hour, minute, second);
    } else {
        written = std::snprintf(buffer.data(), buffer.size(),
                                "%04lld-%02u-%02uT%02u:%02u:%02u.%09dZ",
                                static_cast<long long>(year), month, day, hour, minute, second,
                                static_cast<int>(nanos_));
    }
    if (written <= 0) {
        return "0000-00-00T00:00:00Z";
    }
    return std::string{buffer.data(), static_cast<std::size_t>(written)};
}

Result<Instant> Instant::parse_iso8601(std::string_view text) {
    // Strict shape checks keep parsing deterministic and reject ambiguous input.
    const auto digit = [&](std::size_t index) -> int {
        const char c = text[index];
        return (c >= '0' && c <= '9') ? (c - '0') : -1;
    };
    if (text.size() < 20) {
        return Reason{ReasonCode::InvalidArgument, "timestamp is shorter than YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return Reason{ReasonCode::InvalidArgument,
                      "timestamp must be YYYY-MM-DDTHH:MM:SS[.fffffffff]Z"};
    }
    const std::array<std::size_t, 14> digit_positions{0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    std::array<int, 14> digits{};
    for (std::size_t i = 0; i < digit_positions.size(); ++i) {
        digits[i] = digit(digit_positions[i]);
        if (digits[i] < 0) {
            return Reason{ReasonCode::InvalidArgument, "timestamp contains a non-digit field"};
        }
    }
    const std::int64_t year = digits[0] * 1000LL + digits[1] * 100LL + digits[2] * 10LL + digits[3];
    const unsigned month = static_cast<unsigned>(digits[4] * 10 + digits[5]);
    const unsigned day = static_cast<unsigned>(digits[6] * 10 + digits[7]);
    const unsigned hour = static_cast<unsigned>(digits[8] * 10 + digits[9]);
    const unsigned minute = static_cast<unsigned>(digits[10] * 10 + digits[11]);
    const unsigned second = static_cast<unsigned>(digits[12] * 10 + digits[13]);

    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return Reason{ReasonCode::InvalidArgument, "timestamp field is out of range"};
    }

    std::size_t cursor = 19;
    std::int32_t nanos = 0;
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        std::size_t digits_read = 0;
        while (cursor < text.size() && digits_read < 9 && text[cursor] >= '0' &&
               text[cursor] <= '9') {
            nanos = static_cast<std::int32_t>(nanos * 10 + (text[cursor] - '0'));
            ++cursor;
            ++digits_read;
        }
        if (digits_read == 0) {
            return Reason{ReasonCode::InvalidArgument, "fractional seconds are empty"};
        }
        while (digits_read < 9) {
            nanos = static_cast<std::int32_t>(nanos * 10);
            ++digits_read;
        }
    }
    if (cursor >= text.size() || text[cursor] != 'Z') {
        return Reason{ReasonCode::InvalidArgument, "timestamp must end with 'Z' (UTC)"};
    }
    if (cursor + 1 != text.size()) {
        return Reason{ReasonCode::InvalidArgument, "trailing characters after timestamp"};
    }

    const std::int64_t days = days_from_civil(year, month, day);
    const std::int64_t seconds =
        days * kSecondsPerDay + static_cast<std::int64_t>(hour) * 3600 +
        static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    return Instant{seconds, nanos};
}

Result<Interval> Interval::make(Instant start, Instant end) {
    const int order = start.compare(end);
    if (order == 0) {
        return Reason{ReasonCode::EmptyInterval, "interval start equals interval end"};
    }
    if (order > 0) {
        return Reason{ReasonCode::InvalidInterval, "interval start is after interval end"};
    }
    Interval interval;
    interval.start_ = start;
    interval.end_ = end;
    return interval;
}

bool Interval::contains(const Instant& instant) const noexcept {
    return start_.compare(instant) <= 0 && instant.compare(end_) < 0;
}

bool Interval::contains(const Interval& other) const noexcept {
    return start_.compare(other.start_) <= 0 && other.end_.compare(end_) <= 0;
}

bool Interval::overlaps(const Interval& other) const noexcept {
    return start_.compare(other.end_) < 0 && other.start_.compare(end_) < 0;
}

Result<Rational> Interval::duration_seconds() const {
    const std::int64_t end_seconds = end_.seconds();
    const std::int64_t start_seconds = start_.seconds();
    if ((end_seconds > 0 && start_seconds < 0 && end_seconds > kMaxSeconds + start_seconds) ||
        (end_seconds < 0 && start_seconds > 0 && end_seconds < kMinSeconds + start_seconds)) {
        return Reason{ReasonCode::ArithmeticOverflow, "interval duration exceeds the representable range"};
    }
    const std::int64_t seconds = end_seconds - start_seconds;
    const std::int64_t nanos = static_cast<std::int64_t>(end_.nanos()) - start_.nanos();
    const auto whole = Rational::make(seconds, 1);
    const auto fraction = Rational::make(nanos, kNanosPerSecond);
    if (!whole.has_value() || !fraction.has_value()) {
        return Reason{ReasonCode::ArithmeticOverflow, "interval duration is not representable"};
    }
    return whole->add(*fraction);
}

std::string Interval::to_string() const {
    return "[" + start_.to_iso8601() + ", " + end_.to_iso8601() + ")";
}

}  // namespace fel
