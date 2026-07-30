# on9kvdb Performance and ESP-IDF NVS Comparison

## Scope and qualification

This document summarizes the performance investigation performed on
2026-07-30. It records:

- the scaling model for `get`, staged `set`, durable `commit`, and `erase`;
- the relevant behavior of ESP-IDF v6.0.2 NVS;
- the measured ESP32-S3 SPI-NOR baseline;
- the expected impact of the optimizations implemented afterward; and
- the bounded runtime-memory cost of the current demo configuration.

The timing results are from the earlier ESP32-S3 FATFS-over-wear-levelling
run. They are not ESP32-P4 SD-card results. The current ESP32-P4/SDMMC build
must be measured independently before making claims about that platform.
Storage residency also matters: the sequential typed benchmark deliberately
caused early keys to remain in the memtable while later keys moved into
SSTables. It is useful as a lifecycle stress workload, but it is not a
controlled type-to-type microbenchmark.

All configured capacities are hard bounds, so every operation is technically
`O(1)` for one fixed firmware build. The notation below treats database
capacity, table geometry, and value size as variables so that the algorithms
can be compared meaningfully.

## Symbols

| Symbol | Meaning |
| --- | --- |
| `N` | Total records considered by a full database scan |
| `V` | Value size |
| `M` | Mutations staged in the active transaction; currently at most 10 |
| `Q` | Distinct mutations in one commit; currently at most 10 |
| `H` | Memtable hash buckets; currently 512 |
| `S` | Bytes moved inside the bounded staging arena |
| `A` | Bytes moved inside the bounded memtable arena |
| `T` | Candidate active SSTables; currently at most 3 |
| `D` | Cached sparse-index entries inspected for one SSTable |
| `B` | Records inspected in one SSTable data block |
| `R` | Records sorted during a memtable flush |
| `L` | Encoded WAL transaction payload size |
| `F` | WAL frames occupied by a transaction |
| `P` | NVS pages searched |
| `E` | NVS entries represented by one page's hash list; physically bounded |
| `C` | NVS blob chunks |

## ESP-IDF NVS behavior relevant to the comparison

The inspected ESP-IDF v6.0.2 implementation does not defer writes until
`nvs_commit()`:

- `nvs_set_*()` and erase operations update the raw NVS partition directly.
- `NVSHandleSimple::commit()` currently validates the handle and returns
  `ESP_OK`; it performs no additional persistence operation.
- NVS still documents `nvs_commit()` as required, so applications should not
  depend on the implementation remaining a no-op.
- NVS searches pages in the partition. Each page maintains a RAM hash list of
  24-bit item hashes, but lookup still scans the bounded nodes in that page's
  hash list and may continue across pages.
- Small scalar values use compact raw-flash entries. Blobs are split into
  chunks and may span pages.

This makes the benchmark's “NVS staging” time the actual persistent-write
time. The final 11 us `nvs_commit()` sample is not comparable to
`on9kvdb::commit()`, which performs the WAL write and durability barrier.

There is also a semantic difference. Twenty NVS setter calls are twenty
independently visible persistent changes; a reset can expose a partial batch.
on9kvdb stages changes in RAM and commits up to ten distinct mutations as one
checksummed, recoverable, atomic WAL transaction.

## Complexity comparison

### Get

on9kvdb checks the transaction overlay, then the committed memtable, then the
active SSTables:

| Case | on9kvdb time |
| --- | --- |
| Best, RAM-resident hit | `O(V)` |
| Expected memtable hit | `O(M + V)`, with expected `O(1)` hash lookup |
| SSTable hit or miss | `O(M + T(D + B) + V)` |
| Worst bounded probe and table search | `O(M + H + T(D + B) + V)` |

With `T <= 3`, table count is a small constant, but `D` grows with table
geometry. A candidate SSTable read currently validates a complete 12 KiB data
block before trusting a record.

NVS searches page hash lists and reads the matching entry or blob chunks:

```text
best/early hit:       O(V)
general lookup:       O(P * E + C + V)
scaling worst case:   O(N + V)
```

Because an NVS page contains a fixed number of entries, `E` is a platform
constant and the usual simplified expression is `O(P + C + V)`. Both engines
therefore have a linear scaling worst case, but their constants differ
substantially. on9kvdb wins when the value is in its memtable; NVS currently
wins when on9kvdb must read and CRC-check SSTable blocks.

### Set and commit

An on9kvdb setter only changes the transaction's bounded staging arena:

```text
best staged set:          O(V)
average staged set:       O(M + V)
replacement worst case:   O(M + S + V)
```

A durable ordinary commit preflights the previous committed state, encodes the
transaction, writes `F` complete WAL frames, and executes one WAL `fsync()`:

```text
ordinary commit:
    O(Q * (H + T(D + B) + A) + L + F)

flush-triggering commit:
    O(H + R log R + output bytes)

compaction-triggering commit:
    O(N * T + R log R + output bytes)
```

Since `T <= 3`, the multiway table work is effectively linear in `N`. These
expressions count CPU and bounded I/O work. One `fsync()` is `O(1)` in call
count but has storage-dependent and highly variable wall-clock latency.

NVS performs its lookup, comparison, and raw-partition update during each
setter:

```text
ordinary scalar set:       O(P * E + V)
blob set:                  O(P * E + C + V)
garbage-collection worst:  O(N + V)
```

Both engines are append-oriented in their ordinary case and have a linear
maintenance worst case. NVS has much smaller physical records and avoids the
FAT/VFS/full-frame `fsync()` path. on9kvdb instead pays for a filesystem WAL
transaction and stronger multi-key atomicity.

### Erase

on9kvdb first proves the key exists, stages a zero-value tombstone, and commits
it through the same WAL path:

```text
staged erase:
    O(M + H + T(D + B))

durable erase:
    lookup cost + ordinary commit cost

maintenance worst:
    same linear flush/compaction class as set
```

NVS searches for the item and marks its raw-flash entry erased. Blob erasure
also visits its chunks:

```text
ordinary erase:       O(P * E + C)
worst reclaim path:   O(N)
```

This explains why a one-byte set and an erase cost nearly the same in
on9kvdb: both occupy a WAL transaction and require the same durability
barrier. Payload size is not the dominant cost.

### Recovery

The implemented recovery path validates active SSTables and rebuilds logical
state using one bounded cursor per active table. Equal composite keys are
grouped and the record with the greatest transaction sequence wins:

```text
on9kvdb merged table recovery: O(N * T)
with T <= 3:                    O(N)
```

The former per-record cross-table lookup repeatedly loaded blocks and did
approximately `O(N * T(D + B))` work. The replacement therefore changes the
scaling model to a sequential linear merge for the current fixed table count.

NVS initialization also scans partition pages and builds its page hash
metadata, so its broad initialization class is approximately `O(N)`.
on9kvdb has additional full-file/table CRC validation and WAL replay work.
No current ESP32-P4/SD measurement establishes which implementation has the
smaller recovery constant.

## Measured ESP32-S3/NOR comparison

The representative populated run used 32 operations per type.

| Workload | on9kvdb relative to NVS | Interpretation |
| --- | ---: | --- |
| Memtable scalar `get` | `0.20x` time | on9kvdb was about 5x faster |
| SSTable-backed `get` | `8.05x` to `129.85x` time | NVS was about 8x to 130x faster |
| All logged gets, summed | `27.02x` time | Residency-skewed aggregate, not a controlled average |
| Individually committed `set` | `34.50x` to `63.05x` time | NVS was much faster |
| All logged sets, summed | `46.78x` time | Same 320 calls and mixed value sizes |
| Individually committed `erase` | `33.56x` to `69.05x` time | NVS was much faster |
| All logged erases, summed | `52.75x` time | Maintenance events affect the aggregate |

Representative absolute read latencies were:

```text
on9kvdb memtable scalar get       23-24 us
NVS scalar get                   114-119 us
on9kvdb SSTable scalar get         5-15 ms
```

The mixed batch result was:

| Phase | on9kvdb | NVS | on9kvdb/NVS |
| --- | ---: | ---: | ---: |
| 20 setter calls, 10 keys | 1.099 ms | 26.454 ms | `0.04x` |
| Commit call | 325.166 ms | 0.011 ms | `29560.54x` |
| Durable end-to-end total | 326.295 ms | 26.469 ms | `12.32x` |
| Amortized per setter call | 16.314 ms | 1.323 ms | `12.32x` |

The phase split must not be read as on9kvdb having a 29,560x slower equivalent
commit primitive. NVS had already performed its writes in the setter loop.
The meaningful latency comparison is the 12.32x end-to-end result, with the
important qualification that only the on9kvdb result represents one atomic
multi-key transaction.

## Interpretation

The measured implementation is better than NVS in one important performance
case: a committed value found in the on9kvdb memtable was approximately five
times faster to read. It is not currently better in general latency:

- NVS is substantially faster for individual persistent sets and erases.
- NVS is substantially faster for reads that require on9kvdb SSTable I/O.
- on9kvdb batching amortizes one WAL synchronization across several logical
  mutations, but the measured ten-key transaction was still 12.32x slower
  end-to-end.
- on9kvdb provides atomic multi-key transactions, full checksummed WAL
  recovery, bounded component memory, and FATFS/SD-card placement. These are
  functional guarantees rather than evidence of lower raw latency.

The appropriate claim is therefore:

> on9kvdb provides faster RAM-resident reads and stronger bounded,
> filesystem-backed transaction semantics, while ESP-IDF NVS currently has
> much lower raw-flash write, erase, and storage-backed read latency.

## Expected impact of the implemented optimizations

The following changes are implemented but require controlled ESP32-P4/SDMMC
before/after measurements:

- **Decoded SSTable index cache and stable result buffer:** a one-table hit can
  fall from as much as 48 KiB of filesystem input to one checked 12 KiB data
  block. It does not eliminate that block read or CRC.
- **Consolidated pre-publication synchronizations:** unreachable SSTable
  contents and redundant metadata share one file synchronization; redundant
  headers for a new unreachable WAL generation do likewise. This improves
  structural-maintenance latency, not the ordinary one-WAL-sync commit floor.
- **Two 1 MiB WAL files:** capacity rises from 120 to 504 single-frame
  transactions across both WALs, reducing rotation frequency by about 4.2x.
  It does not make an individual `fsync()` faster and increases the maximum
  recovery scan.
- **Linear SSTable recovery merge:** replaces repeated per-record table
  lookups with `O(N * T)`, effectively `O(N)`, sequential cursor work. This
  targets initialization/recovery rather than steady-state setter latency.

No percentage improvement should be attached to these changes until the
current P4/SD-card target is measured with equivalent database state, cache
state, and maintenance classification.

## Current bounded-memory context

For the inspected demo configuration,
`CONFIG_ON9KVDB_RUNTIME_MEMORY_BUDGET=143360`. The exact runtime arena carve
is:

| Partition | Bytes | Placement |
| --- | ---: | --- |
| WAL/I/O frame | 4,096 | Internal DMA-capable RAM |
| Namespace registry | 2,240 | PSRAM arena |
| Namespace handles | 352 | PSRAM arena |
| Transaction metadata | 544 | PSRAM arena |
| Memtable buckets | 6,144 | PSRAM arena |
| Staging and WAL-recovery bytes | 25,016 | PSRAM arena |
| Memtable record data | 36,864 | PSRAM arena |
| Decoded SSTable index cache | 20,400 | PSRAM arena |
| Stable maximum-value result buffer | 8,192 | PSRAM arena |
| Shared future/compaction scratch | 39,512 | PSRAM arena |
| **Total** | **143,360** | **4 KiB internal + 136 KiB PSRAM** |

Additional known allocations in the current demo are:

- approximately 168 bytes of FreeRTOS mutex queue payload, plus allocator
  metadata;
- an 8,192-byte internal DMA buffer when the SDMMC backend is selected;
- a 4,600-byte static `on9kvdb` object in the inspected ESP32-P4 ELF; and
- an 8,192-byte main-task stack, which is not component heap.

The known component arena, mutex payload, and optional SD DMA payload total
151,720 bytes of dynamic payload. Including the static database object gives
156,320 bytes, but static storage is not heap. These totals exclude FATFS,
VFS, SDMMC, NVS, allocator metadata, and other ESP-IDF subsystem allocations.
No measured NVS heap delta is available, so this document makes no claim that
on9kvdb uses less RAM than NVS.

## Required next measurements

The ESP32-P4/SDMMC comparison should report:

- fresh, steady-state, and recovered databases separately;
- memtable hit, SSTable hit, and miss separately;
- ordinary commit, WAL rotation, flush, and compaction separately;
- logical bytes and physical `pread`/`pwrite`/`fsync` bytes and calls;
- batch sizes of 1, 2, 5, and 10 distinct mutations;
- P50, P95, P99, and maximum latency;
- initialization split into table validation, merged logical-state rebuild,
  and WAL replay; and
- internal-RAM and PSRAM free/largest-block deltas around mount and database
  initialization.

The benchmark procedure must hold database contents and cache residency
constant when comparing types or engines.
