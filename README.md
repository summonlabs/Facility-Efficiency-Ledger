# Facility Efficiency Ledger

Facility Efficiency Ledger is a Data Center Control Plane (DCCP) runtime that keeps
a durable, auditable, deterministic account of what a facility's physical resource
consumption actually did.

It answers one question, for a bounded interval and an exact set of source
generations:

> How much physical resource consumption produced useful service, how much was
> avoidable, stranded, or wasted, how much remains unknown or unmeasured, and why?

Every answer is an exact rational accounting closure over the committed evidence,
published with the reason codes, the record digests, the source generations, and
the freshness of each generation that produced it.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

## What this repository is

* **A ledger, not a controller.** It records, classifies, attributes, reconciles,
  and explains. It does not switch anything, schedule anything, or price anything.
* **Exact.** Quantities are exact rationals with 64-bit numerators and
  denominators and checked 128-bit intermediates. There is no floating point
  anywhere in the accounting path, so a closure is either exact or explicitly
  reported as not closed. Nothing is rounded into agreement.
* **Durable.** The journal is an append-only, checksummed, hash-chained record
  stream behind a kernel-enforced single-writer lock, with an exact commit point
  and an exact recovery rule for torn tails.
* **Explainable.** A reconciliation report names every contributing record digest,
  every source generation it depended on, and every defect that stopped it from
  closing. A report has a canonical JSON rendering and a content digest.
* **Bounded.** It does not own capacity, placement, scheduling, pricing, policy,
  or actuation, and it refuses to treat another runtime's evidence as its own
  conclusion.

## Systems boundary

### This repository owns

* durable accounting of facility resource consumption across an accounting
  interval;
* classification of consumption into useful, avoidable, stranded, wasted,
  unknown, and unmeasured;
* attribution of measured consumption to subjects (facility, zone, hall, row,
  rack, device, tenant, workload);
* reconciliation and conservation checking, including the explainable residual;
* historical generations, revisions, corrections, and sealed intervals;
* the evidence and freshness model those conclusions rest on.

### This repository does not own

* energy delivery, thermal control, or any plant actuation;
* capacity, placement, scheduling, or admission;
* pricing, cost allocation, billing, or market settlement;
* policy, tenancy, or lifecycle decisions;
* metering, sensor acquisition, or telemetry transport.

Those belong to adjacent authorities. The ledger accepts their published evidence
as evidence and never infers authority from visibility. See
[docs/AUTHORITY-BOUNDARY.md](docs/AUTHORITY-BOUNDARY.md).

## The core question, precisely

For an interval `[start, end)` and the committed record set at one revision:

```
input_total   = sum of live measurements fully inside the interval
              + sum of live unbound quantified residuals
classified    = useful + avoidable + stranded + wasted + unknown + unmeasured
unclassified  = input_total - classified
```

The interval **closes** exactly when `unclassified` is zero for every physical
dimension in scope, no classification exceeds the measurement it draws from, no
evidence interval crosses the accounting boundary, no conflicting or duplicated
evidence is present, and every residual in scope is quantified.

Anything else is reported. A non-zero remainder is never absorbed into waste or
useful work, and an explicitly unquantified residual makes the dimension
`NotQuantified` rather than quietly closed.

## Domain model

### Service classes

| Class | Meaning |
| --- | --- |
| `useful` | consumption that produced service |
| `avoidable` | consumption an available better action could have avoided |
| `stranded` | capacity held but not usable for service in the interval |
| `wasted` | consumption with no service and no avoidable-action framing |
| `unknown` | consumption whose service classification is not established |
| `unmeasured` | consumption known to exist that no evidence quantifies |

`unknown` and `unmeasured` are terminal outcomes, not gaps to be filled in later
by assumption.

### Residuals

A residual is an explicit, attributed, evidenced statement.

* A **bound** residual accounts for part of one measured entry. It is counted
  against that entry, because the measurement already counted the resource.
* An **unbound** residual declares resource that no measurement captured. It
  enters the input side and is classified by the declaration itself.
* An unquantified residual must be class `unmeasured`, must be unbound, and must
  carry no amount. It makes closure indeterminate instead of silently zero.

### Generations, revisions, and epochs

* A **generation** is a source's evidence version. It advances only forward, and a
  measurement must cite the source's current generation.
* A **revision** advances by exactly one per mutating record. It is the ledger's
  accounting identity, and it is the optimistic-concurrency token: a command that
  cites a revision that has moved is refused rather than applied.
* An **epoch** identifies a writer session. Every writer open adopts the next
  epoch and records the adoption durably. A command that cites a stale epoch is
  refused.

### Corrections

A correction is a new record, never an edit. It names the record it supersedes,
carries that record's digest, and states a reason. If the digest does not match
the live record, the correction is refused. The superseded record stays in the
journal; the fold reports both, and compaction may retire the superseded payload
while the correction keeps the audit trail.

### Sealing

An interval can be sealed only when it reconciles exactly. The seal records the
report digest computed from the accounting content. A later change does not erase
the seal; it makes the seal stale, and every subsequent report says so.

## Evidence, authority, and freshness

Evidence is a digest plus the statement it supports. Authority is who may assert
what.

* The ledger's own conclusions are always authored by the ledger and attributed to
  the caller's request identity. An adjacent authority supplies evidence, never a
  classification.
* `FactClass` separates what this runtime owns (accounting, classification,
  attribution, reconciliation, residual, generation, correction, seal) from what it
  refuses to own (capacity, placement, scheduling, pricing, policy, actuation,
  energy delivery, thermal control).
* Evidence from an adjacent authority is classified into exactly one disposition:
  `accepted`, `stale`, `conflicting`, `unsupported`, `refused`, or
  `unavailable`. None of them is silently treated as zero.

Freshness has five states: `unattested`, `current`, `expired`, `recovered`,
and `retired`.

Recovered evidence is never promoted to current. If a session begins by
discarding an uncommitted record tail, a recovery barrier is recorded, every
generation attested before that barrier becomes `recovered`, and a reconciliation
on the current basis is refused with `RecoveredNotCurrent`. The same interval can
still be reconciled on the historical basis, labelled as recovered, and it becomes
current again only after a fresh `GenerationAttested` record.

## Architecture

```
include/fel/            public headers
  status.hpp            reason codes, Status, Result
  strong.hpp  ids.hpp   validated identifiers, checked counters
  rational.hpp          exact rationals with checked 128-bit intermediates
  unit.hpp  quantity.hpp  dimensions, exact conversions, quantities
  time.hpp              exact instants and half-open intervals
  digest.hpp            in-tree SHA-256 and CRC32C
  codec.hpp             canonical length-bounded binary encoding
  journal.hpp           record model and framing
  store.hpp             durable append-only store, recovery, compaction
  platform.hpp          kernel lock, durable flush, atomic replace
  view.hpp              deterministic journal fold
  reconcile.hpp         conservation engine
  report.hpp            canonical JSON rendering
  ledger.hpp            command surface
  authority.hpp         adjacent authority boundary
src/                    implementations
apps/fel_main.cpp       the fel command line tool
tests/                  the test suite, including real child processes
benchmarks/             the benchmark driver
examples/consumer/      an independent out-of-tree find_package consumer
```

The layering is strict and one-directional:

```
platform  ->  store  ->  ledger  ->  { view, reconcile, report }
codec, digest, rational, unit, time  ->  everything above
authority  ->  consumes ledger types, owns no ledger state
```

Every state transition lives in exactly one place. `LedgerView::apply` is used both
by a full replay of the journal and by the live index inside a running ledger, so
replay and live state cannot drift apart.

```
                     fel (CLI)
                         |
        Ledger  (thread-safe command surface, shared/exclusive locking)
          |                   |                        |
       Store              LedgerView               reconcile
  (durable journal)     (deterministic fold)    (conservation engine)
          |                                            |
      platform                                   report (canonical JSON)
```

## Persistence, recovery, and concurrency

A ledger is a directory:

```
fel.manifest              atomically published pointer to the live files
fel.lock                  kernel-locked single-writer lock file
fel-segment-NNNNNNNN.fel  append-only record stream
fel-snapshot-NNNNNNNN.fel compacted retained record set
```

### Record framing and the commit point

Each record is framed as a 48-byte little-endian header, a canonical payload, and a
32-byte chain digest:

```
magic | format version | record type | flags | sequence | revision | epoch
      | payload length  | payload CRC32C | header CRC32C
payload
SHA-256(previous chain digest || header || payload)
```

**The exact commit point for a record** is the moment the complete header, payload,
and chain trailer are on disk *and* the append has been flushed with a durable
write barrier. Anything shorter is not committed.

### Recovery

On open, the segment is walked record by record. Each record is accepted only if
its header checksum matches, its payload checksum matches, and its chain digest
links to its predecessor.

* A trailing region that is shorter than a complete record, or that is entirely
  zero, or that is a prefix of a record whose declared length runs past the end of
  the file, is an **uncommitted torn tail**. A writer truncates it to the last
  committed boundary and records that it recovered; a reader observes it without
  modifying anything.
* A complete record whose checksum or chain link does not match, or a sequence or
  revision discontinuity, is **interior corruption**. The ledger refuses to open
  and leaves the bytes untouched.

Recovery is conservative by construction: it can discard bytes that were never
committed, and it can never discard or repair bytes that were.

### Compaction

Compaction writes a snapshot of the retained record set, re-anchors the chain in a
fresh segment, and then **atomically replaces the manifest**. The manifest
replacement is the commit point of a compaction. Files written before it but never
named by it are uncommitted and are collected as garbage on the next writer open.
Records that a retained correction supersedes may be dropped; the correction keeps
the superseded identifier, digest, and reason, and the snapshot records a digest
over everything that was dropped.

### Concurrency

* **Across processes:** the kernel lock. A second writer, and any reader, is
  refused with `LockHeldExclusive` while a writer holds the ledger.
* **Within a process:** `Ledger` is thread-safe. Every public method takes exactly
  one lock and never takes another. Read paths take a shared lock; commands and
  compaction take an exclusive lock. No lock is upgraded, no lock is held across a
  callback, and internal helpers never take a lock of their own. Sealing is the
  interesting case: it holds the exclusive lock and needs a reconciliation
  internally, so it calls the unlocked reconciliation rather than re-entering the
  public one. [docs/CONCURRENCY.md](docs/CONCURRENCY.md) documents the full audit.

## Building

Requirements: CMake 3.25 or newer, a C++20 compiler, and Ninja or another
generator. There are no third-party dependencies and no network access is needed.

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Options: `FEL_BUILD_CLI`, `FEL_BUILD_TESTS`, `FEL_BUILD_BENCHMARKS`,
`FEL_WARNINGS_AS_ERRORS`, `FEL_ENABLE_ASAN`, `FEL_ENABLE_UBSAN`.

On MSVC the library, the CLI, the tests, and the benchmarks are built with
`/W4 /permissive- /WX`; elsewhere with
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.
First-party code is warning-free in both Release and Debug.

## Using the library

```cpp
#include <fel/fel.hpp>

using namespace fel;

Result<Ledger> created =
    Ledger::create("/var/lib/fel/facility-01", LedgerId::parse("facility-01").value(), "my-service");
Ledger& ledger = created.value();

RegisterSourceRequest source;
source.source = SourceId::parse("meter-hall-a").value();
source.kind = SourceKind::Meter;
source.unit = Unit::KilowattHour;
source.context.now = Instant::parse_iso8601("2026-01-01T00:00:00Z").value();
ledger.register_source(source);

PublishGenerationRequest generation;
generation.source = source.source;
generation.generation = Generation{1};
generation.evidence = Sha256::hash("meter-hall-a export 2026-01-01T00:00Z");
generation.context.now = source.context.now;
ledger.publish_generation(generation);

RecordMeasurementRequest measurement;
measurement.entry = EntryId::parse("e-0001").value();
measurement.interval = Interval::make(Instant::parse_iso8601("2026-01-01T00:00:00Z").value(),
                                      Instant::parse_iso8601("2026-01-01T01:00:00Z").value()).value();
measurement.source = source.source;
measurement.generation = Generation{1};
measurement.quantity = Quantity{Rational{100}, Unit::KilowattHour};
measurement.evidence = Sha256::hash("interval register");
measurement.context.now = Instant::parse_iso8601("2026-01-01T01:05:00Z").value();
ledger.record_measurement(measurement);

RecordClassificationRequest useful;
useful.allocation = AllocationId::parse("a-0001").value();
useful.entry = measurement.entry;
useful.klass = ServiceClass::Useful;
useful.quantity = Quantity{Rational{80}, Unit::KilowattHour};
useful.evidence = Sha256::hash("workload accounting export");
useful.context.now = measurement.context.now;
ledger.record_classification(useful);

// The remaining 20 kWh has no classification decision, so the interval does not
// close, and the report says exactly that.
ReconcileRequest request;
request.interval = measurement.interval;
Result<ReconciliationReport> report = ledger.reconcile(request);
// report.value().closed          == false
// report.value().code            == ReasonCode::UnclassifiedRemainder
// report.value().dimensions[0].unclassified == 72000000 J
```

Every command returns a `CommandOutcome` carrying the record type, revision,
sequence, epoch, record digest, and chain head. Every failure returns a stable
`ReasonCode` plus a detail string.

### Idempotent retries

Give a command an `IdempotencyKey`. The key and a digest of the request are
committed in the same record as the mutation, so a retry with the same key and the
same request replays the original outcome instead of applying it twice, and a
retry with the same key and a different request is refused with
`IdempotencyConflict`.

## Command line

```
fel init <dir> --ledger <id>
fel verify <dir>
fel status <dir>
fel export <dir>
fel compact <dir>

fel source add <dir> --id --kind --unit [--authority --authority-role --label]
fel source retire <dir> --id --reason
fel generation publish <dir> --source --generation --evidence [--valid-until --method]
fel generation attest  <dir> --source --generation --evidence [--valid-until --method]
fel measure  <dir> --entry --source --generation --start --end --amount --unit --evidence
fel classify <dir> --allocation --entry --class --amount --unit --evidence
fel residual <dir> --residual --start --end --class --basis --evidence [--amount]
fel void     <dir> --void-id --target-kind --target --target-digest --reason
fel reconcile <dir> --start --end [--basis current|historical] [--as-of]
fel seal      <dir> --seal --start --end [--note]
fel boundary [--role <role>] [--fact <fact>]
```

Common options: `--json` for canonical JSON, `--now <ISO-8601 instant>` (required
by mutating commands so runs are reproducible), `--idempotency <key>`,
`--expect-revision <n>`, `--expect-epoch <n>`.

Exit status is 0 for success and for a closed reconciliation, 1 for an explicit
refusal or an unclosed reconciliation, and 2 for a usage error.

```
$ fel init /var/lib/fel/facility-01 --ledger facility-01
initialized ledger facility-01 at /var/lib/fel/facility-01 (epoch 1)

$ fel reconcile /var/lib/fel/facility-01 --start 2026-01-01T00:00:00Z --end 2026-01-01T01:00:00Z
reconcile OPEN code=UnclassifiedRemainder revision=4 digest=1a2b3c4d5e6f7081
  energy: input=360000000 J useful=288000000 J avoidable=0 J stranded=0 J wasted=0 J unknown=0 J unmeasured=0 J unclassified=72000000 J -> 72000000 J of measured input has no classification decision
```

## Installing and consuming from another project

```
cmake --install build/release --prefix /opt/fel
```

This installs the static library, the public headers, the `fel` tool, the CMake
package `FacilityEfficiencyLedger`, and the documentation files. A downstream
project then does:

```cmake
find_package(FacilityEfficiencyLedger 1.0 REQUIRED CONFIG)
target_link_libraries(my_service PRIVATE FacilityEfficiencyLedger::fel)
```

[examples/consumer](examples/consumer) is a complete, independent consumer that is
deliberately not part of this repository's build. It is configured, built, and run
against an install prefix only. See
[docs/VALIDATION.md](docs/VALIDATION.md) for the exact recorded procedure and
output.

## Validation

The suite contains **102 tests** in nine categories. It is not a smoke test: it
kills processes, corrupts bytes, and races threads on purpose.

| Category | What it proves |
| --- | --- |
| Exact arithmetic and units | exact rational arithmetic, overflow refusal at the 64-bit boundary, exact unit conversion, decimal and binary data units kept distinct |
| Time and intervals | ISO-8601 round trips, leap-year boundaries, half-open interval semantics, nanosecond-exact durations |
| Encoding and digests | canonical codec round trips for every record type, bounded decoding of hostile lengths, published SHA-256 vectors, CRC32C reference vector |
| Journal framing | every record type round trips; every single-byte change in a frame header is detected; chain digests bind both predecessor and payload |
| Durable store | commit and reopen, torn-tail discard, interior-corruption refusal, kernel lock exclusion, compaction, orphan collection, integrity verification |
| Fold and freshness | incremental and full replay agree, supersession and voiding, conflict detection, all five freshness states |
| Conservation | exact closure, mixed units, residuals, unquantified residuals, boundary crossing, per-dimension independence, current vs historical basis, report determinism |
| Commands and lifecycle | idempotent replay and conflict, stale revision and epoch refusal, corrections with digest checking, voiding, sealing, a full create-to-compaction cycle across separate handles |
| Adversarial, concurrency, and restart | exhaustive truncation, exhaustive single-bit flips, record removal and reordering, corrupt snapshots, real multiprocess lock contention, real abrupt process death, real multithreaded writers and readers |

Recorded results, from the machine described below:

| Configuration | Result |
| --- | --- |
| Release (`/W4 /WX`) | 102 passed, 0 failed |
| Debug (`/W4 /WX`) | 102 passed, 0 failed, zero first-party warnings |
| RelWithDebInfo + AddressSanitizer | 102 passed, 0 failed, no sanitizer diagnostics, no leaks reported |

Specific adversarial claims that the suite proves rather than asserts:

* **Every truncation of the segment is recovered exactly.** For every length from
  zero to the full segment size, the ledger either refuses because not one whole
  record survived, or opens with exactly the whole records inside that prefix,
  reports the discarded tail byte count, and leaves the journal at a committed
  boundary that a subsequent verification confirms is free of torn tails.
* **Every single-bit flip is detected.** For every byte offset of a committed
  segment, flipping one bit causes the open to be refused. There is no offset at
  which a corrupted ledger silently opens with different state.
* **A removed or reordered record is refused.** Splicing a record out, or swapping
  two records, breaks the chain and is never repaired.
* **An uncommitted compaction is ignored.** Snapshot and segment files that exist
  but are not named by the manifest are ignored, and their presence never changes
  the accounting revision. They are collected as garbage.
* **The kernel lock is real.** A child process holding the write lock makes both a
  second writer and any reader fail with `LockHeldExclusive`; when that process is
  killed without releasing anything, the next open succeeds.
* **Committed work survives abrupt death.** A child that registers sources and then
  terminates without unwinding leaves every committed record durable and
  verifiable by the parent.
* **A genuinely torn write is recoverable, and recovered evidence is not current.**
  A killed writer leaves a valid record header followed by part of its payload; the
  next session truncates it, records the recovery barrier, and refuses to assert a
  current reconciliation until the affected generations are re-attested.

Reproduce with:

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

## Benchmarks

`fel_bench` measures completed work only. Every number is a measurement of this
repository's own binaries running on this machine, not an estimate.

Methodology:

* each timing is a median over repeated runs of the same workload;
* timing uses a monotonic clock and covers the whole operation, including durable
  flushes for anything that writes;
* the facility data is generated by the driver (labelled `SYNTHETIC`) because no
  real facility telemetry exists here;
* the runtime, the storage, the locking, and the arithmetic are real
  (`REAL`), and no measurements are reported for instrumentation that does not
  exist (`UNSUPPORTED`).

Measured on AMD Ryzen 7 9800X3D (8 cores, 16 threads), Windows 11 Pro build
26300, MSVC 19.44, Release build, `--scale=1` (2 sources x 40 measurements):

| Measurement | Label | Unit | Value |
| --- | --- | --- | --- |
| Exact rational operations (add + multiply) | REAL | ops/s | 718,049 |
| In-tree SHA-256 | REAL | MiB/s | 227.9 |
| Durable commit throughput (validate, frame, append, flush, re-index) | REAL | records/s | 660 |
| Durable commit latency | REAL | us/record | 1,514 |
| Journal fold throughput (full deterministic replay) | REAL | records/s | 446,643 |
| Reconciliation latency (fully classified interval) | REAL | ms/report | 1.01 |
| Restart recovery (reopen, chain verify, tail recovery, epoch adoption) | REAL | ms | 7.50 |
| Compaction (snapshot publication plus segment re-anchoring) | REAL | ms | 10.4 |

The commit number is dominated by the durable flush and is the honest one to
quote for ingestion planning: this ledger commits about 660 records per second on
this machine when every record is flushed to disk before it is acknowledged. Reads
and replays are two to three orders of magnitude cheaper.

Run it yourself:

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFEL_BUILD_BENCHMARKS=ON
cmake --build build/release
build/release/bin/fel_bench --scale=2
```

## REAL, SYNTHETIC, and UNSUPPORTED

**REAL.** The library, the command line tool, the durable journal, the kernel file
lock, the recovery rules, the compaction publication, the exact arithmetic, the
conservation engine, the canonical reports, the CMake package, the install, and the
out-of-tree consumer are all real and are exercised by the tests and the
benchmarks on this machine.

**SYNTHETIC.** The facility itself is synthetic. There is no meter, no PDU, no
UPS, no BMS, no DCIM, and no real site. Every measured quantity in the tests and
benchmarks is generated locally. Adjacent runtimes (a data centre control plane,
an asset inventory, a data fabric authority, a building management system) are
represented by typed contracts and explicit evidence dispositions; none of them is
contacted, and no integration with any of them is claimed.

**UNSUPPORTED.** No claim is made about building management or DCIM telemetry
throughput, multi-node cluster behaviour, electrical or cooling measurements, or
any real facility efficiency outcome. There is no hardware here to prove them with,
so they are reported as `UNSUPPORTED` rather than estimated.

## Limitations

* **One writer per ledger directory.** The kernel lock is exclusive across
  processes, and a reader is refused while a writer holds it. A reader that wants a
  consistent view waits rather than reading a moving target.
* **Recovery is conservative, not clairvoyant.** An uncommitted tail is discarded.
  If a record's bytes are all present but its checksums do not match, the ledger
  refuses to open instead of guessing which half was intended.
* **Compaction is irreversible.** Superseded payloads dropped by compaction do not
  come back; the corrections that retired them, their identifiers, their digests,
  and a digest over everything dropped are preserved.
* **Whole-interval evidence only.** A measurement that crosses the accounting
  boundary is reported, not split. Splitting is a re-measurement decision, not an
  accounting assumption.
* **In-memory working set.** Both the full fold and the live index hold one entry
  per live record. The benchmarks cover tens to thousands of records; a facility
  with millions of live measurements in a single interval is beyond what has been
  measured here.
* **No real facility has been instrumented.** Every quantity in this repository
  came from a synthetic generator. The accounting is exact; the data is not real.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
