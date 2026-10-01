#pragma once

// A quantity is an exact amount paired with a unit. Addition and subtraction
// require identical units; a caller that wants to combine kWh with J must ask
// for the conversion explicitly, so a unit mismatch is a reported refusal
// rather than a silent assumption.

#include <string>

#include "fel/rational.hpp"
#include "fel/status.hpp"
#include "fel/unit.hpp"

namespace fel {

class Quantity {
public:
    Quantity() noexcept = default;
    Quantity(Rational amount, Unit unit) noexcept : amount_(amount), unit_(unit) {}

    static Result<Quantity> make(Rational amount, Unit unit);

    const Rational& amount() const noexcept { return amount_; }
    Unit unit() const noexcept { return unit_; }
    Dimension dimension() const noexcept { return dimension_of(unit_); }

    bool is_zero() const noexcept { return amount_.is_zero(); }
    bool is_negative() const noexcept { return amount_.is_negative(); }

    Result<Quantity> add(const Quantity& other) const;
    Result<Quantity> subtract(const Quantity& other) const;
    Result<Quantity> negate() const;

    // Exact conversion. Refuses across dimensions.
    Result<Quantity> convert_to(Unit target) const;

    // Both operands must already share a unit.
    Result<int> compare(const Quantity& other) const;

    std::string to_string() const;

private:
    Rational amount_{};
    Unit unit_{Unit::Each};
};

inline bool operator==(const Quantity& a, const Quantity& b) noexcept {
    return a.unit() == b.unit() && a.amount() == b.amount();
}
inline bool operator!=(const Quantity& a, const Quantity& b) noexcept { return !(a == b); }

}  // namespace fel
