# Persistence, recovery, and compaction

## Layout

```
<ledger directory>/
  fel.manifest                atomically published pointer to the live files
  fel.lock                    kernel-locked single-writer lock file
  fel-segment-NNNNNNNN.fel    append-only record stream
  fel-snapshot-NNNNNNNN.fel   compacted retained record set (absent before the first compaction)
```

The manifest names the active segment and, optionally, the base snapshot. It is the
only file that decides which other files are live.

## Frame format

Every record is framed identically. All integers are little-endian.

```
offset  size  field
0       4     magic 'FELR'
4       2     format version
6       2     record type
8       4     flags
12      8     sequence
20      8     revision
28      8     epoch
36      4     payload length
40      4     payload CRC32C
44      4     header CRC32C (over bytes 0..43)
48      N     payload (canonical encoding, fixed field order per record type)
48+N    32    SHA-256(previous chain digest || header bytes || payload bytes)
```

Header size 48, trailer size 32. The chain digest binds the record to everything
before it, so removing, reordering, or editing any record breaks the chain at that
point and every later record.

## Commit points

* **A record** is committed when its complete header, payload, and chain trailer
  are on disk *and* the append has been flushed with a durable write barrier
  (`FlushFileBuffers` / `fsync`). A record whose bytes are only partially present
  is not committed and is never visible after recovery.
* **A compaction** is committed when `fel.manifest` has been atomically replaced
  (`MoveFileEx` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` /
  `rename`). Files written before that replacement but never named by the manifest
  are uncommitted.

Both commit points are single operations, so there is no window in which a reader
can observe a half-published change.

## Recovery rules

On open, the reader walks the active segment from the beginning.

For each record, in order:

1. Fewer than 48 bytes remain: **torn tail**. Discard the tail.
2. The header checksum does not match:
   * all remaining bytes are zero: **torn tail**, discard;
   * fewer than 80 bytes remain: **torn tail**, discard;
   * otherwise: **interior corruption**, refuse to open.
3. The declared payload runs past the end of the file: **torn tail**, discard.
4. The payload checksum does not match:
   * all remaining bytes are zero: **torn tail**, discard;
   * otherwise: **interior corruption**, refuse to open.
5. The chain digest does not match the stored trailer: **chain broken**, refuse.
6. The sequence is not exactly one greater than the previous record's, or a
   mutating record's revision does not advance the previous revision by exactly
   one, or the epoch regresses: **interior corruption**, refuse.

A writer truncates the discarded tail to the last committed boundary and records
that it recovered. A reader observes the tail and modifies nothing.

Recovery can discard bytes that were never committed. It can never discard, repair,
or reinterpret bytes that were committed: if the bytes are all there and the
checksums disagree, the ledger stops.

## The manifest

```
u16 format version | string ledger id | string active segment | string base snapshot
| u64 published epoch | string runtime | 32-byte SHA-256 over the preceding bytes
```

It is written to `fel.manifest.tmp`, flushed, and then atomically renamed over
`fel.manifest`.

## The snapshot

```
u16 format version | string ledger id | u64 base revision | digest chain digest
| u64 epoch | u64 min sequence | u64 max sequence | u64 retained count
| u64 dropped count | digest dropped digest | instant created at
then, retained count times:
  u64 sequence | u64 revision | u64 epoch | u16 record type
  | u32 payload length | payload | 32-byte content digest
finally:
  32-byte SHA-256 over everything above
```

The snapshot is self-verifying. A truncated or edited snapshot is refused as a
whole; it is never partially applied.

## Compaction

1. Fold the live state and decide which records to retain. A measurement,
   classification, or residual is dropped only when a retained record supersedes or
   voids it. Corrections, voids, seals, registrations, generations, and epoch
   adoptions are always retained.
2. Write the new snapshot atomically.
3. Create the new segment and write its segment header followed by an
   `EpochAdopted` record with basis `compaction`. The new segment's chain starts
   from the snapshot's chain digest, so the audit trail is continuous.
4. Publish the new manifest atomically. **This is the commit point.**
5. Collect unreferenced ledger files as garbage.

If a crash happens before step 4, the previously named files are still intact and
still authoritative; the newly written files are unreferenced and are collected on
the next writer open. A test constructs exactly that state by copying the live
snapshot and segment to higher-numbered names without touching the manifest, and
confirms that the reopen ignores them, keeps the same revision, and removes them.

## Epochs

Every successful writer open adopts the next epoch and appends an `EpochAdopted`
record naming the basis:

| Basis | Meaning |
| --- | --- |
| `initial_creation` | the ledger was created |
| `clean_reopen` | the previous session ended without a torn tail |
| `recovered_torn_tail` | the previous session left an uncommitted tail |
| `compaction` | a compaction re-anchored the chain |

`recovered_torn_tail` is the recovery barrier described in the README: evidence
attested before it is recovered evidence and is never promoted to current.

## Integrity verification

`verify_ledger_directory` reads the manifest, the snapshot, and the segment and
revalidates every checksum, every chain link, every content digest, and every
sequence and revision invariant, without modifying anything. It takes the shared
lock, so it refuses with `LockHeldExclusive` while a writer holds the ledger
instead of reading a moving target. The `fel verify` command exposes it.
