#pragma once

// Typed boundary between this ledger and adjacent control-plane authorities.
//
// The ledger owns accounting, classification, attribution, reconciliation,
// historical generations, and explainable residuals. It does not own energy
// delivery, capacity, pricing, scheduling, placement, policy, or actuation.
// Visible evidence from an adjacent runtime is exactly that: evidence. It never
// transfers that runtime's authority, and it never becomes a ledger conclusion
// without an explicit, attributed, FEL-owned record.
//
// Evidence from an adjacent authority is classified into one of six explicit
// dispositions. Unsupported, conflicting, refused, and unavailable evidence is
// named rather than silently dropped or treated as zero.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fel/digest.hpp"
#include "fel/ids.hpp"
#include "fel/journal.hpp"
#include "fel/status.hpp"
#include "fel/time.hpp"

namespace fel::authority {

// Adjacent runtimes this ledger is designed to compose with. A role is a
// declaration of what the evidence describes, not a claim that the runtime is
// reachable.
enum class Role : std::uint8_t {
    DataCenterControlPlane = 1,
    AssetStateInventory = 2,
    DataFabricIntelligence = 3,
    BuildingManagement = 4,
    Telemetry = 5,
    External = 6,
};

std::string_view to_string(Role role) noexcept;
std::optional<Role> role_from_string(std::string_view name) noexcept;

// Disposition of evidence offered by an adjacent authority.
enum class Disposition : std::uint8_t {
    Accepted = 0,
    Stale = 1,
    Conflicting = 2,
    Unsupported = 3,
    Refused = 4,
    Unavailable = 5,
};

std::string_view to_string(Disposition disposition) noexcept;

// Fact classes. The ledger owns the first set and refuses to own the second.
enum class FactClass : std::uint8_t {
    Accounting = 1,
    Classification = 2,
    Attribution = 3,
    Reconciliation = 4,
    Residual = 5,
    Generation = 6,
    Correction = 7,
    Seal = 8,
    Capacity = 9,
    Placement = 10,
    Scheduling = 11,
    Pricing = 12,
    Policy = 13,
    Actuation = 14,
    EnergyDelivery = 15,
    ThermalControl = 16,
};

std::string_view to_string(FactClass fact) noexcept;
std::optional<FactClass> fact_from_string(std::string_view name) noexcept;
bool is_ledger_owned(FactClass fact) noexcept;

struct Contract {
    AuthorityId id{};
    Role role{Role::External};
    std::vector<FactClass> authoritative_for{};
    std::vector<FactClass> consuming{};
    bool may_assert_classification{false};
    std::string description{};
};

// The contract this ledger assumes for each adjacent role.
Result<Contract> default_contract(Role role, const AuthorityId& id);

struct BoundaryVerdict {
    bool permitted{false};
    ReasonCode code{ReasonCode::Ok};
    std::string detail{};
};

// Answers whether this runtime may treat a fact class as its own.
BoundaryVerdict check_fact_ownership(FactClass fact);

// Answers whether evidence from a role may be used to author a ledger fact.
BoundaryVerdict check_evidence_use(const Contract& contract, FactClass fact);

struct Observation {
    AuthorityId authority{};
    Role role{Role::External};
    Digest digest{};
    Instant observed_at{};
    Instant valid_until{};
    std::string subject{};
    std::string fact{};
    bool supported{true};
    bool reachable{true};
    bool conflicting{false};
    std::string detail{};
};

struct EvidenceEnvelope {
    AuthorityId authority{};
    Role role{Role::External};
    Digest digest{};
    std::string subject{};
    std::string fact{};
    Instant observed_at{};
    Instant valid_until{};
    Disposition disposition{Disposition::Unavailable};
    ReasonCode code{ReasonCode::Ok};
    std::string detail{};
};

// Classifies an observation at a reference instant. Never promotes stale or
// recovered evidence to current.
EvidenceEnvelope evaluate(const Observation& observation, const Instant& reference);

// Canonical rendering of the boundary table for documentation and CLI output.
std::string render_boundary_table();

}  // namespace fel::authority
