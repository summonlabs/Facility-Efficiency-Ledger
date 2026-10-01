#include "fel/unit.hpp"

#include <array>
#include <utility>

namespace fel {
namespace {

struct UnitInfo {
    Unit unit;
    std::string_view symbol;
    Dimension dimension;
    std::int64_t scale_numerator;
    std::int64_t scale_denominator;
};

// Scale is expressed relative to the base unit of the dimension.
constexpr std::array<UnitInfo, 27> kUnits{{
    {Unit::Each, "ea", Dimension::Count, 1, 1},

    {Unit::Joule, "J", Dimension::Energy, 1, 1},
    {Unit::Kilojoule, "kJ", Dimension::Energy, 1000, 1},
    {Unit::KilowattHour, "kWh", Dimension::Energy, 3600000, 1},
    {Unit::MegawattHour, "MWh", Dimension::Energy, 3600000000LL, 1},

    {Unit::Watt, "W", Dimension::Power, 1, 1},
    {Unit::Kilowatt, "kW", Dimension::Power, 1000, 1},
    {Unit::Megawatt, "MW", Dimension::Power, 1000000, 1},

    {Unit::Liter, "L", Dimension::Volume, 1, 1},
    {Unit::CubicMeter, "m3", Dimension::Volume, 1000, 1},

    {Unit::Kilogram, "kg", Dimension::Mass, 1, 1},

    {Unit::Byte, "B", Dimension::Data, 1, 1},
    {Unit::Kilobyte, "kB", Dimension::Data, 1000, 1},
    {Unit::Megabyte, "MB", Dimension::Data, 1000000, 1},
    {Unit::Gigabyte, "GB", Dimension::Data, 1000000000LL, 1},
    {Unit::Terabyte, "TB", Dimension::Data, 1000000000000LL, 1},
    {Unit::Kibibyte, "KiB", Dimension::Data, 1024, 1},
    {Unit::Mebibyte, "MiB", Dimension::Data, 1048576, 1},
    {Unit::Gibibyte, "GiB", Dimension::Data, 1073741824LL, 1},
    {Unit::Tebibyte, "TiB", Dimension::Data, 1099511627776LL, 1},

    {Unit::ByteHour, "B.h", Dimension::DataTime, 1, 1},
    {Unit::GigabyteHour, "GB.h", Dimension::DataTime, 1000000000LL, 1},
    {Unit::GibibyteHour, "GiB.h", Dimension::DataTime, 1073741824LL, 1},

    {Unit::Second, "s", Dimension::Time, 1, 1},
    {Unit::Minute, "min", Dimension::Time, 60, 1},
    {Unit::Hour, "h", Dimension::Time, 3600, 1},
    {Unit::Day, "d", Dimension::Time, 86400, 1},
}};

const UnitInfo* find(Unit unit) noexcept {
    for (const UnitInfo& info : kUnits) {
        if (info.unit == unit) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace

std::string_view unit_symbol(Unit unit) noexcept {
    const UnitInfo* info = find(unit);
    return info != nullptr ? info->symbol : std::string_view{"?"};
}

std::optional<Unit> unit_from_symbol(std::string_view symbol) noexcept {
    for (const UnitInfo& info : kUnits) {
        if (info.symbol == symbol) {
            return info.unit;
        }
    }
    return std::nullopt;
}

std::string_view dimension_name(Dimension dimension) noexcept {
    switch (dimension) {
        case Dimension::Count:
            return "count";
        case Dimension::Energy:
            return "energy";
        case Dimension::Power:
            return "power";
        case Dimension::Volume:
            return "volume";
        case Dimension::Mass:
            return "mass";
        case Dimension::Data:
            return "data";
        case Dimension::DataTime:
            return "data_time";
        case Dimension::Time:
            return "time";
    }
    return "unknown";
}

std::optional<Dimension> dimension_from_name(std::string_view name) noexcept {
    constexpr std::array<std::pair<std::string_view, Dimension>, 8> kNames{{
        {"count", Dimension::Count},
        {"energy", Dimension::Energy},
        {"power", Dimension::Power},
        {"volume", Dimension::Volume},
        {"mass", Dimension::Mass},
        {"data", Dimension::Data},
        {"data_time", Dimension::DataTime},
        {"time", Dimension::Time},
    }};
    for (const auto& entry : kNames) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return std::nullopt;
}

Dimension dimension_of(Unit unit) noexcept {
    const UnitInfo* info = find(unit);
    return info != nullptr ? info->dimension : Dimension::Count;
}

Result<Rational> conversion_factor(Unit source, Unit target) {
    const UnitInfo* from = find(source);
    const UnitInfo* to = find(target);
    if (from == nullptr || to == nullptr) {
        return Reason{ReasonCode::UnknownUnit, "unit is not registered"};
    }
    if (from->dimension != to->dimension) {
        return Reason{ReasonCode::DimensionMismatch,
                      std::string(dimension_name(from->dimension)) + " cannot be converted to " +
                          std::string(dimension_name(to->dimension))};
    }
    const auto numerator = Rational::make(from->scale_numerator, to->scale_numerator);
    const auto denominator = Rational::make(to->scale_denominator, from->scale_denominator);
    if (!numerator.has_value() || !denominator.has_value()) {
        return Reason{ReasonCode::ArithmeticOverflow, "unit conversion factors are not representable"};
    }
    return numerator->multiply(*denominator);
}

std::uint8_t unit_code(Unit unit) noexcept { return static_cast<std::uint8_t>(unit); }

std::optional<Unit> unit_from_code(std::uint8_t code) noexcept {
    for (const UnitInfo& info : kUnits) {
        if (unit_code(info.unit) == code) {
            return info.unit;
        }
    }
    return std::nullopt;
}

}  // namespace fel
