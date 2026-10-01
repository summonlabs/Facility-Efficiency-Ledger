# Architecture

## Purpose

Facility Efficiency Ledger is a durable accounting runtime. It answers, for a
bounded interval and an exact set of source generations, how much physical
resource consumption produced useful service, how much was avoidable, stranded, or
wasted, how much remains unknown or unmeasured, and why.

## Layers

```
           +-------------------------------------------------------+
           |  authority   adjacent authority contracts and the     |
           |              evidence boundary (owns no ledger state) |
           +-------------------------------------------------------+
           |  ledger      command surface, validation, locking     |
           +-------------------------------------------------------+
           |  store       durable journal, recovery, compaction    |
           +-------------------------------------------------------+
           |  platform    kernel lock, durable flush, atomic       |
           |              replace, file I/O primitives             |
           +-------------------------------------------------------+
   view / reconcile / report are deterministic projections of the journal
   codec / digest / rational / unit / quantity / time are shared primitives
```

Dependencies point one way. `platform` knows nothing about records; `store` knows
records but nothing about accounting; `ledger` knows accounting and durability but
nothing about adjacent runtimes; `authority` knows the boundary rules and owns no
state.

## The journal is the database

There is exactly one authoritative store: an append-only sequence of framed
records. Everything else is either a durability mechanism (`store`, `platform`) or
a deterministic function of that sequence (`view`, `reconcile`, `report`).

This is a deliberate choice. It makes replay trivial, makes every conclusion
traceable to the record that produced it, makes corrections additive, and makes
recovery a well-defined single-file problem.

## One implementation of every state transition

`LedgerView::apply` is the only place where a record changes ledger state. A full
replay folds every committed record through it; the live index inside a running
ledger applies records through it one at a time. Replay and live state therefore
cannot drift apart, and the property test that compares them is checking a
structural guarantee rather than hoping for one.

`LedgerView::analyze` performs the checks that need to compare records with each
other: over-allocation, overlapping evidence from one source, duplicated
observations. `LedgerView::finalize` judges freshness at a reference instant.
Both are separate from `apply` because they are queries, not transitions.

## Accounting model

Each measurement enters the ledger as a positive quantity from a named source at a
named generation, over a half-open interval, resting on a named piece of evidence.

Each classification draws a quantity from exactly one measured entry and assigns
it to a service class. A bound residual also draws from exactly one entry; an
unbound residual declares input no measurement captured.

Per physical dimension, over the scope of one interval:

```
input_total  = measured_input + declared_input
classified   = useful + avoidable + stranded + wasted + unknown + unmeasured
unclassified = input_total - classified
```

The dimension closes exactly when `unclassified` is zero, every entry's
classification total is at most its measured quantity, no evidence crosses the
boundary, no conflicts are present, and every residual in scope is quantified.

Nothing is ever rounded, absorbed, or redistributed to make a closure happen.

## Revisions, epochs, and generations

* A generation belongs to a source and advances forward only.
* A revision belongs to the ledger and advances by exactly one per mutating record.
* An epoch belongs to a writer session and advances on every writer open.

They are separate types so they cannot be interchanged, and each has its own
staleness refusal. A command can cite an expected revision and an expected epoch;
either being stale refuses the command instead of applying it optimistically.

## Determinism

* No floating point in the accounting path.
* Records are encoded in a fixed, canonical order and hashed.
* Folds sort their outputs; reports sort their keys.
* Report digests cover the accounting content and deliberately exclude session
  bookkeeping (chain head, epoch) and the wall-clock generation instant, so a seal
  taken before a restart can still be re-verified afterwards.

Two runs over the same journal produce byte-identical canonical JSON.

## Freshness and recovery

A session that discards an uncommitted tail records a recovery barrier. Generations
attested before that barrier become `recovered`. Current-basis reconciliation is
refused with `RecoveredNotCurrent` until a fresh attestation moves them forward.
Historical-basis reconciliation still works and labels the evidence honestly.

## Error model

Every fallible entry point returns `Result<T>` or `Status` carrying a stable
`ReasonCode` and a detail string. Reason codes are part of the observable
contract: they are named in canonical JSON, they are never renumbered, and the
tests assert on them. Refusals are outcomes, not exceptions.

## Source layout

| Path | Contents |
| --- | --- |
| `include/fel/status.hpp` | reason codes, `Status`, `Result` |
| `include/fel/strong.hpp`, `ids.hpp` | validated identifiers, checked counters, concrete id types |
| `include/fel/rational.hpp` | exact rationals with checked 128-bit intermediates |
| `include/fel/unit.hpp`, `quantity.hpp` | dimensions, exact conversions, quantities |
| `include/fel/time.hpp` | exact instants, half-open intervals, civil calendar |
| `include/fel/digest.hpp` | in-tree SHA-256 and CRC32C |
| `include/fel/codec.hpp` | canonical length-bounded binary encoding |
| `include/fel/journal.hpp` | record model, payload encoding, framing |
| `include/fel/platform.hpp` | kernel lock, durable file, atomic replace |
| `include/fel/store.hpp` | durable store, recovery, compaction, integrity verification |
| `include/fel/view.hpp` | deterministic fold and live index |
| `include/fel/reconcile.hpp` | conservation engine and report types |
| `include/fel/report.hpp` | canonical JSON rendering |
| `include/fel/ledger.hpp` | command surface and locking |
| `include/fel/authority.hpp` | adjacent authority contracts and evidence dispositions |
| `apps/fel_main.cpp` | the `fel` command line tool |
