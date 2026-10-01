// Command line interface for the Facility Efficiency Ledger.
//
// Every command prints a deterministic result. With --json the output is
// canonical JSON; without it a compact human line is printed. Exit status is 0
// for a successful, closed outcome, 1 for an explicit refusal, and 2 for a usage
// error. An unclosed reconciliation is a refusal, not a partial success.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "fel/fel.hpp"

namespace {

using namespace fel;

struct Args {
    std::string command;
    std::string subcommand;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    std::set<std::string> flags;
};

const std::set<std::string> kValueOptions{
    "ledger",   "id",         "kind",       "unit",      "authority", "authority-role",
    "label",    "reason",     "generation", "evidence",  "valid-until", "method",
    "entry",    "start",      "end",        "amount",    "subject-kind", "subject",
    "allocation", "class",    "residual",   "basis",     "bound-entry", "supersedes",
    "supersedes-digest", "void-id", "target", "target-kind", "target-digest", "seal",
    "as-of",    "now",        "idempotency", "expect-revision", "expect-epoch", "note",
    "fact",     "role",       "source",
};

bool is_value_option(const std::string& name) { return kValueOptions.count(name) != 0U; }

Result<Args> parse_args(int argc, char** argv) {
    Args args;
    std::vector<std::string> tokens;
    for (int i = 1; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
    }
    if (tokens.empty()) {
        return Reason{ReasonCode::InvalidArgument, "no command was given"};
    }
    args.command = tokens[0];
    std::size_t index = 1;
    if (index < tokens.size() && !tokens[index].empty() && tokens[index][0] != '-') {
        const bool known_subcommand =
            args.command == "source" || args.command == "generation" || args.command == "boundary";
        if (known_subcommand) {
            args.subcommand = tokens[index];
            ++index;
        }
    }
    while (index < tokens.size()) {
        const std::string& token = tokens[index];
        if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
            std::string name = token.substr(2);
            std::string value;
            const std::size_t equals = name.find('=');
            if (equals != std::string::npos) {
                value = name.substr(equals + 1);
                name = name.substr(0, equals);
            }
            if (is_value_option(name)) {
                if (value.empty()) {
                    ++index;
                    if (index >= tokens.size()) {
                        return Reason{ReasonCode::InvalidArgument,
                                      "option --" + name + " requires a value"};
                    }
                    value = tokens[index];
                }
                args.options[name] = value;
            } else {
                args.flags.insert(name);
            }
        } else {
            args.positional.push_back(token);
        }
        ++index;
    }
    return args;
}

bool has_flag(const Args& args, const char* name) { return args.flags.count(name) != 0U; }

std::optional<std::string> option(const Args& args, const char* name) {
    const auto found = args.options.find(name);
    if (found == args.options.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string option_or(const Args& args, const char* name, const std::string& fallback) {
    const auto found = args.options.find(name);
    return found == args.options.end() ? fallback : found->second;
}

Result<Rational> parse_amount(std::string_view text) {
    if (text.empty() || text.size() > 40) {
        return Reason{ReasonCode::MalformedNumber, "amount is empty or too long"};
    }
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) {
        const auto value = Rational::parse(text);
        if (!value.has_value()) {
            return Reason{ReasonCode::MalformedNumber,
                          "amount \"" + std::string(text) + "\" is not an exact decimal"};
        }
        return *value;
    }
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = text.substr(dot + 1);
    if (fraction.empty() || fraction.size() > 18) {
        return Reason{ReasonCode::MalformedNumber, "fractional part is empty or too long"};
    }
    for (char c : fraction) {
        if (c < '0' || c > '9') {
            return Reason{ReasonCode::MalformedNumber, "fractional part is not decimal"};
        }
    }
    std::string combined{whole};
    combined.append(fraction);
    const auto numerator = Rational::parse(combined);
    std::int64_t scale = 1;
    for (std::size_t i = 0; i < fraction.size(); ++i) {
        scale *= 10;
    }
    if (!numerator.has_value()) {
        return Reason{ReasonCode::MalformedNumber, "amount is not representable"};
    }
    const auto scaled = numerator->divide(Rational{scale});
    if (!scaled.ok()) {
        return scaled.reason();
    }
    return scaled.value();
}

Result<Unit> parse_unit(std::string_view text) {
    const auto unit = unit_from_symbol(text);
    if (!unit.has_value()) {
        return Reason{ReasonCode::UnknownUnit, "unit \"" + std::string(text) + "\" is not registered"};
    }
    return *unit;
}

Result<Instant> parse_instant(const std::string& text) { return Instant::parse_iso8601(text); }

Instant resolve_now(const Args& args, bool* was_explicit) {
    const auto explicit_now = option(args, "now");
    if (explicit_now.has_value()) {
        const Result<Instant> parsed = parse_instant(*explicit_now);
        if (!parsed.ok()) {
            if (was_explicit != nullptr) {
                *was_explicit = false;
            }
            return Instant{};
        }
        if (was_explicit != nullptr) {
            *was_explicit = true;
        }
        return parsed.value();
    }
    if (was_explicit != nullptr) {
        *was_explicit = false;
    }
    return platform::system_utc_now();
}

Result<Interval> parse_interval(const Args& args) {
    const auto start_text = option(args, "start");
    const auto end_text = option(args, "end");
    if (!start_text.has_value() || !end_text.has_value()) {
        return Reason{ReasonCode::InvalidArgument, "both --start and --end are required"};
    }
    const Result<Instant> start = parse_instant(*start_text);
    if (!start.ok()) {
        return Reason{start.code(), "--start: " + start.reason().detail()};
    }
    const Result<Instant> end = parse_instant(*end_text);
    if (!end.ok()) {
        return Reason{end.code(), "--end: " + end.reason().detail()};
    }
    return Interval::make(start.value(), end.value());
}

int emit_raw(const std::string& json_text, bool as_json, const std::string& text,
             int exit_code) {
    if (as_json) {
        std::cout << json_text << "\n";
    } else {
        std::cout << text << "\n";
    }
    return exit_code;
}

int emit(const Json& json, bool as_json, const std::string& text, int exit_code) {
    if (as_json) {
        std::cout << json.dump() << "\n";
    } else {
        std::cout << text << "\n";
    }
    return exit_code;
}

int emit_failure(ReasonCode code, const std::string& detail, bool as_json, const std::string& context) {
    Json json = Json::object();
    json.set("code", Json::text(std::string(to_string(code))));
    json.set("detail", Json::text(detail));
    json.set("ok", Json::boolean(false));
    json.set("operation", Json::text(context));
    if (as_json) {
        std::cout << json.dump() << "\n";
    } else {
        std::cerr << "refused [" << to_string(code) << "] " << context << ": " << detail << "\n";
    }
    return 1;
}

int emit_usage(const std::string& detail) {
    std::cerr << "usage error: " << detail << "\n";
    return 2;
}

Json outcome_json(const CommandOutcome& outcome) {
    Json json = Json::object();
    json.set("applied", Json::boolean(outcome.applied));
    json.set("chain_head", Json::text(outcome.chain_head.to_hex()));
    json.set("code", Json::text(std::string(to_string(outcome.code))));
    json.set("detail", Json::text(outcome.detail));
    json.set("epoch", Json::uinteger(outcome.epoch.value()));
    json.set("outcome_id", Json::text(outcome.outcome_id));
    json.set("record_digest", Json::text(outcome.record_digest.to_hex()));
    json.set("record_type", Json::text(std::string(to_string(outcome.record_type))));
    json.set("replayed", Json::boolean(outcome.replayed));
    json.set("revision", Json::uinteger(outcome.revision.value()));
    json.set("sequence", Json::uinteger(outcome.sequence.value()));
    return json;
}

std::string outcome_text(const std::string& verb, const CommandOutcome& outcome) {
    return verb + ": " + (outcome.replayed ? "replayed" : "committed") + " id=" + outcome.outcome_id +
           " record=" + std::string(to_string(outcome.record_type)) +
           " revision=" + std::to_string(outcome.revision.value()) +
           " sequence=" + std::to_string(outcome.sequence.value()) +
           " epoch=" + std::to_string(outcome.epoch.value()) +
           " digest=" + outcome.record_digest.to_hex().substr(0, 16);
}

CommandContext build_context(const Args& args, const Instant& now) {
    CommandContext context;
    context.now = now;
    const auto key = option(args, "idempotency");
    if (key.has_value()) {
        const Result<IdempotencyKey> parsed = IdempotencyKey::parse(*key);
        if (parsed.ok()) {
            context.idempotency = parsed.value();
        }
    }
    const auto revision = option(args, "expect-revision");
    if (revision.has_value()) {
        context.expected_revision = Revision{std::stoull(*revision)};
    }
    const auto epoch = option(args, "expect-epoch");
    if (epoch.has_value()) {
        context.expected_epoch = Epoch{std::stoull(*epoch)};
    }
    return context;
}

Result<SourceKind> parse_source_kind(const std::string& text) {
    const auto kind = source_kind_from_string(text);
    if (!kind.has_value()) {
        return Reason{ReasonCode::InvalidArgument, "unknown source kind \"" + text + "\""};
    }
    return *kind;
}

Result<ServiceClass> parse_class(const std::string& text) {
    const auto klass = service_class_from_string(text);
    if (!klass.has_value()) {
        return Reason{ReasonCode::InvalidArgument, "unknown service class \"" + text + "\""};
    }
    return *klass;
}

Result<SubjectKind> parse_subject_kind(const std::string& text) {
    const auto kind = subject_kind_from_string(text);
    if (!kind.has_value()) {
        return Reason{ReasonCode::InvalidArgument, "unknown subject kind \"" + text + "\""};
    }
    return *kind;
}

Result<TargetKind> parse_target_kind(const std::string& text) {
    if (text == "measurement") {
        return TargetKind::Measurement;
    }
    if (text == "classification") {
        return TargetKind::Classification;
    }
    if (text == "residual") {
        return TargetKind::Residual;
    }
    if (text == "source") {
        return TargetKind::SourceRegistration;
    }
    return Reason{ReasonCode::InvalidArgument, "unknown target kind \"" + text + "\""};
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int command_init(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("init requires a ledger directory");
    }
    const std::string ledger_id = option_or(args, "ledger", "facility");
    const Result<LedgerId> id = LedgerId::parse(ledger_id);
    if (!id.ok()) {
        return emit_failure(id.code(), id.reason().detail(), as_json, "init");
    }
    const Result<Ledger> ledger = Ledger::create(args.positional[0], id.value(), "fel-cli");
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "init");
    }
    Json json = Json::object();
    json.set("chain_head", Json::text(ledger.value().store().chain_head().to_hex()));
    json.set("directory", Json::text(args.positional[0]));
    json.set("epoch", Json::uinteger(ledger.value().store().epoch().value()));
    json.set("ledger", Json::text(ledger_id));
    json.set("revision", Json::uinteger(ledger.value().store().revision().value()));
    json.set("runtime", Json::text(std::string(kRuntimeIdentity)));
    json.set("version", Json::text(std::string(library_version_string())));
    return emit(json, as_json,
                "initialized ledger " + ledger_id + " at " + args.positional[0] +
                    " (epoch " + std::to_string(ledger.value().store().epoch().value()) + ")",
                0);
}

int command_verify(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("verify requires a ledger directory");
    }
    const Result<IntegrityReport> report = verify_ledger_directory(args.positional[0]);
    if (!report.ok()) {
        return emit_failure(report.code(), report.reason().detail(), as_json, "verify");
    }
    Json json = Json::object();
    json.set("chain_head", Json::text(report.value().chain_head.to_hex()));
    json.set("detail", Json::text(report.value().detail));
    json.set("epoch", Json::uinteger(report.value().epoch.value()));
    json.set("ok", Json::boolean(report.value().ok));
    json.set("revision", Json::uinteger(report.value().revision.value()));
    json.set("segment_records", Json::uinteger(report.value().segment_records));
    json.set("snapshot_digest", Json::text(report.value().snapshot_digest.to_hex()));
    json.set("snapshot_records", Json::uinteger(report.value().snapshot_records));
    json.set("torn_tail", Json::boolean(report.value().torn_tail));
    json.set("torn_tail_bytes", Json::uinteger(report.value().torn_bytes));
    return emit(json, as_json,
                std::string("verify: ") + (report.value().ok ? "ok" : "failed") + "; " +
                    report.value().detail,
                0);
}

int command_status(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("status requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Reader;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "status");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    const Result<LedgerView> view = ledger.value().view(now);
    if (!view.ok()) {
        return emit_failure(view.code(), view.reason().detail(), as_json, "status");
    }
    Json json = Json::object();
    json.set("chain_head", Json::text(view.value().chain_head().to_hex()));
    json.set("epoch", Json::uinteger(view.value().epoch().value()));
    json.set("ledger", Json::text(view.value().ledger().str()));
    json.set("measurements", Json::uinteger(view.value().measurements().size()));
    json.set("allocations", Json::uinteger(view.value().allocations().size()));
    json.set("residuals", Json::uinteger(view.value().residuals().size()));
    json.set("recovered_session", Json::boolean(view.value().recovered_session()));
    json.set("revision", Json::uinteger(view.value().revision().value()));
    json.set("seals", Json::uinteger(view.value().seals().size()));
    json.set("sources", Json::uinteger(view.value().sources().size()));
    json.set("issues", Json::uinteger(view.value().issues().size()));
    Json sources = Json::array();
    for (const SourceState& source : view.value().sources()) {
        Json item = Json::object();
        item.set("freshness", Json::text(std::string(to_string(source.freshness))));
        item.set("generation", Json::uinteger(source.generation.value()));
        item.set("has_generation", Json::boolean(source.has_generation));
        item.set("kind", Json::text(std::string(to_string(source.kind))));
        item.set("measurements", Json::uinteger(source.measurement_count));
        item.set("retired", Json::boolean(source.retired));
        item.set("source", Json::text(source.id.str()));
        item.set("unit", Json::text(std::string(unit_symbol(source.unit))));
        sources.push(std::move(item));
    }
    json.set("source_detail", std::move(sources));
    std::string text = "ledger " + view.value().ledger().str() +
                       " revision=" + std::to_string(view.value().revision().value()) +
                       " epoch=" + std::to_string(view.value().epoch().value()) +
                       " sources=" + std::to_string(view.value().sources().size()) +
                       " measurements=" + std::to_string(view.value().measurements().size()) +
                       " allocations=" + std::to_string(view.value().allocations().size()) +
                       " residuals=" + std::to_string(view.value().residuals().size());
    return emit(json, as_json, text, 0);
}

int command_source(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("source requires a ledger directory");
    }
    const bool retire = args.subcommand == "retire";
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "source");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    if (retire) {
        const auto id_text = option(args, "id");
        if (!id_text.has_value()) {
            return emit_usage("source retire requires --id");
        }
        const Result<SourceId> id = SourceId::parse(*id_text);
        if (!id.ok()) {
            return emit_failure(id.code(), id.reason().detail(), as_json, "source retire");
        }
        RetireSourceRequest request;
        request.context = build_context(args, now);
        request.source = id.value();
        request.reason = option_or(args, "reason", "");
        const Result<CommandOutcome> outcome = ledger.value().retire_source(request);
        if (!outcome.ok()) {
            return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "source retire");
        }
        return emit(outcome_json(outcome.value()), as_json,
                    outcome_text("source retire", outcome.value()), 0);
    }
    const auto id_text = option(args, "id");
    if (!id_text.has_value()) {
        return emit_usage("source add requires --id");
    }
    const Result<SourceId> id = SourceId::parse(*id_text);
    if (!id.ok()) {
        return emit_failure(id.code(), id.reason().detail(), as_json, "source add");
    }
    const Result<SourceKind> kind = parse_source_kind(option_or(args, "kind", "meter"));
    if (!kind.ok()) {
        return emit_failure(kind.code(), kind.reason().detail(), as_json, "source add");
    }
    const Result<Unit> unit = parse_unit(option_or(args, "unit", "J"));
    if (!unit.ok()) {
        return emit_failure(unit.code(), unit.reason().detail(), as_json, "source add");
    }
    RegisterSourceRequest request;
    request.context = build_context(args, now);
    request.source = id.value();
    request.kind = kind.value();
    request.unit = unit.value();
    request.label = option_or(args, "label", "");
    const auto authority_text = option(args, "authority");
    if (authority_text.has_value()) {
        const Result<AuthorityId> authority = AuthorityId::parse(*authority_text);
        if (!authority.ok()) {
            return emit_failure(authority.code(), authority.reason().detail(), as_json, "source add");
        }
        request.authority = authority.value();
    }
    const auto role_text = option(args, "authority-role");
    if (role_text.has_value()) {
        const auto role = authority::role_from_string(*role_text);
        if (!role.has_value()) {
            return emit_failure(ReasonCode::InvalidArgument,
                                "unknown authority role \"" + *role_text + "\"", as_json,
                                "source add");
        }
        if (request.authority.valid()) {
            const Result<authority::Contract> contract =
                authority::default_contract(role.value(), request.authority);
            if (!contract.ok()) {
                return emit_failure(contract.code(), contract.reason().detail(), as_json, "source add");
            }
            const authority::BoundaryVerdict verdict =
                authority::check_evidence_use(contract.value(), authority::FactClass::Accounting);
            if (!verdict.permitted) {
                return emit_failure(verdict.code, verdict.detail, as_json, "source add");
            }
        }
    }
    const Result<CommandOutcome> outcome = ledger.value().register_source(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "source add");
    }
    return emit(outcome_json(outcome.value()), as_json,
                outcome_text("source add", outcome.value()), 0);
}

int command_generation(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("generation requires a ledger directory");
    }
    const bool attest = args.subcommand == "attest";
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "generation");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto source_text = option(args, "source");
    const auto generation_text = option(args, "generation");
    const auto evidence_text = option(args, "evidence");
    if (!source_text.has_value() || !generation_text.has_value() || !evidence_text.has_value()) {
        return emit_usage("generation requires --source, --generation, and --evidence");
    }
    const Result<SourceId> source = SourceId::parse(*source_text);
    if (!source.ok()) {
        return emit_failure(source.code(), source.reason().detail(), as_json, "generation");
    }
    const Result<Digest> evidence = Digest::parse_hex(*evidence_text);
    if (!evidence.ok()) {
        return emit_failure(evidence.code(), evidence.reason().detail(), as_json, "generation");
    }
    Generation generation{std::stoull(*generation_text)};
    Instant valid_until{};
    const auto valid_text = option(args, "valid-until");
    if (valid_text.has_value()) {
        const Result<Instant> parsed = parse_instant(*valid_text);
        if (!parsed.ok()) {
            return emit_failure(parsed.code(), parsed.reason().detail(), as_json, "generation");
        }
        valid_until = parsed.value();
    }
    const CommandContext context = build_context(args, now);
    const std::string method = option_or(args, "method", "");
    Result<CommandOutcome> outcome =
        attest ? ledger.value().attest_generation(AttestGenerationRequest{
                     context, source.value(), generation, evidence.value(), valid_until, method})
               : ledger.value().publish_generation(PublishGenerationRequest{
                     context, source.value(), generation, evidence.value(), valid_until, method});
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json,
                            attest ? "generation attest" : "generation publish");
    }
    return emit(outcome_json(outcome.value()), as_json,
                outcome_text(attest ? "generation attest" : "generation publish", outcome.value()),
                0);
}

int command_measure(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("measure requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "measure");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto entry_text = option(args, "entry");
    const auto source_text = option(args, "source");
    if (!entry_text.has_value() || !source_text.has_value()) {
        return emit_usage("measure requires --entry and --source");
    }
    const Result<EntryId> entry = EntryId::parse(*entry_text);
    if (!entry.ok()) {
        return emit_failure(entry.code(), entry.reason().detail(), as_json, "measure");
    }
    const Result<SourceId> source = SourceId::parse(*source_text);
    if (!source.ok()) {
        return emit_failure(source.code(), source.reason().detail(), as_json, "measure");
    }
    const Result<Interval> interval = parse_interval(args);
    if (!interval.ok()) {
        return emit_failure(interval.code(), interval.reason().detail(), as_json, "measure");
    }
    const auto amount_text = option(args, "amount");
    if (!amount_text.has_value()) {
        return emit_usage("measure requires --amount");
    }
    const Result<Rational> amount = parse_amount(*amount_text);
    if (!amount.ok()) {
        return emit_failure(amount.code(), amount.reason().detail(), as_json, "measure");
    }
    const Result<Unit> unit = parse_unit(option_or(args, "unit", "J"));
    if (!unit.ok()) {
        return emit_failure(unit.code(), unit.reason().detail(), as_json, "measure");
    }
    const auto evidence_text = option(args, "evidence");
    if (!evidence_text.has_value()) {
        return emit_usage("measure requires --evidence <sha256 hex>");
    }
    const Result<Digest> evidence = Digest::parse_hex(*evidence_text);
    if (!evidence.ok()) {
        return emit_failure(evidence.code(), evidence.reason().detail(), as_json, "measure");
    }
    RecordMeasurementRequest request;
    request.context = build_context(args, now);
    request.entry = entry.value();
    request.interval = interval.value();
    request.source = source.value();
    request.generation = Generation{std::stoull(option_or(args, "generation", "1"))};
    request.quantity = Quantity{amount.value(), unit.value()};
    request.subject_kind = SubjectKind::Unattributed;
    const auto subject_kind = option(args, "subject-kind");
    if (subject_kind.has_value()) {
        const Result<SubjectKind> parsed = parse_subject_kind(*subject_kind);
        if (!parsed.ok()) {
            return emit_failure(parsed.code(), parsed.reason().detail(), as_json, "measure");
        }
        request.subject_kind = parsed.value();
    }
    request.subject = option_or(args, "subject", "");
    request.evidence = evidence.value();
    request.method = option_or(args, "method", "");
    const auto supersedes = option(args, "supersedes");
    if (supersedes.has_value()) {
        const Result<EntryId> target = EntryId::parse(*supersedes);
        if (!target.ok()) {
            return emit_failure(target.code(), target.reason().detail(), as_json, "measure");
        }
        const auto digest_text = option(args, "supersedes-digest");
        if (!digest_text.has_value()) {
            return emit_usage("--supersedes requires --supersedes-digest");
        }
        const Result<Digest> target_digest = Digest::parse_hex(*digest_text);
        if (!target_digest.ok()) {
            return emit_failure(target_digest.code(), target_digest.reason().detail(), as_json,
                                "measure");
        }
        request.supersedes = true;
        request.supersedes_entry = target.value();
        request.supersedes_digest = target_digest.value();
        request.reason = option_or(args, "reason", "");
    }
    const Result<CommandOutcome> outcome = ledger.value().record_measurement(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "measure");
    }
    return emit(outcome_json(outcome.value()), as_json, outcome_text("measure", outcome.value()), 0);
}

int command_classify(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("classify requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "classify");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto allocation_text = option(args, "allocation");
    const auto entry_text = option(args, "entry");
    const auto class_text = option(args, "class");
    const auto amount_text = option(args, "amount");
    if (!allocation_text.has_value() || !entry_text.has_value() || !class_text.has_value() ||
        !amount_text.has_value()) {
        return emit_usage("classify requires --allocation, --entry, --class, and --amount");
    }
    const Result<AllocationId> allocation = AllocationId::parse(*allocation_text);
    if (!allocation.ok()) {
        return emit_failure(allocation.code(), allocation.reason().detail(), as_json, "classify");
    }
    const Result<EntryId> entry = EntryId::parse(*entry_text);
    if (!entry.ok()) {
        return emit_failure(entry.code(), entry.reason().detail(), as_json, "classify");
    }
    const Result<ServiceClass> klass = parse_class(*class_text);
    if (!klass.ok()) {
        return emit_failure(klass.code(), klass.reason().detail(), as_json, "classify");
    }
    const Result<Rational> amount = parse_amount(*amount_text);
    if (!amount.ok()) {
        return emit_failure(amount.code(), amount.reason().detail(), as_json, "classify");
    }
    const Result<Unit> unit = parse_unit(option_or(args, "unit", "J"));
    if (!unit.ok()) {
        return emit_failure(unit.code(), unit.reason().detail(), as_json, "classify");
    }
    const auto evidence_text = option(args, "evidence");
    if (!evidence_text.has_value()) {
        return emit_usage("classify requires --evidence <sha256 hex>");
    }
    const Result<Digest> evidence = Digest::parse_hex(*evidence_text);
    if (!evidence.ok()) {
        return emit_failure(evidence.code(), evidence.reason().detail(), as_json, "classify");
    }
    RecordClassificationRequest request;
    request.context = build_context(args, now);
    request.allocation = allocation.value();
    request.entry = entry.value();
    request.klass = klass.value();
    request.quantity = Quantity{amount.value(), unit.value()};
    const auto subject_kind = option(args, "subject-kind");
    if (subject_kind.has_value()) {
        const Result<SubjectKind> parsed = parse_subject_kind(*subject_kind);
        if (!parsed.ok()) {
            return emit_failure(parsed.code(), parsed.reason().detail(), as_json, "classify");
        }
        request.subject_kind = parsed.value();
    }
    request.subject = option_or(args, "subject", "");
    request.evidence = evidence.value();
    request.method = option_or(args, "method", "");
    const auto supersedes = option(args, "supersedes");
    if (supersedes.has_value()) {
        const Result<AllocationId> target = AllocationId::parse(*supersedes);
        if (!target.ok()) {
            return emit_failure(target.code(), target.reason().detail(), as_json, "classify");
        }
        const auto digest_text = option(args, "supersedes-digest");
        if (!digest_text.has_value()) {
            return emit_usage("--supersedes requires --supersedes-digest");
        }
        const Result<Digest> target_digest = Digest::parse_hex(*digest_text);
        if (!target_digest.ok()) {
            return emit_failure(target_digest.code(), target_digest.reason().detail(), as_json,
                                "classify");
        }
        request.supersedes = true;
        request.supersedes_allocation = target.value();
        request.supersedes_digest = target_digest.value();
        request.reason = option_or(args, "reason", "");
    }
    const Result<CommandOutcome> outcome = ledger.value().record_classification(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "classify");
    }
    return emit(outcome_json(outcome.value()), as_json, outcome_text("classify", outcome.value()), 0);
}

int command_residual(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("residual requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "residual");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto residual_text = option(args, "residual");
    const auto class_text = option(args, "class");
    const auto basis_text = option(args, "basis");
    if (!residual_text.has_value() || !class_text.has_value() || !basis_text.has_value()) {
        return emit_usage("residual requires --residual, --class, and --basis");
    }
    const Result<ResidualId> residual = ResidualId::parse(*residual_text);
    if (!residual.ok()) {
        return emit_failure(residual.code(), residual.reason().detail(), as_json, "residual");
    }
    const Result<ServiceClass> klass = parse_class(*class_text);
    if (!klass.ok()) {
        return emit_failure(klass.code(), klass.reason().detail(), as_json, "residual");
    }
    const Result<Interval> interval = parse_interval(args);
    if (!interval.ok()) {
        return emit_failure(interval.code(), interval.reason().detail(), as_json, "residual");
    }
    RecordResidualRequest request;
    request.context = build_context(args, now);
    request.residual = residual.value();
    request.interval = interval.value();
    request.klass = klass.value();
    request.basis = *basis_text;
    request.quantified = !has_flag(args, "unquantified");
    const auto amount_text = option(args, "amount");
    const Result<Unit> unit = parse_unit(option_or(args, "unit", "J"));
    if (!unit.ok()) {
        return emit_failure(unit.code(), unit.reason().detail(), as_json, "residual");
    }
    if (request.quantified) {
        if (!amount_text.has_value()) {
            return emit_usage("a quantified residual requires --amount");
        }
        const Result<Rational> amount = parse_amount(*amount_text);
        if (!amount.ok()) {
            return emit_failure(amount.code(), amount.reason().detail(), as_json, "residual");
        }
        request.quantity = Quantity{amount.value(), unit.value()};
    } else {
        request.quantity = Quantity{Rational{0}, unit.value()};
    }
    const auto bound = option(args, "bound-entry");
    if (bound.has_value()) {
        const Result<EntryId> entry = EntryId::parse(*bound);
        if (!entry.ok()) {
            return emit_failure(entry.code(), entry.reason().detail(), as_json, "residual");
        }
        request.bound_to_entry = true;
        request.entry = entry.value();
    }
    const auto subject_kind = option(args, "subject-kind");
    if (subject_kind.has_value()) {
        const Result<SubjectKind> parsed = parse_subject_kind(*subject_kind);
        if (!parsed.ok()) {
            return emit_failure(parsed.code(), parsed.reason().detail(), as_json, "residual");
        }
        request.subject_kind = parsed.value();
    }
    request.subject = option_or(args, "subject", "");
    const auto evidence_text = option(args, "evidence");
    if (!evidence_text.has_value()) {
        return emit_usage("residual requires --evidence <sha256 hex>");
    }
    const Result<Digest> evidence = Digest::parse_hex(*evidence_text);
    if (!evidence.ok()) {
        return emit_failure(evidence.code(), evidence.reason().detail(), as_json, "residual");
    }
    request.evidence = evidence.value();
    const auto supersedes = option(args, "supersedes");
    if (supersedes.has_value()) {
        const Result<ResidualId> target = ResidualId::parse(*supersedes);
        if (!target.ok()) {
            return emit_failure(target.code(), target.reason().detail(), as_json, "residual");
        }
        const auto digest_text = option(args, "supersedes-digest");
        if (!digest_text.has_value()) {
            return emit_usage("--supersedes requires --supersedes-digest");
        }
        const Result<Digest> target_digest = Digest::parse_hex(*digest_text);
        if (!target_digest.ok()) {
            return emit_failure(target_digest.code(), target_digest.reason().detail(), as_json,
                                "residual");
        }
        request.supersedes = true;
        request.supersedes_residual = target.value();
        request.supersedes_digest = target_digest.value();
        request.reason = option_or(args, "reason", "");
    }
    const Result<CommandOutcome> outcome = ledger.value().record_residual(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "residual");
    }
    return emit(outcome_json(outcome.value()), as_json, outcome_text("residual", outcome.value()), 0);
}

int command_void(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("void requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "void");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto void_id = option(args, "void-id");
    const auto target = option(args, "target");
    const auto target_kind = option(args, "target-kind");
    const auto target_digest = option(args, "target-digest");
    if (!void_id.has_value() || !target.has_value() || !target_kind.has_value() ||
        !target_digest.has_value()) {
        return emit_usage("void requires --void-id, --target, --target-kind, and --target-digest");
    }
    const Result<VoidId> id = VoidId::parse(*void_id);
    if (!id.ok()) {
        return emit_failure(id.code(), id.reason().detail(), as_json, "void");
    }
    const Result<TargetKind> kind = parse_target_kind(*target_kind);
    if (!kind.ok()) {
        return emit_failure(kind.code(), kind.reason().detail(), as_json, "void");
    }
    const Result<Digest> digest = Digest::parse_hex(*target_digest);
    if (!digest.ok()) {
        return emit_failure(digest.code(), digest.reason().detail(), as_json, "void");
    }
    VoidTargetRequest request;
    request.context = build_context(args, now);
    request.void_id = id.value();
    request.target_kind = kind.value();
    request.target_id = *target;
    request.target_digest = digest.value();
    request.reason = option_or(args, "reason", "");
    const Result<CommandOutcome> outcome = ledger.value().void_target(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "void");
    }
    return emit(outcome_json(outcome.value()), as_json, outcome_text("void", outcome.value()), 0);
}

int command_reconcile(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("reconcile requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Reader;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "reconcile");
    }
    const Result<Interval> interval = parse_interval(args);
    if (!interval.ok()) {
        return emit_failure(interval.code(), interval.reason().detail(), as_json, "reconcile");
    }
    ReconcileRequest request;
    request.interval = interval.value();
    const std::string basis = option_or(args, "basis", "current");
    if (basis == "current") {
        request.basis = BasisPolicy::Current;
    } else if (basis == "historical") {
        request.basis = BasisPolicy::Historical;
    } else {
        return emit_usage("--basis must be current or historical");
    }
    const auto as_of = option(args, "as-of");
    if (as_of.has_value()) {
        const Result<Instant> parsed = parse_instant(*as_of);
        if (!parsed.ok()) {
            return emit_failure(parsed.code(), parsed.reason().detail(), as_json, "reconcile");
        }
        request.as_of = parsed.value();
    }
    const Result<ReconciliationReport> report = ledger.value().reconcile(request);
    if (!report.ok()) {
        return emit_failure(report.code(), report.reason().detail(), as_json, "reconcile");
    }
    std::string text = std::string("reconcile ") + (report.value().closed ? "closed" : "OPEN") +
                       " code=" + std::string(to_string(report.value().code)) +
                       " revision=" + std::to_string(report.value().revision.value()) +
                       " digest=" + report.value().report_digest.to_hex().substr(0, 16);
    for (const DimensionClosure& closure : report.value().dimensions) {
        text.append("\n  ");
        text.append(dimension_name(closure.dimension));
        text.append(": input=");
        text.append(closure.input_total.to_string());
        text.append(" useful=");
        text.append(closure.classified_useful.to_string());
        text.append(" avoidable=");
        text.append(closure.classified_avoidable.to_string());
        text.append(" stranded=");
        text.append(closure.classified_stranded.to_string());
        text.append(" wasted=");
        text.append(closure.classified_wasted.to_string());
        text.append(" unknown=");
        text.append(closure.classified_unknown.to_string());
        text.append(" unmeasured=");
        text.append(closure.classified_unmeasured.to_string());
        text.append(" unclassified=");
        text.append(closure.unclassified.to_string());
        text.append(" -> ");
        text.append(closure.explanation);
    }
    for (const ClosureIssue& issue : report.value().issues) {
        text.append("\n  issue ");
        text.append(to_string(issue.code));
        text.append(" ");
        text.append(issue.subject);
        text.append(": ");
        text.append(issue.detail);
    }
    return emit_raw(render_json(report.value()), as_json, text, report.value().closed ? 0 : 1);
}

int command_seal(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("seal requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "seal");
    }
    bool explicit_now = false;
    const Instant now = resolve_now(args, &explicit_now);
    if (!explicit_now) {
        return emit_usage("mutating commands require --now <ISO-8601 UTC instant>");
    }
    const auto seal_text = option(args, "seal");
    if (!seal_text.has_value()) {
        return emit_usage("seal requires --seal");
    }
    const Result<SealId> seal = SealId::parse(*seal_text);
    if (!seal.ok()) {
        return emit_failure(seal.code(), seal.reason().detail(), as_json, "seal");
    }
    const Result<Interval> interval = parse_interval(args);
    if (!interval.ok()) {
        return emit_failure(interval.code(), interval.reason().detail(), as_json, "seal");
    }
    SealIntervalRequest request;
    request.context = build_context(args, now);
    request.seal = seal.value();
    request.interval = interval.value();
    request.note = option_or(args, "note", "");
    const Result<CommandOutcome> outcome = ledger.value().seal_interval(request);
    if (!outcome.ok()) {
        return emit_failure(outcome.code(), outcome.reason().detail(), as_json, "seal");
    }
    return emit(outcome_json(outcome.value()), as_json, outcome_text("seal", outcome.value()), 0);
}

int command_compact(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("compact requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Writer;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "compact");
    }
    const Result<std::uint64_t> dropped = ledger.value().compact();
    if (!dropped.ok()) {
        return emit_failure(dropped.code(), dropped.reason().detail(), as_json, "compact");
    }
    Json json = Json::object();
    json.set("dropped_records", Json::uinteger(dropped.value()));
    json.set("epoch", Json::uinteger(ledger.value().store().epoch().value()));
    json.set("revision", Json::uinteger(ledger.value().store().revision().value()));
    json.set("segment", Json::text(ledger.value().store().active_segment_name()));
    json.set("snapshot", Json::text(ledger.value().store().base_snapshot_name()));
    return emit(json, as_json,
                "compacted: dropped=" + std::to_string(dropped.value()) +
                    " segment=" + ledger.value().store().active_segment_name() +
                    " snapshot=" + ledger.value().store().base_snapshot_name(),
                0);
}

int command_export(const Args& args, bool as_json) {
    if (args.positional.empty()) {
        return emit_usage("export requires a ledger directory");
    }
    StoreOptions options;
    options.mode = OpenMode::Reader;
    Result<Ledger> ledger = Ledger::open(args.positional[0], options);
    if (!ledger.ok()) {
        return emit_failure(ledger.code(), ledger.reason().detail(), as_json, "export");
    }
    Json records = Json::array();
    for (const Record& record : ledger.value().store().records()) {
        Json item = Json::object();
        item.set("digest", Json::text(record.content_digest().to_hex()));
        item.set("epoch", Json::uinteger(record.epoch().value()));
        item.set("revision", Json::uinteger(record.revision().value()));
        item.set("sequence", Json::uinteger(record.sequence().value()));
        item.set("type", Json::text(std::string(to_string(record.type()))));
        switch (record.type()) {
            case RecordType::SourceRegistered:
                item.set("source", Json::text(record.source_registered().source.str()));
                item.set("unit", Json::text(std::string(unit_symbol(record.source_registered().unit))));
                break;
            case RecordType::MeasurementRecorded:
                item.set("entry", Json::text(record.measurement().entry.str()));
                item.set("interval", Json::text(record.measurement().interval.to_string()));
                item.set("quantity", quantity_json(record.measurement().quantity));
                break;
            case RecordType::ClassificationRecorded:
                item.set("allocation", Json::text(record.classification().allocation.str()));
                item.set("entry", Json::text(record.classification().entry.str()));
                item.set("class", Json::text(std::string(to_string(record.classification().klass))));
                item.set("quantity", quantity_json(record.classification().quantity));
                break;
            case RecordType::ResidualRecorded:
                item.set("residual", Json::text(record.residual().residual.str()));
                item.set("class", Json::text(std::string(to_string(record.residual().klass))));
                item.set("quantified", Json::boolean(record.residual().quantified));
                item.set("quantity", quantity_json(record.residual().quantity));
                break;
            case RecordType::TargetVoided:
                item.set("void_id", Json::text(record.target_voided().void_id.str()));
                item.set("target", Json::text(record.target_voided().target_id));
                break;
            case RecordType::IntervalSealed:
                item.set("seal", Json::text(record.interval_sealed().seal.str()));
                item.set("closed", Json::boolean(record.interval_sealed().closed));
                break;
            case RecordType::EpochAdopted:
                item.set("basis", Json::text(std::string(to_string(record.epoch_adopted().basis))));
                break;
            case RecordType::GenerationPublished:
            case RecordType::GenerationAttested:
            case RecordType::SegmentHeader:
            case RecordType::LedgerOpened:
            case RecordType::SourceRetired:
                break;
        }
        if (record.meta().supersedes) {
            item.set("supersedes", Json::text(record.meta().superseded_id));
            item.set("supersedes_reason", Json::text(record.meta().reason));
        }
        records.push(std::move(item));
    }
    Json json = Json::object();
    json.set("count", Json::uinteger(ledger.value().store().records().size()));
    json.set("ledger", Json::text(ledger.value().store().records().empty()
                                      ? std::string{}
                                      : ledger.value().index().ledger().str()));
    json.set("records", std::move(records));
    json.set("revision", Json::uinteger(ledger.value().revision().value()));
    if (as_json) {
        std::cout << json.dump() << "\n";
        return 0;
    }
    std::cout << "records=" << ledger.value().store().records().size()
              << " revision=" << ledger.value().revision().value() << "\n";
    for (const Record& record : ledger.value().store().records()) {
        std::cout << "  " << record.describe() << "\n";
    }
    return 0;
}

int command_boundary(const Args& args, bool as_json) {
    const auto role_text = option(args, "role");
    const auto fact_text = option(args, "fact");
    if (!role_text.has_value()) {
        if (as_json) {
            Json json = Json::object();
            Json roles = Json::array();
            for (const char* name : {"dccp", "asi", "dfi", "bms", "telemetry", "external"}) {
                roles.push(Json::text(name));
            }
            json.set("roles", std::move(roles));
            json.set("table", Json::text(authority::render_boundary_table()));
            std::cout << json.dump() << "\n";
            return 0;
        }
        std::cout << authority::render_boundary_table();
        return 0;
    }
    const auto role = authority::role_from_string(*role_text);
    if (!role.has_value()) {
        return emit_usage("unknown --role " + *role_text);
    }
    const Result<AuthorityId> id = AuthorityId::parse(std::string("authority-") + *role_text);
    if (!id.ok()) {
        return emit_failure(id.code(), id.reason().detail(), as_json, "boundary");
    }
    const Result<authority::Contract> contract = authority::default_contract(role.value(), id.value());
    if (!contract.ok()) {
        return emit_failure(contract.code(), contract.reason().detail(), as_json, "boundary");
    }
    if (!fact_text.has_value()) {
        Json json = Json::object();
        json.set("authority", Json::text(contract.value().id.str()));
        json.set("description", Json::text(contract.value().description));
        json.set("may_assert_classification",
                 Json::boolean(contract.value().may_assert_classification));
        json.set("role", Json::text(std::string(authority::to_string(contract.value().role))));
        std::string text = std::string(authority::to_string(contract.value().role)) + " (" +
                           contract.value().id.str() + "): " + contract.value().description;
        return emit(json, as_json, text, 0);
    }
    const auto fact = authority::fact_from_string(*fact_text);
    if (!fact.has_value()) {
        return emit_usage("unknown --fact " + *fact_text);
    }
    const authority::BoundaryVerdict verdict = authority::check_evidence_use(contract.value(), *fact);
    const authority::BoundaryVerdict ownership = authority::check_fact_ownership(*fact);
    Json json = Json::object();
    json.set("code", Json::text(std::string(to_string(ownership.code))));
    json.set("fact", Json::text(std::string(authority::to_string(*fact))));
    json.set("ledger_owned", Json::boolean(authority::is_ledger_owned(*fact)));
    json.set("permitted", Json::boolean(ownership.permitted && verdict.permitted));
    json.set("detail", Json::text(ownership.permitted ? verdict.detail : ownership.detail));
    return emit(json, as_json, (ownership.permitted ? "permitted: " : "refused: ") +
                                   (ownership.permitted ? verdict.detail : ownership.detail),
                ownership.permitted && verdict.permitted ? 0 : 1);
}

void print_usage() {
    std::cout <<
        "fel " << library_version_string() << " - Facility Efficiency Ledger\n"
        "\n"
        "usage: fel <command> [arguments] [options]\n"
        "\n"
        "commands:\n"
        "  version                                 print version information\n"
        "  init <dir> --ledger <id>                create a ledger directory\n"
        "  verify <dir>                            verify committed records and the chain\n"
        "  status <dir>                            fold the journal and report state\n"
        "  export <dir>                            dump committed records\n"
        "  compact <dir>                           snapshot and re-anchor the journal\n"
        "  source add <dir> --id --kind --unit [--authority --authority-role --label]\n"
        "  source retire <dir> --id --reason\n"
        "  generation publish <dir> --source --generation --evidence [--valid-until --method]\n"
        "  generation attest <dir> --source --generation --evidence [--valid-until --method]\n"
        "  measure <dir> --entry --source --generation --start --end --amount --unit --evidence\n"
        "  classify <dir> --allocation --entry --class --amount --unit --evidence\n"
        "  residual <dir> --residual --start --end --class --basis --evidence [--amount]\n"
        "  void <dir> --void-id --target-kind --target --target-digest --reason\n"
        "  reconcile <dir> --start --end [--basis current|historical] [--as-of]\n"
        "  seal <dir> --seal --start --end [--note]\n"
        "  boundary [--role <role>] [--fact <fact>]\n"
        "\n"
        "common options:\n"
        "  --json                     emit canonical JSON\n"
        "  --now <ISO-8601 instant>   required by mutating commands; keeps runs reproducible\n"
        "  --idempotency <key>        make the command safe to retry\n"
        "  --expect-revision <n>      refuse if the ledger moved\n"
        "  --expect-epoch <n>         refuse if the ledger session changed\n";
}

}  // namespace

int run(int argc, char** argv) {
    const Result<Args> parsed = parse_args(argc, argv);
    if (!parsed.ok()) {
        print_usage();
        return 2;
    }
    const Args& args = parsed.value();
    const bool as_json = has_flag(args, "json");

    if (args.command == "version") {
        Json json = Json::object();
        json.set("commit", Json::text(std::string(library_commit_string())));
        json.set("runtime", Json::text(std::string(kRuntimeIdentity)));
        json.set("version", Json::text(std::string(library_version_string())));
        return emit(json, as_json,
                    std::string(kRuntimeIdentity) + " " + std::string(library_version_string()) +
                        " (commit " + std::string(library_commit_string()) + ")",
                    0);
    }
    if (args.command == "help" || has_flag(args, "help")) {
        print_usage();
        return 0;
    }
    if (args.command == "init") {
        return command_init(args, as_json);
    }
    if (args.command == "verify") {
        return command_verify(args, as_json);
    }
    if (args.command == "status") {
        return command_status(args, as_json);
    }
    if (args.command == "source") {
        return command_source(args, as_json);
    }
    if (args.command == "generation") {
        return command_generation(args, as_json);
    }
    if (args.command == "measure") {
        return command_measure(args, as_json);
    }
    if (args.command == "classify") {
        return command_classify(args, as_json);
    }
    if (args.command == "residual") {
        return command_residual(args, as_json);
    }
    if (args.command == "void") {
        return command_void(args, as_json);
    }
    if (args.command == "reconcile") {
        return command_reconcile(args, as_json);
    }
    if (args.command == "seal") {
        return command_seal(args, as_json);
    }
    if (args.command == "compact") {
        return command_compact(args, as_json);
    }
    if (args.command == "export") {
        return command_export(args, as_json);
    }
    if (args.command == "boundary") {
        return command_boundary(args, as_json);
    }
    print_usage();
    return 2;
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "usage error: " << error.what() << "\n";
        return 2;
    }
}
