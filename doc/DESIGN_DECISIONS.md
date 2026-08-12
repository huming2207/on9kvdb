# on9kvdb Approved Design Decisions

> **vNext note:** Decisions that assume C strings or typed values are
> superseded by [`BINARY_VALUE_API.md`](BINARY_VALUE_API.md). The active format
> revision is 6 and names are explicit binary slices.

Reviewed on 2026-07-29. Phase 0 decisions and the Phase 1 through Phase 6
implementation policies are approved. Later changes must follow these
decisions and must ask before changing them.

## Motivation and platform scope

- The custom engine is intended to avoid known or suspected NVS scaling and
  latency limitations and to support SD-card storage, which ESP-IDF NVS does
  not provide.
- V1 must support both SD card and NOR flash through mounted FATFS.
- A filesystem journaling layer is a platform prerequisite. Upstream
  `esp_jrnl` may be used for NOR flash; the planned SD-card port is separate
  work and is not owned by `on9kvdb`.
- FATFS must retain both FAT copies: production sets `use_one_fat=false`.
- Production `max_files` must be comfortably above the component's final,
  measured descriptor requirement rather than assuming that three descriptors
  are sufficient.
- Expected SD media are SanDisk High Endurance/Max Endurance cards or qualified
  SLC-based SD NAND such as XTX-class parts. Published TBW informs endurance
  planning but does not prove power-loss durability.

## Capacity, names, and workload

- Each permanent FAT32-compatible database file is at most `UINT32_MAX` bytes
  (4 GiB minus one byte).
- V1 geometry is selected at compile time through Kconfig and persisted in the
  manifest. The default maximum encoded logical-state size is 768 KiB and the
  default total provisioned database size is exactly 4 MiB.
- The default permanent layout is one 8 KiB manifest, two 256 KiB WAL files,
  and six 596 KiB SSTable files. All file sizes and the total are multiples of
  4096 bytes:

  ```text
  8192 + 2 * 262144 + 6 * 610304 = 4194304 bytes
  ```

- Kconfig exposes maximum logical-state bytes, total provisioned bytes, WAL file size,
  SSTable file size, and SSTable count. Invalid arithmetic, non-4096-byte
  alignment, files above the FAT32 limit, fewer than two WALs, fewer than four
  SSTables, an odd SSTable count, or a file-size sum different from the configured total fail at
  build time where possible and at initialization otherwise.
- "Logical-state bytes" means the encoded size of the newest committed records,
  including tombstones, namespace, key, type, value, and required record
  metadata. The persisted and Kconfig field retains its historical
  `max_live_bytes` name. It is a logical admission limit, not the number of
  bytes currently occupied by old LSM versions.
- "Provisioned bytes" means the sum of every permanent file's final FATFS file
  size, including manifests, WALs, SSTables, stale versions, and compaction
  workspace. It excludes unrelated application files, FAT metadata, and the
  unused tail of a file's final allocation cluster. Actual volume consumption
  is the sum of each file size rounded up to the mounted cluster size.
- Namespace and key names contain 1 through 32 bytes, excluding the terminating
  NUL used by the C++ API. The limit is bytes, not Unicode code points.
- A persisted string or blob is at most 8192 bytes. String length includes the
  terminating NUL, so a string contains at most 8191 non-NUL bytes.
- Values that exceed the limit are rejected. Fragmented and streaming
  string/blob I/O are not v1 features.
- A transaction contains at most 10 mutations. Its staged byte capacity is
  also limited by the configured memory budget, so ten maximum-sized values
  are not guaranteed to fit simultaneously.
- The expected workload is at most 10 commits per normal day and 20 commits per
  peak day. A typical commit changes two keys; a commit changes at most ten.
- Required service life is ten years.

A **live key** is the latest committed, non-tombstoned `(namespace, key)` pair
across the whole database. It is not a namespace handle, an old LSM version, or
an uncommitted mutation. V1 does not require one permanent RAM-index entry per
live key: total live namespaces and keys are bounded by the fixed logical-state
and on-disk capacity. Open handles, active transactions, staged mutations,
memtable entries, and compaction scratch space remain independently bounded RAM
resources.

## Memory and performance

- Runtime arenas are allocated from PSRAM during initialization.
- The default component-owned runtime-memory budget is 100 KiB.
- The configurable v1 hard ceiling is strictly less than 200 KiB.
- The runtime arena has fixed partitions for namespaces, handles, the
  transaction, staging/recovery bytes, memtable buckets/data, sortable offsets,
  table blocks, manifest snapshots, and compaction cursors. Normal operations
  allocate no component-owned heap after initialization.
- SD-card initialization or recovery may take up to approximately three
  minutes. Long scans must yield or otherwise integrate with task-watchdog
  policy; disabling safety checks globally is not an acceptable shortcut.
- Foreground synchronous flush and compaction are accepted for v1.
- Commit/flush performance was intended to improve on ESP-IDF NVS. Performance
  claims must benchmark both engines with the same logical workload and report
  initialization, get, set/commit, compaction, and tail latency.
- The 2026-07-30 ESP32-S3/NOR baseline did not meet that write-latency target:
  see [`PERFORMANCE_COMPARISON.md`](PERFORMANCE_COMPARISON.md). It remains a
  design goal rather than a current performance claim, and ESP32-P4/SDMMC
  results are still pending.

## API and transaction semantics

- The primary v1 API is C++. A C wrapper is deferred.
- One namespace handle selects exactly one namespace.
- A v1 transaction is associated with one namespace handle. Atomic
  cross-namespace transactions are deferred.
- Commit is atomic across every mutation staged in the transaction.
- A transaction reads its own staged mutations. Other namespace handles see
  only committed state.
- Explicit transaction `close()` is an alias for commit and returns
  `esp_err_t`. A failed close leaves the transaction valid for retry or abort.
- Namespace close returns `ESP_ERR_INVALID_STATE` while its transaction is
  active.
- Destruction or abandonment aborts dirty transaction state rather than
  silently committing it.
- Typed integer, string, blob, erase-key, stats, and find-key operations are v1
  requirements.
- General iteration, erase-all, secure purge, encryption, namespace
  enumeration, partition-like multiple stores, and the C wrapper are deferred.

## Phase 3 handles, RAM, WAL, and namespace decisions

Reviewed and approved on 2026-07-29:

- A database supports 64 durable namespaces and eight simultaneously open
  namespace handles by default. Both limits are Kconfig values. One
  generation-checked handle selects one namespace.
- V1 permits one active transaction globally. Other handles may continue to
  read committed values. A transaction reads its own staged overlay.
- A transaction contains at most ten distinct keys. Setting the same key again
  replaces the staged mutation and still counts once. Replacement compacts the
  fixed staging arena in place.
- The default staged-value byte limit is 24576 bytes. A setter that would cross
  the mutation or byte limit returns `ESP_ERR_INVALID_SIZE` without changing
  any existing staged mutation. The transaction remains valid for commit,
  retry, or abort.
- The default memtable has 512 hash slots and a 36864-byte record arena.
  Replacement and publication use bounded in-place compaction. Before any WAL
  write, commit proves that the final memtable and namespace registry state
  will fit. Capacity exhaustion returns `ESP_ERR_NO_MEM`.
- A read-write open of a missing namespace creates only a volatile handle.
  Its first successful non-empty transaction makes the namespace durable.
  The namespace remains known after its last key is tombstoned. A read-only
  open of an unknown namespace returns `ESP_ERR_NOT_FOUND`; an empty commit is
  a no-op and does not create a namespace.
- Bulk runtime arenas are allocated during `init()` and prefer/require PSRAM
  by default. The intended build fails initialization with
  `ESP_ERR_NOT_SUPPORTED` when PSRAM is required but unavailable; it never
  silently consumes the default 100 KiB from internal RAM. One 4096-byte,
  DMA-capable internal I/O frame is allocated at initialization.
- The current demo `sdkconfig` does not yet enable `CONFIG_SPIRAM`; board/module
  qualification and the matching PSRAM mode remain an application integration
  task.
- Production disables `CONFIG_FATFS_PER_FILE_CACHE` for this low-rate workload
  so the permanent descriptor set does not also consume one 4096-byte cache
  per file. The application should provide a preallocated SDMMC DMA bounce
  buffer when its selected host cannot transfer the database's buffers
  directly.
- WAL transaction frames are exactly 4096 bytes even on a FATFS volume using
  512-byte logical sectors. Each transaction begins and ends at a frame
  boundary. Each default WAL file has 240 KiB of frame area. Phase 5 safely
  recycles a slot only after compaction checkpoints its reachable transactions
  and both manifest copies release it.
- The manifest persists the WAL frame size, namespace/handle limits, memtable
  capacity, transaction mutation/byte limits, active WAL slot, referenced WAL
  generations, and safe checkpoint sequence. V1 requires an exact match with
  the firmware's Kconfig logical limits.

## Phase 4 immutable SSTable decisions

Approved on 2026-07-29:

- Phase 4 uses foreground synchronous flushing. It does not add a background
  worker or second immutable memtable. Correctness is preferred over an
  unmeasured flush-latency target; hardware qualification must measure the
  resulting tail latency before release.
- SSTable logical blocks are configurable and persisted, with a 12288-byte
  default. The block size is a multiple of the 4096-byte format alignment and
  is large enough for one maximum-size value plus maximum namespace/key names
  and entry metadata without record fragmentation.
- The default 596 KiB table layout is exactly: 8 KiB permanent identity,
  8 KiB redundant generation headers, 47 12-KiB data blocks, one 12-KiB sparse
  index block, and one 4-KiB footer slot.
- One flush creates one immutable level-0 table in an unreferenced physical
  slot of the active bank. The revision-4 manifest persists the active bank;
  Phase 5 may reuse only the opposite bank after redundant-copy
  stabilization.
- FatFs `f_expand()` does not initialize newly allocated data sectors.
  Provisioning therefore writes and syncs zeroed table header/footer marker
  slots explicitly; the implementation never assumes preallocated contents
  are zero.
- A flush sorts 32-bit memtable record offsets with a bounded in-place
  heapsort. The default scratch partition is one data block, one index block,
  and 512 sortable offsets. It uses no STL, recursion, lambda, or heap
  allocation.
- Data blocks, the sparse index, footer, and both table headers are
  checksummed. The complete authoritative table is synced, read back, and
  validated before a manifest generation may reference it.
- Because a new table is unreachable until that manifest publication, its
  body, footer, and both redundant headers are written first and made durable
  by one table-file sync. The separately synced manifest remains the sole
  publication point. Redundant headers for an unreachable new WAL generation
  are likewise written before one WAL-file sync.
- The manifest remains the sole publication point. Its table references bind
  physical slot, logical generation, level, sequence range, entry/block
  counts, content checksum, and full minimum/maximum composite keys.
- To fit full key ranges and bounded descriptors in one 4096-byte manifest
  slot, v1 supports at most 16 configured SSTable slots. The default remains
  six.
- The checkpoint advances only after the new table reference is durable in the
  manifest. Recovery validates referenced tables before replaying WAL
  transactions newer than the safe checkpoint.
- Phase 4 preserves all tombstones. Phase 5 retains the newest tombstone for
  each erased key so namespace identity remains durable.
- No compression, Bloom filter, generic read cache, or public manual-flush API
  is added in Phase 4. Commit flushes automatically when the current memtable
  cannot accept the complete transaction or when transitioning to the second
  WAL slot.

## Phase 5 compaction and reuse decisions

Approved on 2026-07-29:

- The default remains six 596-KiB SSTables and exactly 4 MiB provisioned. The
  default hard logical-state limit becomes 768 KiB.
- The configured SSTable count is even and split into two equal banks. One
  bank is authoritative while foreground full compaction writes the other.
- Compaction merges every published table and the committed memtable, keeping
  only the newest record for each composite key. V1 retains the newest
  tombstone so a namespace remains durable after its final key is erased.
- Every output table is synced, read back, and validated before manifest
  publication. A second stabilizing manifest write makes both redundant
  copies select the new bank before any old table slot can be reused.
- Compaction is synchronous and uses only bounded initialization-time scratch
  memory. There is no background task, second immutable memtable, STL
  container, or steady-state heap allocation.
- WAL reuse follows the same reachability rule. A WAL generation can be
  overwritten only after all its committed transactions are checkpointed and
  neither valid manifest copy references it.
- If the complete logical state cannot fit the inactive bank, the operation
  returns `ESP_ERR_NO_MEM` without publishing partial output or overwriting the
  authoritative bank.
- General iterators and `erase_all()` remain deferred. Deleting the complete
  database file set is an explicit application maintenance operation, not an
  `on9kvdb` API.

## Phase 6 API and diagnostics decisions

Approved on 2026-07-29:

- V1 freezes the C++ API listed above. It includes all integer, string, blob,
  erase-key, find-key, explicit-transaction, and stats operations.
- `get_stats()` returns a lock-consistent, non-secret snapshot. It includes
  logical counts, byte use, compaction totals/latency, capacities, bounded RAM
  use, active table/WAL topology, and manifest recovery state.
- Compaction totals are saturating per-boot diagnostics and are not persisted.
- A failed manifest write or sync latches a storage fault because publication
  is ambiguous. Normal APIs then return `ESP_ERR_INVALID_STATE` until
  `deinit(true)` followed by `init()` performs recovery. `get_stats()` remains
  available to report that fault.
- General iterators, `erase_all()`, namespace enumeration, encryption, and a C
  wrapper remain deferred.

## Corruption, reset, and integration policy

- Corruption handling is fail-closed. Do not start services that require stored
  configuration; report a typed diagnostic and wait for maintenance.
- The database never automatically deletes files, formats FATFS, or replaces
  corrupt data with defaults.
- Deleting FATFS files is not `erase_all()` or normal factory reset.
- A later explicit logical reset must reinitialize structurally valid fixed
  files in place. Repairing or reformatting damaged FATFS is a separate
  platform maintenance action.
- V1 performs no geometry migration. If any persisted geometry differs from
  the running firmware's Kconfig, `init()` returns
  `ESP_ERR_INVALID_SIZE`. The component does not delete,
  resize, or recreate the database; the application/user must deliberately
  remove the complete database file set and provision a new database.
- At-rest encryption is not a v1 requirement.
- Soulcloud snapshot, command-deduplication, and terminal-result logic belongs
  above the generic KV API. Add that integration after the KV engine is usable;
  the Soulcloud client itself is not part of the current implementation.

## Remaining integration and qualification decisions

These items do not reopen the implemented phases, but they gate Phase 7 or a
future backend/integration:

- Phase 2 uses FAT32 only, with 512-byte or 4096-byte logical sectors and
  4096-byte database format slots/alignment. It supports ESP32-family targets
  through ESP-IDF v6.0.2 FATFS; the initial project qualification target is
  ESP32-S3.
- A future backend may target LittleFS v3 or another suitable filesystem, but
  that is outside v1 and must not weaken or silently reinterpret the FAT32
  on-disk contract.
- The application must mount FATFS with two FAT copies and a file-descriptor
  allowance of at least `SSTABLE_COUNT + 3` for one database, plus its own
  descriptors. The component keeps the manifest, both WALs, and every SSTable
  open after initialization.
- ESP-IDF's public mounted-FATFS API does not expose the mounted FAT subtype,
  logical-sector size, cluster size, FAT-copy count, or configured
  `max_files`. Phase 2 validates the mount through `esp_vfs_fat_info()`, checks
  every permanent file with `stat()` and the contiguous-file API, and fails on
  any observable mismatch. FAT32, 512/4096-byte sectors, two FAT copies,
  journaling, and the descriptor allowance remain an explicit platform mount
  contract until ESP-IDF exposes a stable query API; do not couple the
  component to private VFS/FatFs context structures merely to inspect them.
- Phase 5 uses the approved equal-bank full-compaction, stabilization, and
  reuse policy documented above. V1 retains newest tombstones to preserve
  durable namespace identity.
- Performance acceptance requires a measured NVS baseline rather than relying
  on recollection that some operations are `O(n)`.
- Final SD power-loss qualification waits for the separately owned journal
  port and testing on the selected endurance/SLC media.
- Soulcloud integration tests wait for enough of the generic database and
  client stack to exist, but the storage semantics in the Soulcloud
  requirements remain mandatory.
