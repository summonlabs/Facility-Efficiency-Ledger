#pragma once

// Deterministic JSON rendering.
//
// Every public result can be rendered as canonical JSON: object keys are sorted,
// numbers are printed from exact rationals, and no floating point value ever
// appears. Two logically identical results therefore render byte for byte
// identically, which is what makes a published report digest meaningful.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "fel/rational.hpp"
#include "fel/reconcile.hpp"

namespace fel {

struct CommandOutcome;

class Json {
public:
    enum class Kind : std::uint8_t { Null, Boolean, Number, Text, Array, Object };

    Json() = default;

    static Json null();
    static Json boolean(bool value);
    static Json number(std::string literal);
    static Json integer(std::int64_t value);
    static Json uinteger(std::uint64_t value);
    static Json exact(const Rational& value);
    static Json text(std::string value);
    static Json array();
    static Json object();

    Json& set(std::string key, Json value);
    Json& push(Json value);

    Kind kind() const noexcept { return kind_; }
    bool is_object() const noexcept { return kind_ == Kind::Object; }
    bool is_array() const noexcept { return kind_ == Kind::Array; }
    const std::map<std::string, Json>& members() const noexcept { return members_; }

    std::string dump() const;

private:
    void dump_into(std::string& out, int depth) const;

    Kind kind_{Kind::Null};
    bool boolean_{false};
    std::string text_{};
    std::vector<Json> items_{};
    std::map<std::string, Json> members_{};
};

Json quantity_json(const Quantity& value);
Json interval_json(const Interval& value);

// Canonical report body without the digest field, used to compute the digest.
//
// The body deliberately excludes session bookkeeping (chain head and epoch) and
// the wall-clock generation instant: those change when the ledger is reopened or
// re-stamped, while the accounting result does not. Including them would make a
// seal taken before a restart impossible to re-verify afterwards.
std::string render_report_body(const ReconciliationReport& report);
std::string render_json(const ReconciliationReport& report);
std::string render_json(const CommandOutcome& outcome);

}  // namespace fel
