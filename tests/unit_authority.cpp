#include "fel/fel.hpp"
#include "harness.hpp"
#include "support.hpp"

#include <string>

using namespace fel;
using namespace feltest;

FEL_TEST(boundary, ledger_owns_accounting_and_refuses_control_facts) {
    using authority::FactClass;
    CHECK(authority::is_ledger_owned(FactClass::Accounting));
    CHECK(authority::is_ledger_owned(FactClass::Classification));
    CHECK(authority::is_ledger_owned(FactClass::Residual));
    CHECK(!authority::is_ledger_owned(FactClass::Capacity));
    CHECK(!authority::is_ledger_owned(FactClass::Placement));
    CHECK(!authority::is_ledger_owned(FactClass::Scheduling));
    CHECK(!authority::is_ledger_owned(FactClass::Pricing));
    CHECK(!authority::is_ledger_owned(FactClass::Policy));
    CHECK(!authority::is_ledger_owned(FactClass::Actuation));

    const authority::BoundaryVerdict owned = authority::check_fact_ownership(FactClass::Accounting);
    CHECK(owned.permitted);
    const authority::BoundaryVerdict refused = authority::check_fact_ownership(FactClass::Capacity);
    CHECK(!refused.permitted);
    CHECK(refused.code == ReasonCode::AuthorityNotOwned);
    CHECK(refused.detail.find("capacity") != std::string::npos);
}

FEL_TEST(boundary, contracts_describe_each_adjacent_role) {
    const AuthorityId id = feltest::id<AuthorityIdTag>("authority-dccp");
    for (authority::Role role :
         {authority::Role::DataCenterControlPlane, authority::Role::AssetStateInventory,
          authority::Role::DataFabricIntelligence, authority::Role::BuildingManagement,
          authority::Role::Telemetry, authority::Role::External}) {
        const Result<authority::Contract> contract = authority::default_contract(role, id);
        REQUIRE(contract.ok());
        CHECK(contract.value().id == id);
        CHECK(!contract.value().may_assert_classification);
        CHECK(!contract.value().description.empty());
    }
    CHECK(authority::default_contract(authority::Role::External, AuthorityId{}).code() ==
          ReasonCode::InvalidIdentifier);
    CHECK(!authority::role_from_string("nonexistent").has_value());
    CHECK(authority::role_from_string("dccp").value() == authority::Role::DataCenterControlPlane);
}

FEL_TEST(boundary, evidence_dispositions_are_explicit) {
    authority::Observation observation;
    observation.authority = feltest::id<AuthorityIdTag>("authority-dccp");
    observation.role = authority::Role::DataCenterControlPlane;
    observation.digest = feltest::dig("evidence");
    observation.observed_at = feltest::at("2026-01-01T00:00:00Z");
    observation.valid_until = feltest::at("2026-01-01T02:00:00Z");
    observation.subject = "rack-a1";
    observation.fact = "capacity";
    const Instant reference = feltest::at("2026-01-01T01:00:00Z");

    CHECK(authority::evaluate(observation, reference).disposition ==
          authority::Disposition::Accepted);

    authority::Observation stale = observation;
    stale.valid_until = feltest::at("2026-01-01T00:30:00Z");
    const authority::EvidenceEnvelope stale_envelope = authority::evaluate(stale, reference);
    CHECK(stale_envelope.disposition == authority::Disposition::Stale);
    CHECK(stale_envelope.code == ReasonCode::AuthorityStale);

    authority::Observation conflicting = observation;
    conflicting.conflicting = true;
    CHECK(authority::evaluate(conflicting, reference).disposition ==
          authority::Disposition::Conflicting);

    authority::Observation unsupported = observation;
    unsupported.supported = false;
    CHECK(authority::evaluate(unsupported, reference).disposition ==
          authority::Disposition::Unsupported);

    authority::Observation unavailable = observation;
    unavailable.reachable = false;
    CHECK(authority::evaluate(unavailable, reference).disposition ==
          authority::Disposition::Unavailable);

    authority::Observation undigested = observation;
    undigested.digest = Digest{};
    CHECK(authority::evaluate(undigested, reference).disposition ==
          authority::Disposition::Refused);

    authority::Observation future = observation;
    future.observed_at = feltest::at("2026-01-01T03:00:00Z");
    CHECK(authority::evaluate(future, reference).disposition == authority::Disposition::Refused);

    // Evidence without an expiry is accepted indefinitely, which is the
    // documented meaning of the zero instant.
    authority::Observation permanent = observation;
    permanent.valid_until = Instant{};
    CHECK(authority::evaluate(permanent, reference).disposition ==
          authority::Disposition::Accepted);
}

FEL_TEST(boundary, evidence_never_becomes_a_ledger_conclusion_by_itself) {
    const AuthorityId id = feltest::id<AuthorityIdTag>("authority-bms");
    const Result<authority::Contract> contract =
        authority::default_contract(authority::Role::BuildingManagement, id);
    REQUIRE(contract.ok());
    const authority::BoundaryVerdict accounting =
        authority::check_evidence_use(contract.value(), authority::FactClass::Accounting);
    CHECK(accounting.permitted);
    const authority::BoundaryVerdict classification =
        authority::check_evidence_use(contract.value(), authority::FactClass::Classification);
    CHECK(classification.permitted);
    CHECK(classification.detail.find("evidence only") != std::string::npos);
    const authority::BoundaryVerdict actuation =
        authority::check_evidence_use(contract.value(), authority::FactClass::Actuation);
    CHECK(!actuation.permitted);
    CHECK(actuation.code == ReasonCode::AuthorityNotOwned);

    const std::string table = authority::render_boundary_table();
    CHECK(table.find("dccp") != std::string::npos);
    CHECK(table.find("not owned:") != std::string::npos);
    CHECK(table.find("actuation") != std::string::npos);
}
