# Validation record

This document records what was actually built, run, and observed, and it
distinguishes real proof from synthetic input and from things that were not proven
at all.

## Environment

| Item | Value |
| --- | --- |
| Operating system | Windows 11 Pro, build 26300 |
| Processor | AMD Ryzen 7 9800X3D, 8 cores / 16 threads |
| Compiler | Microsoft C/C++ 19.44.35222 (Visual Studio 2022 Build Tools, x64) |
| Build system | CMake 4.3.2 with Ninja 1.13.2 |
| Standard | C++20, no compiler extensions |

## Configurations built

| Configuration | Flags | Result |
| --- | --- | --- |
| Release | `/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /WX` | clean build, zero first-party warnings |
| Debug | same, with `/Od /RTC1` | clean build, zero first-party warnings |
| RelWithDebInfo + ASan | `/fsanitize=address` | clean build, no sanitizer diagnostics |

The library, the CLI, the tests, and the benchmarks are all built with warnings as
errors. A warning would fail the build, so "clean build" is a stronger statement
than "no warnings printed".

## Test results

```
Release           102 passed, 0 failed, 0 filtered out of 102 tests
Debug             102 passed, 0 failed, 0 filtered out of 102 tests
ASan (RelWithDeb) 102 passed, 0 failed, 0 filtered out of 102 tests
```

The suite is a single self-contained executable with no third-party test framework
and no timeouts. It spawns real child processes and kills them mid-append, holds the
kernel lock in one process and proves another is refused, rewrites ledger bytes at
every offset, runs eight writer threads and four reader threads concurrently, and
deletes and reorders records.

## Adversarial coverage in detail

| Attack | Expectation | Covered by |
| --- | --- | --- |
| Truncate the segment at every byte length | open with exactly the whole records in the prefix, discard the tail, or refuse when nothing survived | `adversarial.every_truncation_of_the_segment_is_recovered_exactly` |
| Flip one bit at every byte offset | refuse to open | `adversarial.every_single_bit_flip_is_detected` |
| Flip one bit at every seventh byte of a snapshot | refuse to open | `adversarial.a_corrupt_snapshot_is_refused_rather_than_half_read` |
| Remove a record | chain broken, refuse | `adversarial.a_removed_or_reordered_record_breaks_the_chain` |
| Reorder two records | chain broken, refuse | same |
| Corrupt the manifest | `ManifestCorrupt` | `store.verify_refuses_a_corrupt_manifest` |
| Truncate the snapshot | `SnapshotCorrupt` | `adversarial.a_corrupt_snapshot_is_refused_rather_than_half_read` |
| Delete the segment | `SegmentMissing` | `adversarial.missing_files_are_reported_precisely` |
| Delete the manifest | `ManifestMissing` | same |
| Empty the segment | `InteriorCorruption` | `adversarial.an_empty_segment_is_refused` |
| Leave an uncommitted compaction behind | ignore it, keep the revision, collect the files | `adversarial.an_uncommitted_snapshot_is_ignored_and_collected` |
| Kill a writer mid-append | recover, refuse the current basis, allow the historical basis, allow re-attestation | `restart.a_genuinely_killed_writer_leaves_a_recoverable_torn_tail` |
| Kill a writer after commits | every committed record is durable | `multiprocess.work_committed_before_an_abrupt_exit_is_durable` |
| Open a second writer while one holds the lock | `LockHeldExclusive` | `multiprocess.the_write_lock_is_enforced_by_the_kernel_across_processes` |
| Read while a writer holds the lock | `LockHeldExclusive` | same |
| Kill a lock holder | the next open succeeds | `multiprocess.an_abandoned_write_lock_does_not_survive_process_death` |
| Eight writers, twenty-five commands each | exactly 25 x 8 mutations, no losses, no duplicates | `concurrency.concurrent_commands_are_serialised_exactly_once` |
| Four readers while writers run | monotone revisions, internally consistent snapshots | `concurrency.readers_observe_consistent_snapshots_while_writers_run` |
| Seal while readers reconcile | no self-deadlock, seal recorded | `concurrency.sealing_while_readers_run_takes_locks_in_one_direction` |
| Compact while readers fold | no partially applied compaction | `concurrency.compaction_is_exclusive_and_leaves_the_ledger_usable` |

## Accounting coverage in detail

* Exact closure across two dimensions, with one closed and one open.
* Mixed units inside one dimension (kWh measured, joules classified) reconciling
  exactly.
* Unclassified remainder reported rather than absorbed.
* Over-allocation refused at both the command boundary and in the fold.
* Bound residuals classifying measured input; unbound residuals declaring input no
  meter captured.
* Unquantified residuals making closure `NotQuantified` rather than closed.
* Evidence crossing the accounting boundary reported as `IntervalNotContained`.
* Current-basis reconciliation refused while evidence is recovered; the historical
  basis allowed; re-attestation restoring currency.
* Twenty-four seeded random ledgers, each verified to close exactly, with the
  classification totals recomputed independently.
* Twelve seeded random correction chains, each leaving exactly one live entry and
  no double counting.
* Report digests stable across repeated runs and across process boundaries.

## Defects found and fixed during validation

These were found by the tests and the strict builds, not by inspection.

1. **Record header checksums were never actually compared.**
   `decode_frame_header` recomputed the header CRC and compared it with itself, so
   a corrupted header checksum field was not detected. Found by the
   single-bit-flip test at one specific offset. Fixed, and the test now covers
   every offset.
2. **Initial records were framed before their sequence numbers were stamped.** The
   segment header and the ledger-opened record were written with sequence zero,
   which made every reopen fail with a sequence discontinuity. Found by the first
   end-to-end run. Fixed by stamping sequence, revision, and epoch before encoding.
3. **Compaction collected its own new files as garbage.** Orphan collection ran
   before the in-memory names were updated, so the fresh snapshot and segment were
   deleted immediately after being published. Found by the compaction test. Fixed
   by updating the live names before collecting.
4. **Report digests were not computed at all.** `render_report_body` was never
   called, so every published report digest was zero. Found by the report
   determinism test. Fixed, and the digest scope was then narrowed so that session
   bookkeeping cannot change an accounting digest.
5. **Sealing would have deadlocked against its own reconciliation.** The first
   thread-safety pass had `seal_interval` take the exclusive lock and then re-enter
   the public `reconcile`, which takes a shared lock. Caught by the concurrency
   audit before it shipped; fixed by splitting an unlocked `reconcile_locked` for
   callers that already hold a lock.
6. **Unregistered units were reported as dimension mismatches.** Fixed so that an
   unknown unit is named as such.
7. **A segment containing no complete record opened successfully.** Fixed to refuse
   with `InteriorCorruption`.
8. **Unquantified residuals had no dimension.** They were counted against the wrong
   accumulator, which silently closed a dimension that should have been
   indeterminate. Fixed by requiring a registered unit on every residual.
9. **The test harness could silently pass a failing comparison.** The `CHECK_EQ`
   failure path compared two placeholder strings, which were always equal, so a
   real mismatch did not throw. Fixed; several previously hidden failures surfaced
   immediately afterwards and were then fixed in turn.

## Package and downstream proof

Recorded procedure, executed against the release state:

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
cmake --install build/release --prefix <clean prefix>
cmake -S examples/consumer -B <out of tree> -G Ninja
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<clean prefix>
cmake --build <out of tree>
<out of tree>/fel_consumer
```

Installed tree:

```
bin/fel.exe
include/fel/*.hpp                       (19 public headers)
lib/fel.lib
lib/cmake/FacilityEfficiencyLedger/FacilityEfficiencyLedgerConfig.cmake
lib/cmake/FacilityEfficiencyLedger/FacilityEfficiencyLedgerConfigVersion.cmake
lib/cmake/FacilityEfficiencyLedger/FacilityEfficiencyLedgerTargets.cmake
lib/cmake/FacilityEfficiencyLedger/FacilityEfficiencyLedgerTargets-release.cmake
share/doc/FacilityEfficiencyLedger/README.md
share/doc/FacilityEfficiencyLedger/LICENSE
share/doc/FacilityEfficiencyLedger/NOTICE
```

Consumer result: configures, builds, and links against
`FacilityEfficiencyLedger::fel` using only the install prefix; performs a full
create, register, publish, measure, classify, reconcile cycle in a real ledger
directory; confirms that an incompletely classified interval reports non-closure;
and confirms that the interval closes exactly once the residual is recorded. It
exits 0.

The installed CLI was also run from the prefix and behaved as documented, refusing
to own a fact class the ledger does not own.

## Real, synthetic, unsupported

**REAL:** the library, the CLI, the tests, the benchmarks, the durable journal, the
kernel lock, the recovery rules, the compaction publication, the CMake package, the
install, and the out-of-tree consumer. Every measurement quoted anywhere in this
repository was produced by running these binaries on this machine.

**SYNTHETIC:** all facility data. There is no meter, no PDU, no UPS, no BMS, no
DCIM, and no site. Adjacent runtimes are represented by typed contracts only; none
is contacted.

**UNSUPPORTED:** building management or DCIM telemetry throughput, multi-node
cluster behaviour, electrical or cooling measurements, and any real facility
efficiency outcome. No hardware exists here to prove them, so they are not
estimated.
