#pragma once

// Physical dimensions and units.
//
// Units are compared and converted exactly. Two unit systems that are easy to
// confuse are kept distinct on purpose: decimal (kB = 1000 B) and binary
// (KiB = 1024 B). A quantity never changes dimension silently.

#include <cstdint>
#include <optional>
#include <string_view>

#include "fel/rational.hpp"
#include "fel/status.hpp"

namespace fel {

enum class Dimension : std::uint8_t {
    Count = 0,   // dimensionless equipment/service counts
    Energy,
    Power,
    Volume,
    Mass,
    Data,
    DataTime,    // data held over time, e.g. byte-hours of storage service
    Time,
};

enum class Unit : std::uint8_t {
    Each = 0,

    Joule,
    Kilojoule,
    KilowattHour,
    MegawattHour,

    Watt,
    Kilowatt,
    Megawatt,

    Liter,
    CubicMeter,

    Kilogram,

    Byte,
    Kilobyte,
    Megabyte,
    Gigabyte,
    Terabyte,
    Kibibyte,
    Mebibyte,
    Gibibyte,
    Tebibyte,

    ByteHour,
    GigabyteHour,
    GibibyteHour,

    Second,
    Minute,
    Hour,
    Day,
};

std::string_view unit_symbol(Unit unit) noexcept;
std::optional<Unit> unit_from_symbol(std::string_view symbol) noexcept;

std::string_view dimension_name(Dimension dimension) noexcept;
std::optional<Dimension> dimension_from_name(std::string_view name) noexcept;

Dimension dimension_of(Unit unit) noexcept;

// Exact multiplicative factor that converts one unit into another of the same
// dimension: value_in_target = value_in_source * factor.
Result<Rational> conversion_factor(Unit source, Unit target);

// Stable integer identity used in the durable record encoding.
std::uint8_t unit_code(Unit unit) noexcept;
std::optional<Unit> unit_from_code(std::uint8_t code) noexcept;

}  // namespace fel
