#include "fel/report.hpp"

#include <string>
#include <utility>

#include "fel/ledger.hpp"

namespace fel {
namespace {

void append_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (c < 0x20U) {
                    static const char* kHex = "0123456789abcdef";
                    out.append("\\u00");
                    out.push_back(kHex[(c >> 4) & 0x0FU]);
                    out.push_back(kHex[c & 0x0FU]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

}  // namespace

Json Json::null() { return Json{}; }

Json Json::boolean(bool value) {
    Json json;
    json.kind_ = Kind::Boolean;
    json.boolean_ = value;
    return json;
}

Json Json::number(std::string literal) {
    Json json;
    json.kind_ = Kind::Number;
    json.text_ = std::move(literal);
    return json;
}

Json Json::integer(std::int64_t value) { return Json::number(std::to_string(value)); }

Json Json::uinteger(std::uint64_t value) { return Json::number(std::to_string(value)); }

Json Json::exact(const Rational& value) { return Json::text(value.to_string()); }

Json Json::text(std::string value) {
    Json json;
    json.kind_ = Kind::Text;
    json.text_ = std::move(value);
    return json;
}

Json Json::array() {
    Json json;
    json.kind_ = Kind::Array;
    return json;
}

Json Json::object() {
    Json json;
    json.kind_ = Kind::Object;
    return json;
}

Json& Json::set(std::string key, Json value) {
    if (kind_ != Kind::Object) {
        kind_ = Kind::Object;
        items_.clear();
    }
    members_[std::move(key)] = std::move(value);
    return *this;
}

Json& Json::push(Json value) {
    if (kind_ != Kind::Array) {
        kind_ = Kind::Array;
        members_.clear();
    }
    items_.push_back(std::move(value));
    return *this;
}

void Json::dump_into(std::string& out, int depth) const {
    switch (kind_) {
        case Kind::Null:
            out.append("null");
            break;
        case Kind::Boolean:
            out.append(boolean_ ? "true" : "false");
            break;
        case Kind::Number:
            out.append(text_);
            break;
        case Kind::Text:
            append_escaped(out, text_);
            break;
        case Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const Json& item : items_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                item.dump_into(out, depth + 1);
            }
            out.push_back(']');
            break;
        }
        case Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& member : members_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                append_escaped(out, member.first);
                out.push_back(':');
                member.second.dump_into(out, depth + 1);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_into(out, 0);
    return out;
}

Json quantity_json(const Quantity& value) {
    Json json = Json::object();
    json.set("amount", Json::exact(value.amount()));
    json.set("unit", Json::text(std::string(unit_symbol(value.unit()))));
    return json;
}

Json interval_json(const Interval& value) {
    Json json = Json::object();
    json.set("end", Json::text(value.end().to_iso8601()));
    json.set("start", Json::text(value.start().to_iso8601()));
    return json;
}

namespace {

Json report_json(const ReconciliationReport& report, bool include_session_fields) {
    Json root = Json::object();
    root.set("basis", Json::text(std::string(to_string(report.basis))));
    if (include_session_fields) {
        root.set("chain_head", Json::text(report.chain_head.to_hex()));
        root.set("epoch", Json::uinteger(report.epoch.value()));
        root.set("generated_at", Json::text(report.generated_at.to_iso8601()));
    }
    root.set("changed_since_seal", Json::boolean(report.changed_since_seal));
    root.set("closed", Json::boolean(report.closed));
    root.set("code", Json::text(std::string(to_string(report.code))));
    root.set("conflict_count", Json::uinteger(report.conflict_count));
    root.set("explanation", Json::text(report.explanation));
    root.set("interval", interval_json(report.interval));
    root.set("ledger", Json::text(report.ledger.str()));
    root.set("measurement_count", Json::uinteger(report.measurement_count));
    root.set("allocation_count", Json::uinteger(report.allocation_count));
    root.set("residual_count", Json::uinteger(report.residual_count));
    root.set("revision", Json::uinteger(report.revision.value()));
    root.set("sealed", Json::boolean(report.sealed));
    if (report.sealed) {
        root.set("seal", Json::text(report.seal.str()));
        root.set("sealed_report_digest", Json::text(report.sealed_report_digest.to_hex()));
        root.set("sealed_revision", Json::uinteger(report.sealed_revision.value()));
    }

    Json dimensions = Json::array();
    for (const DimensionClosure& closure : report.dimensions) {
        Json entry = Json::object();
        entry.set("allocation_count", Json::uinteger(closure.allocation_count));
        entry.set("base_unit", Json::text(std::string(unit_symbol(closure.base_unit))));
        entry.set("bound_residual_count", Json::uinteger(closure.bound_residual_count));
        entry.set("classified_avoidable", quantity_json(closure.classified_avoidable));
        entry.set("classified_stranded", quantity_json(closure.classified_stranded));
        entry.set("classified_total", quantity_json(closure.classified_total));
        entry.set("classified_unknown", quantity_json(closure.classified_unknown));
        entry.set("classified_unmeasured", quantity_json(closure.classified_unmeasured));
        entry.set("classified_useful", quantity_json(closure.classified_useful));
        entry.set("classified_wasted", quantity_json(closure.classified_wasted));
        entry.set("closed", Json::boolean(closure.closed));
        entry.set("closure_residual", quantity_json(closure.closure_residual));
        entry.set("code", Json::text(std::string(to_string(closure.code))));
        entry.set("declared_input", quantity_json(closure.declared_input));
        entry.set("declared_residual_count", Json::uinteger(closure.declared_residual_count));
        entry.set("dimension", Json::text(std::string(dimension_name(closure.dimension))));
        entry.set("explanation", Json::text(closure.explanation));
        entry.set("input_total", quantity_json(closure.input_total));
        entry.set("measured_input", quantity_json(closure.measured_input));
        entry.set("measurement_count", Json::uinteger(closure.measurement_count));
        entry.set("unclassified", quantity_json(closure.unclassified));
        entry.set("unquantified_residual_count", Json::uinteger(closure.unquantified_residual_count));
        Json evidence = Json::array();
        for (const std::string& digest : closure.evidence) {
            evidence.push(Json::text(digest));
        }
        entry.set("evidence", std::move(evidence));
        Json issues = Json::array();
        for (const ClosureIssue& issue : closure.issues) {
            Json item = Json::object();
            item.set("code", Json::text(std::string(to_string(issue.code))));
            item.set("detail", Json::text(issue.detail));
            item.set("subject", Json::text(issue.subject));
            issues.push(std::move(item));
        }
        entry.set("issues", std::move(issues));
        dimensions.push(std::move(entry));
    }
    root.set("dimensions", std::move(dimensions));

    Json dependencies = Json::array();
    for (const FreshnessDependency& dependency : report.dependencies) {
        Json entry = Json::object();
        entry.set("detail", Json::text(dependency.detail));
        entry.set("evidence", Json::text(dependency.evidence.to_hex()));
        entry.set("generation", Json::uinteger(dependency.generation.value()));
        entry.set("measurement_count", Json::uinteger(dependency.measurement_count));
        entry.set("source", Json::text(dependency.source.str()));
        entry.set("state", Json::text(std::string(to_string(dependency.state))));
        entry.set("valid_until", Json::text(dependency.valid_until.to_iso8601()));
        dependencies.push(std::move(entry));
    }
    root.set("dependencies", std::move(dependencies));

    Json issues = Json::array();
    for (const ClosureIssue& issue : report.issues) {
        Json item = Json::object();
        item.set("code", Json::text(std::string(to_string(issue.code))));
        item.set("detail", Json::text(issue.detail));
        item.set("record_digest", Json::text(issue.record_digest.to_hex()));
        item.set("subject", Json::text(issue.subject));
        issues.push(std::move(item));
    }
    root.set("issues", std::move(issues));
    return root;
}

}  // namespace

std::string render_report_body(const ReconciliationReport& report) {
    return report_json(report, false).dump();
}

std::string render_json(const ReconciliationReport& report) {
    Json root = report_json(report, true);
    root.set("report_digest", Json::text(report.report_digest.to_hex()));
    return root.dump();
}

std::string render_json(const CommandOutcome& outcome) {
    Json root = Json::object();
    root.set("applied", Json::boolean(outcome.applied));
    root.set("chain_head", Json::text(outcome.chain_head.to_hex()));
    root.set("code", Json::text(std::string(to_string(outcome.code))));
    root.set("detail", Json::text(outcome.detail));
    root.set("epoch", Json::uinteger(outcome.epoch.value()));
    root.set("outcome_id", Json::text(outcome.outcome_id));
    root.set("record_digest", Json::text(outcome.record_digest.to_hex()));
    root.set("record_type", Json::text(std::string(to_string(outcome.record_type))));
    root.set("replayed", Json::boolean(outcome.replayed));
    root.set("revision", Json::uinteger(outcome.revision.value()));
    root.set("sequence", Json::uinteger(outcome.sequence.value()));
    return root.dump();
}

}  // namespace fel
