#include "fel/quantity.hpp"

namespace fel {

Result<Quantity> Quantity::make(Rational amount, Unit unit) {
    if (unit_from_code(unit_code(unit)) != unit) {
        return Reason{ReasonCode::UnknownUnit, "quantity constructed with an unregistered unit"};
    }
    return Quantity{amount, unit};
}

Result<Quantity> Quantity::add(const Quantity& other) const {
    if (unit_ != other.unit_) {
        return Reason{ReasonCode::UnitMismatch,
                      std::string("cannot add ") + std::string(unit_symbol(unit_)) + " and " +
                          std::string(unit_symbol(other.unit_)) +
                          " without an explicit conversion"};
    }
    const Result<Rational> sum = amount_.add(other.amount_);
    if (!sum.ok()) {
        return sum.reason();
    }
    return Quantity{sum.value(), unit_};
}

Result<Quantity> Quantity::subtract(const Quantity& other) const {
    if (unit_ != other.unit_) {
        return Reason{ReasonCode::UnitMismatch,
                      std::string("cannot subtract ") + std::string(unit_symbol(other.unit_)) +
                          " from " + std::string(unit_symbol(unit_)) +
                          " without an explicit conversion"};
    }
    const Result<Rational> difference = amount_.subtract(other.amount_);
    if (!difference.ok()) {
        return difference.reason();
    }
    return Quantity{difference.value(), unit_};
}

Result<Quantity> Quantity::negate() const {
    const Result<Rational> negated = amount_.negate();
    if (!negated.ok()) {
        return negated.reason();
    }
    return Quantity{negated.value(), unit_};
}

Result<Quantity> Quantity::convert_to(Unit target) const {
    if (target == unit_) {
        return *this;
    }
    const Result<Rational> factor = conversion_factor(unit_, target);
    if (!factor.ok()) {
        return factor.reason();
    }
    const Result<Rational> converted = amount_.multiply(factor.value());
    if (!converted.ok()) {
        return converted.reason();
    }
    return Quantity{converted.value(), target};
}

Result<int> Quantity::compare(const Quantity& other) const {
    if (unit_ != other.unit_) {
        return Reason{ReasonCode::UnitMismatch,
                      std::string("cannot compare ") + std::string(unit_symbol(unit_)) + " with " +
                          std::string(unit_symbol(other.unit_))};
    }
    return amount_.compare(other.amount_);
}

std::string Quantity::to_string() const {
    std::string out = amount_.to_string();
    out.push_back(' ');
    out.append(unit_symbol(unit_));
    return out;
}

}  // namespace fel
