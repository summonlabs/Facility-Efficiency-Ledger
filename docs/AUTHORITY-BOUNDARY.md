# Authority boundary

## Principle

Observation is not ownership. Evidence visible to this runtime is evidence, not
authority, and it never becomes a ledger conclusion without an explicit,
attributed, ledger-authored record.

The runtime therefore separates two questions that are easy to conflate:

* **What is this evidence?** A digest, a subject, an observation instant, a validity
  window, and a disposition.
* **Who may assert this fact?** A fact class owned by exactly one authority.

## Owned and not owned

| Owned by this repository | Owned by an adjacent authority |
| --- | --- |
| `accounting` | `capacity` |
| `classification` | `placement` |
| `attribution` | `scheduling` |
| `reconciliation` | `pricing` |
| `residual` | `policy` |
| `generation` | `actuation` |
| `correction` | `energy_delivery` |
| `seal` | `thermal_control` |

`check_fact_ownership` answers whether this runtime may treat a fact class as its
own, and returns `AuthorityNotOwned` with the offending class named for anything in
the right-hand column.

## Adjacent roles

Each adjacent runtime is described by a `Contract`: an identifier, a role, the fact
classes it is authoritative for, the fact classes it may inform, and a description
of the relationship.

| Role | Authoritative for | Description |
| --- | --- | --- |
| `dccp` | capacity, placement, scheduling | consumes ledger accounting as an input to its decisions; the ledger never accepts its capacity or scheduling claims as accounting facts |
| `asi` | attribution | authoritative for asset identity and lifecycle; ledger attribution may cite it, but the citation is evidence, not ownership |
| `dfi` | attribution | authoritative for fabric flow attribution; its claims are recorded as evidence with the authority named |
| `bms` | thermal control, energy delivery | authoritative for plant control and energy delivery; the ledger consumes measured quantities and issues no control action |
| `telemetry` | nothing | supplies raw measurement evidence only |
| `external` | nothing | an unclassified adjacent authority |

No contract may set `may_assert_classification`: classification is always authored
by this ledger.

## Evidence dispositions

Every observation is classified into exactly one disposition at a reference
instant. None of them is silently treated as zero, and none of them is silently
dropped.

| Disposition | Reason code | Condition |
| --- | --- | --- |
| `accepted` | `Ok` | supported, reachable, non-conflicting, digested, not future-dated, not expired |
| `stale` | `AuthorityStale` | the validity window ended before the reference instant |
| `conflicting` | `AuthorityConflicting` | the authority reports a conflicting value |
| `unsupported` | `Unsupported` | the authority does not support this fact |
| `refused` | `InvalidArgument` or `Refused` | untraceable (no digest) or dated after the reference instant |
| `unavailable` | `AuthorityUnavailable` | the authority did not respond |

## Current state of integration

**Synthetic.** No adjacent runtime is contacted. There is no network client in the
library or the command line tool, and no integration with any data centre control
plane, asset inventory, fabric authority, or building management system is claimed.
The contracts, the fact-class ownership rules, the boundary verdicts, and the
evidence dispositions are real, typed, and tested; the peers are not present.

**Real.** The enforcement is real. A ledger command that would need an adjacent
authority to assert an accounting fact is refused, and the CLI refuses a
`source add --authority-role` combination whose contract does not permit the use.

## Command line

```
$ fel boundary
  dccp: capacity, placement, and scheduling authority for the data centre
  ...
  owned fact classes: accounting, classification, attribution, reconciliation,
  residual, generation, correction, seal
  not owned: capacity, placement, scheduling, pricing, policy, actuation,
  energy_delivery, thermal_control

$ fel boundary --role dccp --fact capacity
refused: the ledger does not own capacity; it belongs to an adjacent authority

$ fel boundary --role dccp --fact accounting
permitted: evidence from authority-dccp may be used to author accounting
```
