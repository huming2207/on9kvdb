# on9kvdb Performance Next Steps

## Status

This document records proposed performance work following ESP32-S3 hardware
measurements on 2026-07-30. It is a roadmap, not an approved change to the
durability contract or persistent format.

Implemented on 2026-07-30, pending hardware remeasurement:

- The demo now uses two 1 MiB WAL files and an 8 MiB FAT partition.
- A caller-provided larger arena enables a bounded decoded index cache for all
  physical SSTable slots plus a stable lookup-result buffer. The approved
  100 KiB default retains the disk fallback.
- Unreachable SSTable contents and redundant metadata now share one
  pre-publication file sync. Redundant headers for an unreachable new WAL
  generation are also synchronized together.
- Recovery rebuilds namespaces and logical statistics with a bounded multiway
  merge over the sorted active SSTables instead of performing one cross-table
  lookup per immutable record.

Unless a section explicitly says otherwise, every optimization must preserve
these existing invariants:

- `commit()` returns success only after the complete transaction is durable.
- Every acknowledged transaction is recovered after reset.
- Multi-key transactions are recovered atomically.
- Corruption fails explicitly rather than causing silent truncation or reset.
- A table or WAL slot is not reused while either valid manifest copy can still
  reference it.
- Runtime memory and I/O remain bounded.
- The task watchdog remains enabled and long foreground scans periodically
  block so the idle task can run.

Changes to persisted geometry or record layout require deliberate database
reprovisioning. No automatic migration, deletion, or formatting is proposed.

## Measured baseline

The current development target is:

```text
ESP32-S3, 160 MHz
16 MiB SPI NOR
FATFS over 4096-byte wear-levelling sectors
4096-byte WAL frames
12288-byte SSTable blocks
2 x 262144-byte WAL files
6 x 610304-byte SSTable files
36864-byte memtable data arena
102400-byte runtime memory budget
```

Representative results from a populated database were:

```text
Database initialisation                         8.69 s
Ordinary single-key on9kvdb commit minimum     157-167 ms
Single-key commit average                      184-340 ms
Maintenance-triggering commit maximum          4.9-5.0 s
Memtable scalar get                            23-24 us
SSTable scalar get                             5-15 ms

Mixed on9kvdb batch:
  20 setter calls, 10 distinct keys
  staging                                      1.10 ms
  durable commit                               325.17 ms
  total                                        326.30 ms
  amortised                                    16.31 ms/set call

Mixed NVS batch:
  staging                                      26.45 ms
  nvs_commit()                                 11 us
  total                                        26.47 ms
```

The NVS comparison must be interpreted carefully. In the inspected ESP-IDF
v6.0.2 implementation, `nvs_set_*()` performs the persistent write and
`nvs_commit()` is currently a no-op. The NVS staging loop is therefore the
durable work. It is not an atomic 20-call transaction: a reset in the middle
can expose a partial batch. on9kvdb instead stages in RAM and publishes the ten
distinct mutations as one recoverable atomic WAL transaction.

## Current bottlenecks

### Fixed per-commit durability cost

A small transaction still writes one complete 4096-byte WAL frame and calls
`fsync()` on the WAL file. A one-byte scalar consequently has at least 4096x
logical write amplification before FATFS and wear-levelling overhead.

This explains the approximately 157-167 ms minimum latency. CPU-side
optimizations cannot remove a filesystem synchronization floor.

### Frequent WAL rotation

Each 262144-byte WAL reserves 16384 bytes for identities and redundant
headers:

```text
(262144 - 16384) / 4096 = 60 frames per WAL
```

Two WALs therefore hold only 120 single-frame transactions before reuse
requires checkpointing and, eventually, compaction. The 120-operation
benchmark lands directly on this boundary. Each typed NVS comparison performs
32 sets and 32 erases, so nearly every type crosses another WAL boundary.

### Foreground flush and compaction

The commit which reaches a capacity or reuse boundary synchronously pays for
the complete flush, validation, manifest publication, or compaction. That
produces the measured five-second maximum even though an ordinary WAL append
takes about 167 ms.

### SSTable read amplification

For a hit in one active table, `lookup_table_unsafe()` reads and checks a
12 KiB index block and a 12 KiB data block. `lookup_tables_unsafe()` then
re-reads the winning table because all candidates share the same scratch
buffers. A simple scalar lookup can therefore read as much as:

```text
12 KiB index + 12 KiB data + 12 KiB index + 12 KiB data = 48 KiB
```

This is why early types in the sequential benchmark read in about 23 us while
later types take 5-15 ms. The difference is RAM versus SSTable placement, not
the scalar type.

The same cost affects commits: transaction preflight looks up the previous
committed state of each distinct key before appending the WAL record.

### Recovery amplification

Recovery first validates every active table. It then visits every table record
and calls `lookup_tables_unsafe()` to decide whether the record is the newest
version. The lookup reuses the source block buffer, so recovery reloads that
block before decoding the next record.

The resulting work is approximately proportional to records multiplied by
active tables, with repeated random filesystem reads. The cooperative
`vTaskDelay(1)` calls prevent watchdog starvation but do not address this
algorithmic cost.

## Recommended implementation order

### 1. Add performance accounting

Add bounded diagnostic counters before changing behavior:

- `pread`, `pwrite`, and `fsync` call counts.
- Bytes read and written per file kind.
- Total and maximum time in `pread`, `pwrite`, and `fsync`.
- WAL rotation count and time.
- Memtable flush count and time.
- Compaction count, input/output bytes, and time.
- Table lookup index reads, data reads, and candidate-table count.
- Recovery table-validation, statistics-rebuild, and WAL-scan times.

Benchmark output should classify an operation as ordinary WAL append, flush,
rotation, or compaction rather than averaging all four into one number.

### 2. Keep application transactions batched

Batching is the largest improvement available without changing the storage
format or durability semantics. The measured mixed workload reduced
amortised on9kvdb latency to 16.31 ms per setter call, about 11x better than
the measured single-byte set average.

The current transaction limit is ten distinct mutations. Repeated setter calls
for the same key replace the earlier staged mutation. Applications should
group logically atomic updates and commit once rather than issue one
transaction per scalar.

Potential later work:

- Evaluate increasing the distinct-mutation limit.
- Measure batches of 1, 2, 5, and 10 distinct keys.
- Report logical mutations, setter calls, WAL frames, and physical bytes
  separately.

### 3. Collapse pre-publication synchronizations

SSTable construction currently synchronizes:

1. The table body.
2. The footer.
3. Header copy zero.
4. Header copy one.
5. The manifest publication.

Before manifest publication, the new table is unreachable. It should be
possible to write the body, footer, and both headers, synchronize the table
file once, validate it, and then publish and synchronize the manifest.

The same reasoning applies to a new WAL generation: both redundant WAL headers
can be written before one WAL-file synchronization because the manifest does
not reference that generation yet.

This change should preserve crash safety if the ordering proof is retained:

```text
write complete unreachable object
fsync object
validate object
publish manifest reference
fsync manifest
```

Fault-injection tests must cover reset before, during, and after each remaining
durability boundary.

### 4. Cache SSTable indexes and avoid the winning-table reread

Cache each active table's checked sparse index in PSRAM. With three active
tables and the current geometry, full index-block caching costs at most
36 KiB.

Then change lookup to:

1. Reject tables using manifest min/max keys.
2. Search cached indexes without filesystem I/O.
3. Search candidate tables in descending sequence/generation order.
4. Read the selected 12 KiB data block once.
5. Preserve the winning value in a dedicated bounded result buffer, or stop
   once metadata proves that older tables cannot contain a newer version.

For a one-table hit, this can reduce filesystem input from as much as 48 KiB
to one 12 KiB block. It also reduces transaction-preflight latency.

A small Bloom filter per table is a possible follow-up for absent keys and
overlapping key ranges. Adding it to persistent table metadata is a format
change; maintaining checked in-memory filters built during recovery is not.

### 5. Replace per-record recovery lookups with a linear merge

Implemented on 2026-07-30, pending ESP32-P4 SD-card remeasurement.

Active SSTables are sorted. Rebuild namespaces and logical statistics using
one cursor per table:

1. Select the lowest composite key.
2. Group equal keys from all cursors.
3. Select the highest transaction sequence.
4. Count its live value or tombstone.
5. Advance all cursors in the group.

This is the same fundamental operation used by compaction. It changes
statistics rebuilding from repeated random lookups to a sequential multiway
merge and avoids reloading a source block after every record.

Table block CRCs must still be checked before entries are trusted. The scan
must continue to block periodically for the idle task.

### 6. Increase WAL capacity

Larger WAL files reduce how often foreground operations trigger structural
maintenance:

```text
WAL size       frames per WAL       frames across two WALs
256 KiB              60                     120
512 KiB             124                     248
1 MiB               252                     504
```

A 1 MiB WAL reduces rotation frequency by approximately 4.2x. The demo now
uses this geometry. It does not reduce ordinary `fsync()` latency or the
duration of an individual compaction. It also increases the maximum WAL
recovery scan, so recovery time must be remeasured.

Changing WAL size changes persisted geometry and requires deliberate removal
and reprovisioning of the complete database. The demo FAT partition has been
increased from 6 MiB to 8 MiB rather than relying on the small amount of free
space that two 1 MiB WALs would leave in the previous volume.

### 7. Increase memtable capacity with the WAL

The 36 KiB memtable is small relative to the available 8 MiB PSRAM. A starting
experiment is:

```text
memtable entries       1024
memtable data          96 KiB
runtime budget         approximately 170-190 KiB
```

The exact budget must be derived using the existing checked arena layout.
Increasing the memtable:

- Keeps more keys on the microsecond lookup path.
- Coalesces repeated updates.
- Reduces capacity-triggered flushes.
- Reduces SSTable reads during transaction preflight.

It should be combined with a larger WAL. A larger memtable alone does not
remove the current 60-frame WAL rotation boundary.

These limits are persisted and therefore require database reprovisioning.

### 8. Move flush and compaction off the triggering commit

For predictable tail latency, use two memtable arenas:

- The active memtable accepts new commits.
- A full memtable becomes immutable and is handed to a bounded worker.
- A fresh active memtable is selected immediately.
- The worker writes, synchronizes, validates, and publishes the SSTable.

Compaction can similarly run in bounded slices before space becomes critical.
Manifest publication and physical-slot reuse still require exclusive
serialization.

This does not eliminate flash work, but it prevents one otherwise ordinary
setter from paying the complete five-second maintenance cost. It is an
architectural change and needs additional RAM, task-lifecycle handling, and
power-cut tests.

## Secondary experiments

### ROM CRC

Compare `calc_crc32_update()` with `esp_rom_crc32_le()` after proving identical
incremental and final-complement semantics with golden vectors. This mainly
affects recovery, validation, lookup, and compaction. It will not materially
change filesystem synchronization latency.

### FATFS caches

`CONFIG_FATFS_PER_FILE_CACHE` may reduce cache thrashing among the manifest,
two WAL files, and six table files. With 4096-byte sectors it can consume
roughly 36 KiB for the nine permanently open files. The current
`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` keeps pressure off internal RAM but may
reduce peak I/O performance.

Measure both cache configurations. Do not enable
`CONFIG_FATFS_IMMEDIATE_FSYNC`; on9kvdb already places explicit durability
boundaries and immediate synchronization after every `pwrite()` would be much
slower.

### FATFS fast seek

ESP-IDF fast seek only works for files opened read-only. on9kvdb opens all
files `O_RDWR`, so enabling the Kconfig option alone will not help. Exploiting
it requires separate read-only descriptors for immutable table reads, or a
safe descriptor-mode transition. Include descriptor count and CLMT memory in
the evaluation.

### CPU frequency

An ESP32-S3 240 MHz build may improve CRC, sorting, recovery, and transaction
preflight. It cannot proportionally improve NOR erase, wear levelling, or
`fsync()`, and it increases power consumption.

### Smaller scalar blocks with overflow values

The current 12 KiB minimum block exists so an 8 KiB value fits in one block.
A future format could use 4 KiB blocks for scalar/string records and checked
overflow blocks or a value log for large blobs. This would reduce scalar read
and CRC amplification but is a substantial persistent-format redesign.

### Raw-partition backend

A raw-partition backend can avoid FAT and VFS overhead and approach NVS more
closely. It also assumes responsibility for wear levelling, slot allocation,
bad/torn write handling, and power-cut behavior currently delegated to the
filesystem stack. It should be treated as a separate backend, not a shortcut
inside the FATFS implementation.

### Relaxed durability or group commit

An explicitly named asynchronous mode could acknowledge staging and sync
later, but acknowledged updates could then be lost. It must never silently
replace the current `commit()` semantics.

True group commit can preserve durability: several callers wait while one
worker appends their records and performs one shared synchronization. It
requires concurrent transaction queues and WAL-format/API work. Application
batching remains the simpler first choice.

## Benchmark qualification

Future performance reports should separate:

- Fresh database versus long-running steady state.
- Memtable hit versus SSTable hit versus miss.
- Warm cache versus recovery/cold cache.
- Ordinary commit versus rotation, flush, and compaction.
- Single-key transaction versus multi-key transaction.
- Logical payload bytes versus physical bytes written.
- P50, P95, P99, and maximum latency.
- Initialisation time split into file verification, table validation,
  statistics merge, and WAL replay.
- Performance before and after forced reboot/recovery.

The typed comparison should reset or deliberately force the same storage state
for every type. Running all types sequentially currently makes early types
measure RAM and later types measure SSTables, which is useful as a lifecycle
stress workload but not as a type-to-type latency comparison.

## Proposed milestones

1. Add I/O and maintenance instrumentation.
2. Add benchmark storage-state separation and latency percentiles.
3. Re-measure the implemented consolidated table/WAL-header `fsync()`
   boundaries.
4. Re-measure the implemented optional index and stable-result cache.
5. Re-measure the implemented linear merged recovery statistics rebuild.
6. Compare the new 1 MiB WAL geometry with the recorded 256 KiB baseline.
7. Evaluate a larger memtable after the new WAL/cache results are available.
8. Prototype background flush and compaction if tail latency remains
   unacceptable.
9. Consider format or backend changes only after the preceding results are
   available.
