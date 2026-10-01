# Concurrency audit

This document is the audit required before any thread-safety claim is made. It
answers each hazard explicitly rather than relying on tests.

## Ownership

* **Kernel file lock** (`fel.lock`): held for the entire lifetime of a `Store`
  handle. Exclusive for a writer, shared for a reader. Acquired once in
  `Store::open` and released only when the handle is destroyed or the process dies.
* **In-process reader/writer lock** (`Ledger::mutex_`, a `std::shared_mutex`):
  taken by every public `Ledger` method, exactly once per call.
* **No other locks exist.** There are no condition variables, no worker threads,
  no asynchronous callbacks, and no locks inside `LedgerView`, `reconcile`,
  `report`, or the codec.

The two locks have a fixed order: the kernel lock is always already held before any
in-process lock is taken, and it is never acquired while an in-process lock is
held. There is no cycle, so there is no lock-order inversion.

## Hazard-by-hazard

**Self-deadlock.** Not possible from a public method: each takes exactly one lock
and never re-enters a public method while holding it. The one place where
re-entrancy would have been natural — sealing, which needs a reconciliation — calls
the unlocked `reconcile_locked` instead. A test seals an interval while four reader
threads are reconciling in a loop, so a regression here hangs rather than passes.

**Read-to-write lock upgrade while a read guard is held.** Does not exist. No code
path holds a shared lock and then asks for the exclusive one. `std::shared_mutex`
would deadlock on that, and the design removes the possibility rather than relying
on discipline.

**Write locks held across re-entry.** The exclusive lock is held across the whole
command: validation, framing, durable append, flush, and index update. It is never
held across a call into caller-supplied code, because there is no caller-supplied
code in the path: commands take plain data structures, not callbacks.

**Callback or event emission under a lock.** No callbacks and no listeners exist in
the runtime. Nothing observable happens under a lock except the state change the
lock protects.

**Inconsistent lock ordering.** Only two locks exist and their order is fixed by
construction (see Ownership).

**Shutdown or join while holding worker-required state.** There are no workers, no
background threads, and no thread pool. Destruction releases the in-process lock
implicitly and the kernel lock in `FileLock::~FileLock`. A destructor never waits
for another thread.

**Cancellation races.** There is no cancellation. A command either commits or
returns a refusal; there is no partially applied state, because the durable append
and the in-memory index update are performed together under the exclusive lock and
the record is the only source of truth.

**Stale-authority races.** Two guards exist and both are checked under the
exclusive lock immediately before the append: `expected_epoch` (the writer session
that the caller believes it is talking to) and `expected_revision` (the accounting
identity the caller believes it is extending). A stale value refuses the command.
Because the check and the append happen under one lock, a command cannot pass the
check and then commit against a different revision.

**Concurrent publication.** Compaction writes a snapshot and a fresh segment, then
atomically replaces the manifest. Readers hold the shared lock, so they observe
either the old manifest's files or the new manifest's files, never a mixture. The
manifest replacement is the commit point; files written before it are uncommitted.

**Cross-process races.** The kernel lock is the only cross-process mechanism, and
it is a real byte-range lock (Windows `LockFileEx`) or a real advisory lock
(`flock`). A test spawns a child that holds the write lock and confirms that a
second writer and a reader are both refused.

## Lock discipline summary

```
public read  (view, reconcile)            -> shared lock, then a pure function
public write (commands, seal, compact)    -> exclusive lock, then append + index
private helpers                           -> no lock; caller must hold one
```

## Fairness

`std::shared_mutex` gives no fairness guarantee, and neither does the underlying
platform primitive. A writer that repeatedly takes the exclusive lock can delay
readers for an unbounded time, and readers cannot starve a writer into failing.
The library does not promise otherwise, and no test assumes otherwise: the
concurrency tests rendezvous with a `std::latch` after each reader has completed
one read before the writer begins, so the assertions are about the library rather
than about the scheduler.

Operationally this means throughput under sustained mixed load is not fair, and a
caller that needs bounded read latency should quiesce the writer or run the reader
against a separate handle in a separate process.
## What is deliberately not synchronised

`Store` and `LedgerView` are single-threaded components. Their accessors expose
references to internal state, and reading them while another thread is issuing
commands is a data race that the library does not prevent. This is documented in
the headers, and `Ledger`'s unsynchronised accessors carry the same warning.
Callers that need concurrent inspection use `Ledger::view` and
`Ledger::reconcile`, which take the shared lock.

This is a deliberate trade: the durable substrate stays free of synchronisation
cost in the paths that matter, and the thread-safe surface is a single, auditable
class.

## Evidence

* `tests/concurrency_threads.cpp`: 8 writer threads x 25 commands, with 4 reader
  threads folding concurrently; every command commits exactly once; observed
  revisions never move backwards; no read observes an over-allocated entry.
* Sealing while four reader threads reconcile in a loop.
* Compaction while four reader threads fold; no reader observes a partially applied
  compaction.
* `tests/multiprocess_lock.cpp`: real child processes, real lock contention, real
  abrupt death.
* The whole suite runs clean under AddressSanitizer, and the Debug configuration
  builds with `/W4 /WX` and zero warnings.
