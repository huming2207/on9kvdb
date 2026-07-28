# on9kvdb Implementation Plan

## Status and scope

This file is the design and implementation plan for `on9kvdb`. No storage
format or public API described as **proposed** below is approved merely by
appearing in this plan.

The intended component is a fixed-capacity, namespaced key-value database for
ESP32-class devices. It should expose an API similar to ESP-IDF NVS while using
an LSM tree over files on an already-mounted, filesystem-journaled FATFS
volume. The volume may be on an SD card or NOR flash.

The working platform assumption is that a suitable filesystem journaling layer
can be provided. Upstream `esp_jrnl` may be used for SPI NOR. Porting
`esp_jrnl` to SD card is planned later and is explicitly outside the current
`on9kvdb` scope.

This task is not an extension of `on9ringstore`. Reuse its coding conventions,
bounded-I/O helpers, provisioning discipline, and crash-recovery mindset, but
do not reuse its append-only ring semantics.

Before implementing code, read:

- `../on9ringstore/on9rstore.hpp`
- `../on9ringstore/on9rstore_defs.hpp`
- `../on9ringstore/on9rstore.cpp`
- `../on9ringstore/on9rstore_io.cpp`
- `../on9ringstore/on9rstore_manifest.cpp`
- `../on9ringstore/on9rstore_segment.cpp`
- `../on9ringstore/on9rstore_entry.cpp`
- `../on9ringstore/README.md`
- `/home/hu/Projects/llm-docs/soulcloud/20260728-config-store-requirements.md`
- `/home/hu/esp/esp-idf/components/nvs_flash/include/nvs.h`
- `/home/hu/esp/esp-idf/components/nvs_flash/include/nvs_flash.h`
- `/home/hu/esp/esp-idf/components/fatfs/vfs/esp_vfs_fat.h`

The inspected local ESP-IDF baseline is v6.0.2. Recheck the local version and
the relevant declarations before implementation because the IDF checkout may
change.

## Requirement language

This document separates three kinds of statement:

- **Required**: explicitly requested constraints. Do not weaken them without
  asking the user.
- **Proposed**: a recommended design to validate before freezing the on-disk
  format.
- **Open decision**: missing product or workload information. Do not guess.

If an implementation choice would resolve an open decision, stop and ask
instead of silently selecting a value.

## Required constraints

### Storage and files

- Operate on a mounted FATFS path. Mounting, unmounting, media ownership, and
  filesystem journaling belong to the application/platform layer.
- The application may require a journaled mount as a precondition for opening
  the database. `on9kvdb` must document and validate every mount property it
  can observe, but it must not implement or own the future SD-card
  `esp_jrnl` port.
- Support FATFS on SD card and FATFS on NOR flash. Do not silently make the
  database specific to only one medium.
- Create every database file at its final size with
  `esp_vfs_fat_create_contiguous_file(base_path, full_path, size, true)`.
- Verify every file with `stat()` and
  `esp_vfs_fat_test_contiguous_file()`.
- After provisioning, never grow, truncate, rename, unlink, or replace a
  healthy database file. Reuse fixed physical slots by changing checked
  logical generations inside them.
- Treat the geometry persisted by an existing database as authoritative. A
  later Kconfig or runtime default must not resize or reinterpret it.
- Bound every offset and length before seeking, reading, writing, or converting
  to `off_t`.
- Check all I/O results, including short reads/writes and `fsync()`.
- Fail closed on missing, wrongly sized, non-contiguous, unknown-version, or
  structurally inconsistent canonical files. Do not auto-format or silently
  erase a damaged store.

Contiguous fixed-size files reduce steady-state FAT allocation and directory
metadata changes. They do not by themselves make FATFS, the block device, or
the database power-loss atomic.

### Database behavior

- Use an LSM-tree design with a checksummed write-ahead log.
- Keys are scoped by namespace; the internal key is the unambiguous composite
  `(namespace, key)`.
- Use tombstones for deletion and preserve them until compaction proves that no
  older live value can reappear.
- Provide explicit commit semantics similar to `nvs_commit()`.
- A successful durable commit must recover as entirely committed after reset.
  An incomplete transaction must recover as entirely uncommitted.
- Recovery must be deterministic from checksummed generations and commit
  records. Wall-clock time, directory enumeration order, and "largest-looking
  file" are not authority.
- A committed value must never depend solely on a RAM cache.
- Database transaction atomicity must come from the database WAL, manifest,
  checksums, and copy-on-write publication. Do not treat filesystem journaling
  as a substitute for those mechanisms.
- Capacity is fixed after provisioning. Exhaustion must return a typed error;
  it must never trigger file expansion.

### RAM and allocation

- Perform no component-owned heap allocation during normal open/get/set/erase,
  commit, flush, recovery-complete steady state, or compaction operations.
- Heap allocation is allowed only during `init()`/`deinit()` unless a later
  exception is explicitly justified and approved.
- Allocate bounded arenas, indexes, handle tables, transaction staging, and
  compaction scratch space during initialization. Prefer one or a few clearly
  owned allocations over many small allocations.
- Do not use runtime C++ STL containers, strings, streams, smart pointers, or
  algorithms. Compile-time-only use such as `constexpr` lookup-table
  construction is allowed.
- Do not add an unbounded read cache. Any optional block cache must have a
  configured byte limit, deterministic eviction, measurable benefit, and no
  per-operation allocation.
- Audit indirect allocation by the selected VFS/FATFS calls and descriptor
  policy. Keep descriptors open from initialization where practical, or
  document why bounded open/close activity is safe.

### Coding style

Match `components/on9ringstore`:

- C++ source with lower-case snake-case class, method, member, and file names;
- four-space indentation and the same brace/line-wrap style;
- `esp_err_t` error propagation and checked early returns;
- fixed-width integer types and explicit casts;
- `static const constexpr` constants;
- packed persistent structures with `static_assert(sizeof(...))` and important
  `offsetof(...)` checks;
- FreeRTOS mutexes with explicit lifecycle handling;
- `*_unsafe` suffix for methods whose required lock is already held;
- separate source files by responsibility rather than one large source file;
- no exceptions, RTTI-dependent design, iostreams, or hidden ownership;
- concise ESP logging that never prints stored secrets or full values.

Copy behavior and conventions, not existing ring-store bugs. If a reused
pattern conflicts with a requirement in this file, this file wins.

## Platform facts that shape the design

### FATFS

ESP-IDF documents FATFS as weak against sudden power loss. Its normal two FAT
copies improve recovery but are not transactional database storage.

`esp_vfs_fat_create_contiguous_file()` requires a new or zero-length file and
uses FatFS `f_expand()`. Passing `alloc_now=true` allocates the space
immediately. This is a provisioning operation only; do not call it during
steady-state rotation or compaction.

`fsync()` reaches FatFS `f_sync()`. On the inspected ESP-IDF v6.0.2 SDMMC
disk-I/O backend, `CTRL_SYNC` returns success without issuing an additional
card command. Therefore, treat `fsync()` as the required software durability
barrier but do not claim stronger SD-card persistence than real hardware tests
demonstrate.

### esp_jrnl and the journaled-filesystem assumption

As of 2026-07-28, upstream `esp_jrnl`:

- supports FatFS on SPI-flash partitions, not SD cards;
- reserves a journal area outside the filesystem data region;
- wraps one filesystem API call per journal transaction;
- can replay an interrupted filesystem transaction on mount; and
- explicitly does not guarantee complete power-off resilience.

The application/platform layer is expected to provide the journaled FATFS
mount. Do not make `on9kvdb` call journaling mount APIs.

For the current implementation:

- assume an appropriate filesystem journal can be present;
- use upstream `esp_jrnl` for SPI NOR where applicable;
- do not port `esp_jrnl` to SD inside this component; and
- keep database-level WAL/manifest recovery independent of the journal's
  one-filesystem-call transaction boundary.

Until the SD port exists, SD integration may use a test/development mount, but
SD power-loss qualification that depends on filesystem journaling is deferred.

### NVS compatibility target

ESP-IDF v6.0.2 NVS uses opaque handles, namespace-scoped keys, read-only and
read-write modes, typed integer/string/blob accessors, erase operations,
`nvs_commit()`, close, stats, and iterators. Its namespace and key names are at
most 15 characters.

`on9kvdb` should be source-familiar, not symbol-compatible or storage-format
compatible. Use an `on9kvdb_` prefix and component-specific error codes so it
can coexist with NVS.

## RAM cache and WAL decision

A separate generic RAM read cache is not a prerequisite for the WAL.

An LSM tree does require a bounded mutable in-memory table (memtable). The
memtable is the current searchable representation of recent committed writes;
the WAL is the durable recovery copy of those writes until they are present in
an SSTable selected by the manifest.

The required commit order is:

```text
validate and stage the whole transaction
        |
append WAL mutation records and one transaction commit record
        |
fsync the WAL and check the result
        |
publish/apply the transaction to the committed memtable
        |
return ESP_OK
```

If power fails after the WAL sync but before RAM publication, recovery replays
the committed transaction. If power fails before the complete commit record
and durability barrier, recovery ignores it.

**Proposed v1:** use no separate data-block read cache. Use:

- a fixed transaction-staging arena;
- one fixed mutable memtable arena;
- a fixed open-addressed hash index or similarly bounded lookup index over the
  memtable;
- a fixed array of sortable entry references/offsets for SSTable flush; and
- bounded table descriptors and on-disk sparse indexes.

Foreground synchronous flush/compaction is simpler and easier to prove than a
background worker. Begin with it unless latency requirements show it is
unacceptable. A second immutable memtable and background work add concurrency,
memory, shutdown, and recovery states and require explicit approval.

## Proposed physical layout

This layout is a design candidate, not yet frozen:

```text
<base path>/
├── manifest.db
├── wal_0.db
├── wal_1.db
├── table_0.db
├── table_1.db
├── ...
└── table_<N-1>.db
```

Every file is permanent, contiguous, and fixed-size.

### Manifest

`manifest.db` should contain at least two independently checksummed,
sector-aligned manifest slots. A manifest generation is the sole publication
point for:

- storage format version and feature flags;
- database identity;
- persisted geometry and limits;
- live SSTable slot IDs, logical generations, levels, key ranges, and sequence
  ranges;
- active WAL slot/generation and safe replay/checkpoint sequence;
- next logical transaction/table generation;
- provisioning/ready state; and
- bounded health/recovery counters if they are persisted.

Write the inactive manifest slot, `fsync()` it, optionally read it back, and
only then treat its generation as current. Recovery selects the highest valid
generation that is internally consistent with referenced files. Do not mutate
a published manifest slot in place.

### WAL slots

Use at least two physical WAL slots so recycling one cannot destroy the only
durable recovery log.

Each WAL generation should have redundant/checksummed headers and a sequence of
self-delimiting records. Each record needs explicit:

- magic, storage revision, record kind, and header size;
- WAL generation and transaction sequence;
- namespace/key/value type and explicit lengths where applicable;
- payload checksum and header/record checksum;
- bounds/alignment information; and
- commit-record metadata sufficient to validate the complete transaction.

Do not use file length as the logical WAL tail. The file length never changes.
Recovery scans within the generation's valid logical region and stops at the
first erased, invalid, torn, out-of-order, or out-of-bounds record. Only
transactions with a complete valid commit record are replayed.

Before recycling a WAL slot:

1. Flush all transactions it protects into one or more new SSTables.
2. Sync and validate those SSTables.
3. Publish a manifest generation selecting the SSTables and advancing the safe
   WAL checkpoint.
4. Only then initialize the old WAL physical slot with a new logical
   generation.

The exact replicated-header protocol and whether records may wrap inside one
slot are open design details that require fault-injection proof before format
freeze.

### SSTable slots

An SSTable is immutable after it is published by the manifest. It should
contain:

- redundant or separately verifiable header/footer metadata;
- sorted composite internal keys;
- type, transaction sequence, tombstone state, and value bytes;
- per-entry or per-block integrity checks;
- a bounded sparse index;
- optional Bloom-filter data only if measurements justify it; and
- a checksum covering all authoritative table metadata.

Compaction must be copy-on-write:

1. Select only currently free output table slots.
2. Merge live input tables and tombstones into the output slots.
3. Sync and validate every output.
4. Publish one new manifest generation selecting the outputs and no longer
   selecting the inputs.
5. Reuse old input slots only after that manifest is durable.

Reserve enough unselected table capacity to complete worst-case compaction.
Returning `NOT_ENOUGH_SPACE` is preferable to overwriting a live table.

The level count, size ratio, table-slot sizes, compaction policy, and tombstone
drop rules remain open until workload and capacity limits are supplied.

## Proposed logical model

The internal ordering key should be conceptually:

```text
(namespace bytes, key bytes, transaction sequence descending)
```

The exact byte encoding must be explicitly specified and must not depend on C++
object layout, locale, pointer size, or structure padding.

Lookup order should be:

1. the calling handle's uncommitted staged overlay, if read-your-writes is
   approved;
2. the committed mutable memtable;
3. any immutable memtable, if later introduced;
4. live SSTables in an order proven by manifest level/generation invariants.

The first matching newest sequence decides the result. A tombstone means not
found.

## Proposed public API surface

Keep the lifetime of the database instance explicit because storage is a FATFS
path rather than an NVS partition.

Candidate shape:

```c++
struct on9kvdb_cfg;
using on9kvdb_handle_t = uint32_t;

class on9kvdb
{
public:
    explicit on9kvdb(const char *file_path, const on9kvdb_cfg *config);
    ~on9kvdb();

    esp_err_t init();
    esp_err_t open(const char *namespace_name, on9kvdb_open_mode mode,
                   on9kvdb_handle_t *handle_out);
    void close(on9kvdb_handle_t handle);

    esp_err_t set_i8(...);
    esp_err_t set_u8(...);
    esp_err_t set_i16(...);
    esp_err_t set_u16(...);
    esp_err_t set_i32(...);
    esp_err_t set_u32(...);
    esp_err_t set_i64(...);
    esp_err_t set_u64(...);
    esp_err_t set_str(...);
    esp_err_t set_blob(...);

    esp_err_t get_i8(...) const;
    esp_err_t get_u8(...) const;
    esp_err_t get_i16(...) const;
    esp_err_t get_u16(...) const;
    esp_err_t get_i32(...) const;
    esp_err_t get_u32(...) const;
    esp_err_t get_i64(...) const;
    esp_err_t get_u64(...) const;
    esp_err_t get_str(...) const;
    esp_err_t get_blob(...) const;

    esp_err_t erase_key(...);
    esp_err_t erase_all(...);
    esp_err_t commit(on9kvdb_handle_t handle);
    esp_err_t deinit(bool force = false);
};
```

This is illustrative; freeze exact declarations only after deciding whether the
primary API is the C++ instance API above, a C opaque-store API, or both.

Required NVS-familiar behavior:

- opening a namespace returns an opaque generation-checked handle;
- a read-only handle rejects mutations;
- setters are typed and type mismatch is reported;
- `get_str()` and `get_blob()` support a null output buffer length query;
- too-small output reports required length without overflow;
- `erase_key()`, `erase_all()`, `commit()`, and `close()` have documented
  uncommitted-change behavior;
- closing a handle does not silently convert an unsuccessful commit into
  success;
- handles have fixed capacity and stale handles cannot alias newly opened ones
  without a generation check.

**Proposed extension:** make one `commit()` atomic across all staged mutations
on that handle. This is stronger and more useful than merely persisting each
setter independently, but it must be explicitly approved and documented.

Stats, iterators, `find_key()`, namespace enumeration, secure purge, encryption,
and a C wrapper are later phases unless selected as v1 requirements.

Do not reuse `ESP_ERR_NVS_*` values for a different engine. Define stable
component errors for at least invalid name/handle, not found, type mismatch,
invalid length, not enough space, corruption, unsupported/newer format, and
read-only/not allowed.

## Provisioning and recovery

### New-store provisioning

Follow the fail-closed ownership approach used by `on9ringstore`:

1. Validate all requested geometry, arithmetic, path lengths, FATFS free space,
   `off_t` limits, RAM budgets, and descriptor limits.
2. Scan the base directory and reject colliding canonical database filenames.
3. Create `manifest.db` contiguously at final size.
4. Write and sync redundant `provisioning_owned` manifest slots before
   creating data files.
5. Create each WAL and table file contiguously at final size, initialize its
   identity/slot metadata, sync it, and validate size and contiguity.
6. Resume safely if reset occurs after ownership is durable.
7. Publish a `ready` manifest only after all permanent files validate.

If data files exist but no valid ownership manifest exists, do not claim or
erase them automatically.

### Existing-store recovery

1. Validate all canonical file names, types, exact sizes, and contiguity.
2. Read both/all manifest slots and select a valid consistent generation.
3. Validate every referenced table header/footer, generation, bounds, ordering,
   and integrity metadata.
4. Recover the active WAL generation up to the last valid record.
5. Replay only complete committed transactions newer than the manifest's safe
   checkpoint sequence.
6. Rebuild bounded RAM indexes without allocating after the init allocation
   phase.
7. Detect capacity or geometry inconsistencies before publishing
   `initialized=true`.
8. Leave orphaned but valid unselected tables/WAL generations unselected; they
   may become reusable only after the authoritative state proves it is safe.

Recovery must not write repairs until the read-only analysis has identified one
unambiguous safe state. Separate analysis from repair so fault tests can reason
about both.

## Concurrency and visibility

**Proposed v1:**

- one lifecycle mutex;
- one serialized writer/transaction mutex;
- one reader mutex only where shared descriptors or scratch buffers require it;
- no background compaction;
- no lock held while application callbacks or unrelated network I/O run.

Document lock ordering and never invert it. Public getters must not return
pointers into mutable internal arenas. Copy values into caller-owned buffers.

The visibility point is not allowed to precede the successful WAL durability
barrier. Decide before coding whether uncommitted staged writes are visible:

- only to the same handle;
- to all handles in the same namespace; or
- to no getters until commit.

Do not assume NVS behavior here; specify and test the selected rule.

## Implementation phases

Each phase ends with tests and a review before the next on-disk dependency is
added.

### Phase 0: freeze requirements and budgets

- Resolve all open decisions listed below.
- Capture expected key count, namespace count, value distribution, maximum
  transaction, update rate, retention expectations, medium, and service life.
- Record RAM, boot-time, commit-latency, and storage-capacity budgets.
- Decide why custom storage is preferred over the dedicated-NVS,
  two-snapshot/control-record design recommended by the Soulcloud document.
  Use an NVS baseline measurement if the reason is performance or RAM.
- Write the complete durability and visibility contract in the README.

No on-disk format implementation starts before this phase is approved.

### Phase 1: component skeleton and pure format code

- Add `CMakeLists.txt`, `Kconfig`, public header, private definitions, README,
  and test targets.
- Define checked arithmetic, endian encoding/decoding, CRC, name validation,
  handle validation, and error codes.
- Keep persistent encoding separate from host-native packed structures where
  necessary.
- Add compile-time geometry checks and golden-byte format tests.
- Add a small reference/model implementation for tests without putting STL in
  production firmware code.

### Phase 2: bounded FATFS I/O and provisioning

- Implement exact bounded reads/writes, sync, path building, contiguity checks,
  canonical-name checks, and permanent-file provisioning.
- Implement redundant provisioning and ready manifests.
- Add interruption tests after every provisioning write/sync.
- Verify that steady-state operations never change file size or directory
  layout.

### Phase 3: handles, staging, WAL, and memtable

- Implement fixed-capacity generation-checked handles and namespaces.
- Implement typed staging with no operation-time allocation.
- Implement WAL encoding, atomic commit records, sync ordering, scan, and
  replay.
- Implement the bounded memtable and newest-value/tombstone lookup.
- Test resets at every WAL byte/write/sync/publication boundary.

At the end of this phase, the store may be WAL-only and capacity-limited. Do
not call it complete until WAL recycling is protected by SSTables.

### Phase 4: immutable SSTable flush

- Define and freeze SSTable entry, block, index, header, and footer formats.
- Sort memtable references in bounded scratch memory.
- Write, sync, validate, and publish an SSTable through a new manifest
  generation.
- Advance the WAL checkpoint only after the table is selected durably.
- Recover every old/new state around flush and manifest publication.

### Phase 5: compaction and fixed-capacity reuse

- Implement the approved level/slot policy and compaction reserve.
- Preserve newest-value semantics and tombstones.
- Publish compaction outputs copy-on-write through the manifest.
- Recycle table and WAL slots only after they are unreachable from every valid
  authoritative generation.
- Return typed full/busy errors when safe progress is impossible.
- Measure write amplification and worst-case synchronous latency.

### Phase 6: complete NVS-familiar API

- Complete all approved integer, string, blob, erase, stats, and iterator APIs.
- Validate mode, handle, name, type, length, and namespace behavior against the
  written contract.
- Add optional C wrapper only if approved.
- Add health/status reporting with bounded counters and no secret data.

### Phase 7: hardware and power-cut qualification

- Test the exact ESP32 target with the intended NOR flash and SD card models.
- Test NOR FATFS with the selected journaled mount configuration.
- Defer final journaled-SD power-cut qualification until the separately owned
  SD-card `esp_jrnl` port is available. Do not block database-format fault
  injection or NOR qualification on that port.
- Perform randomized physical power cuts during WAL append, sync, manifest
  publication, flush, compaction, and provisioning.
- Characterize rather than assume SD-card durability after `fsync()`.
- Record mount/recovery time, commit latency distribution, compaction stalls,
  stack use, steady-state heap delta, and write amplification.

## Verification requirements

### Deterministic tests

- all integer types, strings, empty strings, blobs, and zero-length blobs;
- maximum and over-limit namespace, key, value, and transaction sizes;
- type mismatch and WinAPI-style string/blob length queries;
- read-only handles, stale handles, handle exhaustion, and namespace isolation;
- repeated set, no-op set, delete, delete missing key, erase-all, and
  delete/recreate ordering;
- transaction atomicity with multiple keys and namespaces as approved;
- sequence wrap/exhaustion handling;
- WAL torn header, payload, checksum, commit record, and tail;
- either manifest copy corrupt or torn; both invalid;
- table header/footer/index/data corruption;
- interrupted flush and every compaction publication boundary;
- full WAL, full memtable, no free compaction output, and fixed-capacity full
  store;
- unknown older/newer format revisions and geometry mismatch;
- missing, resized, fragmented, renamed, extra canonical, and wrong-identity
  files;
- init/deinit repetition and forced shutdown behavior;
- concurrent readers/writer according to the selected visibility contract; and
- proof/instrumentation that component-owned steady-state operations allocate
  no heap.

Use fault injection capable of failing, shortening, tearing, reordering where
the medium permits, or resetting after every low-level write and sync. Remount
the resulting image and compare it with a simple committed-state model.

### Invariants to assert

- at most one manifest generation is selected as authoritative;
- every selected table was fully synced and validates;
- no published table slot is overwritten;
- every acknowledged transaction appears after recovery;
- no unacknowledged incomplete transaction appears after recovery;
- a key lookup returns the greatest committed sequence or a not-found
  tombstone;
- WAL recycling never discards a sequence newer than the safe checkpoint;
- file sizes and cluster chains never change during steady-state operation;
- memory use stays within the approved init-time allocation budget; and
- corruption produces an explicit error, never a silent format/reset.

## Phase 0 design decisions

Reviewed on 2026-07-29. Phase 1 is approved. Later phases must follow these
decisions and must ask before changing them.

### Motivation and platform scope

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

### Capacity, names, and workload

- Each permanent FAT32-compatible database file is at most `UINT32_MAX` bytes
  (4 GiB minus one byte). Total database capacity depends on the persisted file
  count and geometry, which are not yet frozen.
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
live key: total live namespaces and keys are bounded by fixed on-disk capacity
and persisted counters. Open handles, active transactions, staged mutations,
memtable entries, and compaction scratch space remain independently bounded RAM
resources.

### Memory and performance

- Runtime arenas are allocated from PSRAM during initialization.
- The default component-owned runtime-memory budget is 100 KiB.
- The configurable v1 hard ceiling is strictly less than 200 KiB.
- Exact arena partitioning is Phase 3 work. Normal operations must allocate no
  component-owned heap after initialization.
- SD-card initialization or recovery may take up to approximately three
  minutes. Long scans must yield or otherwise integrate with task-watchdog
  policy; disabling safety checks globally is not an acceptable shortcut.
- Foreground synchronous flush and compaction are accepted for v1.
- Commit/flush performance should improve on ESP-IDF NVS, but that target is
  not yet quantified. Before making a performance claim, benchmark both engines
  with the same logical workload and report initialization, get, set/commit,
  compaction, and tail latency.

### API and transaction semantics

- The primary v1 API is C++. A C wrapper is deferred.
- One namespace handle selects exactly one namespace.
- A v1 transaction is associated with one namespace handle. Atomic
  cross-namespace transactions are deferred.
- Commit is atomic across every mutation staged in the transaction.
- A transaction reads its own staged mutations. Other namespace handles see
  only committed state.
- Explicit transaction `close()` is an alias for commit and returns
  `esp_err_t`. A failed close leaves the transaction valid for retry or abort.
- Namespace close returns `ESP_ERR_ON9KVDB_BUSY` while its transaction is
  active.
- Destruction or abandonment aborts dirty transaction state rather than
  silently committing it.
- Typed integer, string, blob, erase-key, stats, and find-key operations are v1
  requirements.
- General iteration, erase-all, secure purge, encryption, namespace
  enumeration, partition-like multiple stores, and the C wrapper are deferred.

### Corruption, reset, and integration policy

- Corruption handling is fail-closed. Do not start services that require stored
  configuration; report a typed diagnostic and wait for maintenance.
- The database never automatically deletes files, formats FATFS, or replaces
  corrupt data with defaults.
- Deleting FATFS files is not `erase_all()` or normal factory reset.
- A later explicit logical reset must reinitialize structurally valid fixed
  files in place. Repairing or reformatting damaged FATFS is a separate
  platform maintenance action.
- At-rest encryption is not a v1 requirement.
- Soulcloud snapshot, command-deduplication, and terminal-result logic belongs
  above the generic KV API. Add that integration after the KV engine is usable;
  the Soulcloud client itself is not part of the current implementation.

### Decisions still required by later phases

These items do not reopen Phase 1, but they gate the phase that consumes them:

- Phase 2 must freeze manifest/WAL/table file sizes and counts, FAT type,
  cluster/sector geometry validation, the exact descriptor budget, and the
  supported ESP32 production targets.
- Phases 3–5 must freeze the arena split, memtable capacity, WAL capacity,
  SSTable levels/size ratio, compaction reserve, and the behavior when a
  transaction reaches its byte limit before ten mutations.
- Performance acceptance requires a measured NVS baseline rather than relying
  on recollection that some operations are `O(n)`.
- Final SD power-loss qualification waits for the separately owned journal
  port and testing on the selected endurance/SLC media.
- Soulcloud integration tests wait for enough of the generic database and
  client stack to exist, but the storage semantics in the Soulcloud
  requirements remain mandatory.

## Suggested source-file split

Do not create all files before the relevant phase, but keep this responsibility
split unless implementation evidence suggests a better one:

```text
on9kvdb.hpp                 public C++ API
on9kvdb_defs.hpp            fixed constants and persisted-format definitions
on9kvdb.cpp                 lifecycle, locks, handles, public dispatch
on9kvdb_io.cpp              bounded FATFS/POSIX I/O and provisioning
on9kvdb_manifest.cpp        manifest publication and recovery
on9kvdb_wal.cpp             WAL append, commit, scan, replay, recycling
on9kvdb_memtable.cpp        bounded staging/memtable/index
on9kvdb_table.cpp           SSTable build, validate, and lookup
on9kvdb_compaction.cpp      merge and slot-reuse policy
on9kvdb_crc.cpp             only if constexpr/private-header placement is poor
Kconfig
CMakeLists.txt
README.md
tests/
```

Avoid circular ownership. The manifest is authoritative, WAL protects recent
commits, the memtable is a RAM view, and SSTables are immutable published data.

## Acceptance criteria

The component is complete only when:

1. The approved NVS-familiar API and all limits are documented.
2. Every permanent file is provisioned contiguously at final size and never
   changes size afterward.
3. Component-owned steady-state paths perform no heap allocation.
4. Atomic commit and recovery behavior pass exhaustive boundary fault tests.
5. WAL flush/recycle and SSTable compaction never overwrite published state.
6. Namespace isolation, typed values, tombstones, and length-query behavior
   pass deterministic tests.
7. Corruption and capacity exhaustion fail explicitly without auto-formatting.
8. SD and NOR hardware results support the durability claims in the README.
9. The required journaled mount contract is documented, the application owns
   it, and database transaction correctness does not rely on journaling to
   replace the WAL or manifest protocol.
10. RAM, boot time, latency, write amplification, and service-life budgets are
    measured and accepted.
11. The Soulcloud transactional snapshot/control-record use case can be built
    above the API without weakening its command deduplication and recovery
    requirements.

## References

- Soulcloud requirements:
  `/home/hu/Projects/llm-docs/soulcloud/20260728-config-store-requirements.md`
- Existing component style: `../on9ringstore/`
- ESP-IDF NVS API:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html>
- ESP-IDF filesystem considerations:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/file-system-considerations.html>
- ESP-IDF FATFS API:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/fatfs.html>
- Espressif filesystem journal:
  <https://github.com/espressif/esp_jrnl>
