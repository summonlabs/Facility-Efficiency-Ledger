#include "fel/authority.hpp"

#include <array>
#include <utility>

namespace fel::authority {
namespace {

struct RoleInfo {
    Role role;
    std::string_view name;
    std::string_view description;
};

constexpr std::array<RoleInfo, 6> kRoles{{
    {Role::DataCenterControlPlane, "dccp",
     "capacity, placement, and scheduling authority for the data centre"},
    {Role::AssetStateInventory, "asi",
     "asset and system inventory plus lifecycle authority"},
    {Role::DataFabricIntelligence, "dfi",
     "fabric and flow attribution authority"},
    {Role::BuildingManagement, "bms",
     "building management and DCIM authority for plant and environmental systems"},
    {Role::Telemetry, "telemetry", "raw metering and sensor evidence"},
    {Role::External, "external", "an adjacent authority outside the named roles"},
}};

struct FactInfo {
    FactClass fact;
    std::string_view name;
    bool ledger_owned;
};

constexpr std::array<FactInfo, 16> kFacts{{
    {FactClass::Accounting, "accounting", true},
    {FactClass::Classification, "classification", true},
    {FactClass::Attribution, "attribution", true},
    {FactClass::Reconciliation, "reconciliation", true},
    {FactClass::Residual, "residual", true},
    {FactClass::Generation, "generation", true},
    {FactClass::Correction, "correction", true},
    {FactClass::Seal, "seal", true},
    {FactClass::Capacity, "capacity", false},
    {FactClass::Placement, "placement", false},
    {FactClass::Scheduling, "scheduling", false},
    {FactClass::Pricing, "pricing", false},
    {FactClass::Policy, "policy", false},
    {FactClass::Actuation, "actuation", false},
    {FactClass::EnergyDelivery, "energy_delivery", false},
    {FactClass::ThermalControl, "thermal_control", false},
}};

const FactInfo* find_fact(FactClass fact) noexcept {
    for (const FactInfo& info : kFacts) {
        if (info.fact == fact) {
            return &info;
        }
    }
    return nullptr;
}

const RoleInfo* find_role(Role role) noexcept {
    for (const RoleInfo& info : kRoles) {
        if (info.role == role) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace

std::string_view to_string(Role role) noexcept {
    const RoleInfo* info = find_role(role);
    return info != nullptr ? info->name : std::string_view{"unknown"};
}

std::optional<Role> role_from_string(std::string_view name) noexcept {
    for (const RoleInfo& info : kRoles) {
        if (info.name == name) {
            return info.role;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Disposition disposition) noexcept {
    switch (disposition) {
        case Disposition::Accepted:
            return "accepted";
        case Disposition::Stale:
            return "stale";
        case Disposition::Conflicting:
            return "conflicting";
        case Disposition::Unsupported:
            return "unsupported";
        case Disposition::Refused:
            return "refused";
        case Disposition::Unavailable:
            return "unavailable";
    }
    return "unknown";
}

std::string_view to_string(FactClass fact) noexcept {
    const FactInfo* info = find_fact(fact);
    return info != nullptr ? info->name : std::string_view{"unknown"};
}

std::optional<FactClass> fact_from_string(std::string_view name) noexcept {
    for (const FactInfo& info : kFacts) {
        if (info.name == name) {
            return info.fact;
        }
    }
    return std::nullopt;
}

bool is_ledger_owned(FactClass fact) noexcept {
    const FactInfo* info = find_fact(fact);
    return info != nullptr && info->ledger_owned;
}

Result<Contract> default_contract(Role role, const AuthorityId& id) {
    if (!id.valid()) {
        return Reason{ReasonCode::InvalidIdentifier,
                      "an adjacent authority contract requires an authority identifier"};
    }
    Contract contract;
    contract.id = id;
    contract.role = role;
    switch (role) {
        case Role::DataCenterControlPlane:
            contract.authoritative_for = {FactClass::Capacity, FactClass::Placement,
                                          FactClass::Scheduling};
            contract.consuming = {FactClass::Accounting, FactClass::Reconciliation,
                                  FactClass::Attribution};
            contract.description =
                "consumes ledger accounting as an input to capacity and placement decisions; the "
                "ledger never accepts its capacity or scheduling claims as accounting facts";
            break;
        case Role::AssetStateInventory:
            contract.authoritative_for = {FactClass::Attribution};
            contract.consuming = {FactClass::Accounting, FactClass::Reconciliation};
            contract.description =
                "authoritative for asset identity and lifecycle; ledger attribution references may "
                "cite it, but the reference is evidence, not ownership";
            break;
        case Role::DataFabricIntelligence:
            contract.authoritative_for = {FactClass::Attribution};
            contract.consuming = {FactClass::Accounting};
            contract.description =
                "authoritative for fabric flow attribution; the ledger records its attribution "
                "claims as evidence with the authority named";
            break;
        case Role::BuildingManagement:
            contract.authoritative_for = {FactClass::ThermalControl, FactClass::EnergyDelivery};
            contract.consuming = {FactClass::Accounting, FactClass::Residual};
            contract.description =
                "authoritative for plant control and energy delivery; the ledger consumes measured "
                "quantities and never issues control actions";
            break;
        case Role::Telemetry:
            contract.authoritative_for = {};
            contract.consuming = {FactClass::Accounting};
            contract.description = "provides raw measurement evidence only";
            break;
        case Role::External:
            contract.authoritative_for = {};
            contract.consuming = {FactClass::Accounting};
            contract.description = "unclassified adjacent authority";
            break;
    }
    contract.may_assert_classification = false;
    return contract;
}

BoundaryVerdict check_fact_ownership(FactClass fact) {
    if (is_ledger_owned(fact)) {
        return BoundaryVerdict{true, ReasonCode::Ok,
                               "the ledger owns " + std::string(to_string(fact))};
    }
    return BoundaryVerdict{false, ReasonCode::AuthorityNotOwned,
                           "the ledger does not own " + std::string(to_string(fact)) +
                               "; it belongs to an adjacent authority"};
}

BoundaryVerdict check_evidence_use(const Contract& contract, FactClass fact) {
    if (!is_ledger_owned(fact)) {
        return BoundaryVerdict{false, ReasonCode::AuthorityNotOwned,
                               "refusing to author " + std::string(to_string(fact)) +
                                   " because the ledger does not own that fact class"};
    }
    if (fact == FactClass::Classification && !contract.may_assert_classification) {
        return BoundaryVerdict{
            true, ReasonCode::Ok,
            "classification is authored by the ledger and attributed to " + contract.id.str() +
                "; the adjacent authority supplies evidence only"};
    }
    return BoundaryVerdict{true, ReasonCode::Ok,
                           "evidence from " + contract.id.str() + " may be used to author " +
                               std::string(to_string(fact))};
}

EvidenceEnvelope evaluate(const Observation& observation, const Instant& reference) {
    EvidenceEnvelope envelope;
    envelope.authority = observation.authority;
    envelope.role = observation.role;
    envelope.digest = observation.digest;
    envelope.subject = observation.subject;
    envelope.fact = observation.fact;
    envelope.observed_at = observation.observed_at;
    envelope.valid_until = observation.valid_until;

    if (!observation.reachable) {
        envelope.disposition = Disposition::Unavailable;
        envelope.code = ReasonCode::AuthorityUnavailable;
        envelope.detail = observation.detail.empty()
                              ? "the adjacent authority did not respond"
                              : observation.detail;
        return envelope;
    }
    if (!observation.supported) {
        envelope.disposition = Disposition::Unsupported;
        envelope.code = ReasonCode::Unsupported;
        envelope.detail = observation.detail.empty()
                              ? "the adjacent authority does not support this fact"
                              : observation.detail;
        return envelope;
    }
    if (observation.conflicting) {
        envelope.disposition = Disposition::Conflicting;
        envelope.code = ReasonCode::AuthorityConflicting;
        envelope.detail = observation.detail.empty()
                              ? "the adjacent authority reports a conflicting value"
                              : observation.detail;
        return envelope;
    }
    if (observation.digest.is_zero()) {
        envelope.disposition = Disposition::Refused;
        envelope.code = ReasonCode::InvalidArgument;
        envelope.detail = "evidence without a digest cannot be attributed and is refused";
        return envelope;
    }
    const bool no_expiry =
        observation.valid_until.seconds() == 0 && observation.valid_until.nanos() == 0;
    if (!no_expiry && observation.valid_until < reference) {
        envelope.disposition = Disposition::Stale;
        envelope.code = ReasonCode::AuthorityStale;
        envelope.detail = "evidence expired at " + observation.valid_until.to_iso8601();
        return envelope;
    }
    if (observation.observed_at > reference) {
        envelope.disposition = Disposition::Refused;
        envelope.code = ReasonCode::Refused;
        envelope.detail = "evidence is dated after the reference instant";
        return envelope;
    }
    envelope.disposition = Disposition::Accepted;
    envelope.code = ReasonCode::Ok;
    envelope.detail = observation.detail.empty() ? "evidence accepted as an input to ledger facts"
                                                 : observation.detail;
    return envelope;
}

std::string render_boundary_table() {
    std::string out;
    for (const RoleInfo& role : kRoles) {
        out.append("  ");
        out.append(role.name);
        out.append(":");
        out.append(role.description);
        out.push_back('\n');
    }
    out.append("\nowned fact classes: ");
    bool first = true;
    for (const FactInfo& info : kFacts) {
        if (!info.ledger_owned) {
            continue;
        }
        if (!first) {
            out.append(", ");
        }
        first = false;
        out.append(info.name);
    }
    out.push_back('\n');
    out.append("not owned: ");
    first = true;
    for (const FactInfo& info : kFacts) {
        if (info.ledger_owned) {
            continue;
        }
        if (!first) {
            out.append(", ");
        }
        first = false;
        out.append(info.name);
    }
    out.push_back('\n');
    return out;
}

}  // namespace fel::authority
