# on9kvdb Implementation Plan

## Status and scope

This file is the design, implementation, and qualification plan for `on9kvdb`.
Sections explicitly marked approved are authoritative. Any future proposal or
open decision still requires user approval before implementation.

Phase 1 through Phase 6 are implemented. Phase 5 adds bounded foreground
full-compaction, redundant-manifest stabilization, and safe table/WAL reuse.
Phase 6 freezes the approved NVS-familiar API and bounded diagnostic snapshot.
Phase 7 is not complete: hardware media, journal integration, physical
power-cut behavior, and performance/endurance measurements remain release
qualification work.

The component is a fixed-capacity, namespaced key-value database for ESP32-class
devices. It exposes an API similar to ESP-IDF NVS while using an LSM tree over
files on an already-mounted, filesystem-journaled FATFS volume. The volume may
be on an SD card or NOR flash.

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
  later Kconfig must not resize or reinterpret it. V1 additionally requires
  every persisted geometry field to match the running firmware's Kconfig
  exactly; a mismatch fails initialization and requires an application-owned
  delete/recreate maintenance operation.
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
- four-space indentation and the same brace style;
- a 130-column default source width; keep related declarations, expressions,
  conditions, and function arguments on one line when they fit, and avoid
  narrow or eager line folding because it makes review harder;
- wrap only when a line would exceed 130 columns or a deliberate multiline
  layout materially improves clarity;
- do not use lambda expressions, including local inline lambdas; use named
  private member functions or private static helpers instead, and mark a
  helper `inline` when its size and call frequency justify it;
- format every changed C++ source/header with the component's `.clang-format`
  file before verification, and run its dry-run check so the 130-column rule
  remains enforced in future phases;
- add concise comments for non-obvious durability ordering, recovery
  invariants, bounded-memory algorithms, and format constraints; do not
  narrate routine control flow or make the source harder to audit;
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
compatible. Use an `on9kvdb_` prefix so it can coexist with NVS. The public API
uses only the standard `ESP_ERR_*` values from `esp_err.h`; it does not define
an engine-specific error range.

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

The implemented v1 uses no separate data-block read cache. It uses:

- a fixed transaction-staging arena;
- one fixed mutable memtable arena;
- a fixed open-addressed hash index or similarly bounded lookup index over the
  memtable;
- a fixed array of sortable entry references/offsets for SSTable flush; and
- bounded table descriptors and on-disk sparse indexes.

Foreground synchronous flush/compaction is the approved v1 policy. A second
immutable memtable or background worker would add concurrency, memory,
shutdown, and recovery states and requires explicit approval.

## V1 physical layout

Phase 2 freezes the permanent filenames, file counts/sizes, manifest slots,
and the two identity slots at the beginning of every WAL/SSTable file:

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

Every file is permanent, contiguous, and fixed-size. Storage revision 4 freezes
the WAL framing, SSTable block/index/footer layout, equal table banks, and
manifest publication fields documented below and in `README.md`.

### Manifest

`manifest.db` contains two independently checksummed, sector-aligned manifest
slots. A manifest generation is the sole publication point for:

- storage format version and feature flags;
- database identity;
- persisted geometry and limits;
- live SSTable slot IDs, logical generations, levels, key ranges, and sequence
  ranges;
- active WAL slot/generation and safe replay/checkpoint sequence;
- next logical transaction/table generation;
- provisioning/ready state.

Write and sync the alternating manifest slot before treating its generation as
current. Recovery selects the highest valid generation that is internally
consistent with referenced files. Do not mutate a published manifest slot in
place. Runtime health and compaction counters are deliberately not persisted.

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

Phase 3 freezes two redundant 4096-byte WAL-generation header slots after the
8192-byte file-identity region. Transaction data starts at offset 16384.
Transactions occupy one or more complete 4096-byte frames and never wrap.
Every frame is written once for the selected logical WAL generation; a later
transaction never shares or rewrites a frame containing an acknowledged
transaction. The final frame carries the commit flag, and every frame binds
the database identity, WAL generation, transaction sequence, frame index/count,
payload bounds, transaction checksum, and frame checksum.

The current engine alternates between two logical WAL generations. Before
overwriting a previously used slot, full compaction checkpoints its reachable
transactions and both manifest copies are stabilized with that slot
unreferenced.

### SSTable slots

An SSTable is immutable after it is published by the manifest. It contains:

- redundant headers and separately verifiable footer metadata;
- sorted composite internal keys;
- type, transaction sequence, tombstone state, and value bytes;
- checksummed data blocks, sparse index, headers, and footer;
- a bounded sparse index;
- a checksum covering all authoritative table metadata.

V1 has no Bloom filter or compression. Either feature requires measurement and
explicit approval.

Compaction must be copy-on-write:

1. Select only currently free output table slots.
2. Merge live input tables and tombstones into the output slots.
3. Sync and validate every output.
4. Publish one new manifest generation selecting the outputs and no longer
   selecting the inputs.
5. Stabilize both manifest copies on the new selection.
6. Reuse old input slots only after neither valid manifest references them.

Reserve enough unselected table capacity to complete worst-case compaction.
Returning `ESP_ERR_NO_MEM` is preferable to overwriting a live table.

## Implemented logical model

The logical identity and ordering key is:

```text
(namespace bytes, key bytes)
```

Each memtable or SSTable source contains at most one version of that composite
key. When multiple published sources contain it, the greatest committed
transaction sequence wins. Persistent encoding is explicitly little-endian and
does not depend on C++ object layout, locale, pointer size, or structure
padding.

Lookup considers:

1. the calling transaction's staged overlay for transaction getter overloads;
2. the committed mutable memtable;
3. every manifest-selected immutable SSTable whose key range may match.

The greatest matching committed sequence decides the result. A tombstone means
not found.

## Approved public API surface

Keep the lifetime of the database instance explicit because storage is a FATFS
path rather than an NVS partition.

The v1 API is a C++ instance API with opaque generation-checked namespace and
transaction handles. It provides:

- explicit `init()` and `deinit()`;
- namespace `open()` and `close()`;
- one explicit atomic transaction at a time with `begin()`, `commit()`,
  transaction `close()`, and `abort()`;
- signed and unsigned 8-, 16-, 32-, and 64-bit setters/getters;
- string and blob setters/getters, including length queries;
- `erase_key()` and `find_key()`; and
- `get_stats()` for bounded counters, capacities, topology, and manifest
  recovery state.

Required NVS-familiar behavior:

- opening a namespace returns an opaque generation-checked handle;
- a read-only handle rejects mutations;
- setters are typed and type mismatch is reported;
- `get_str()` and `get_blob()` support a null output buffer length query;
- too-small output reports required length without overflow;
- `erase_key()`, `commit()`, transaction `close()`, `abort()`, and handle
  `close()` have documented uncommitted-change behavior;
- closing a handle does not silently convert an unsuccessful commit into
  success;
- handles have fixed capacity and stale handles cannot alias newly opened ones
  without a generation check.

One `commit()` is atomic across every staged mutation. General iterators,
`erase_all()`, namespace enumeration, secure purge, encryption, and a C wrapper
are deferred.

Do not reuse `ESP_ERR_NVS_*` values for a different engine and do not define
new component errors. V1 maps outcomes to existing values:

- invalid name, handle, or null argument: `ESP_ERR_INVALID_ARG`;
- type mismatch or structurally invalid decoded data:
  `ESP_ERR_INVALID_RESPONSE`;
- missing namespace or key: `ESP_ERR_NOT_FOUND`;
- read-only mutation: `ESP_ERR_NOT_ALLOWED`;
- value, transaction, geometry, or destination-buffer size mismatch:
  `ESP_ERR_INVALID_SIZE`;
- fixed RAM, memtable, namespace, or storage capacity exhausted:
  `ESP_ERR_NO_MEM`;
- checksum corruption: `ESP_ERR_INVALID_CRC`;
- unsupported stored format revision: `ESP_ERR_INVALID_VERSION`; and
- invalid lifecycle, busy handle, or sequence exhaustion:
  `ESP_ERR_INVALID_STATE`.

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
4. Recover referenced WAL generations in logical-generation order up to each
   last valid record.
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

The implemented v1 uses one lifecycle mutex and one serialized operation mutex.
There is one active writer transaction globally and no background compaction.
Lock ordering is lifecycle then operation; no application callback or unrelated
network I/O runs under either lock.

Public getters copy into caller-owned buffers. A transaction reads its own
staged overlay. Every other handle, including another handle for the same
namespace, sees only committed state. Committed visibility never precedes the
successful WAL durability barrier.

## Implementation phases

Phases 0 through 6 are complete. Phase 7 is pending. The phase descriptions
remain as the required implementation and review record.

### Phase 0: freeze requirements and budgets

- Resolve the workload, durability, API, and capacity decisions recorded below.
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

- Complete all approved integer, string, blob, erase, find, and stats APIs.
- Validate mode, handle, name, type, length, and namespace behavior against the
  written contract.
- Add health/status reporting with bounded counters and no secret data.
- Keep iterators, `erase_all()`, namespace enumeration, encryption, and the C
  wrapper deferred.

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

## Current verification status

Completed on 2026-07-29:

- The ESP32-S3 demo firmware builds with the current component and revision-4
  format.
- Native Phase 1 through Phase 5 format, fault-model, capacity, and publication
  tests pass.
- A separate ESP-IDF Unity compile application validates the Phase 6 public
  overloads and diagnostic structure.
- `git diff --check`, the 130-column audit, and the no-lambda audit pass.

These checks do not replace Phase 7. The complete database has not yet been
qualified on mounted NOR/SD FATFS with the intended journal, physical reset
injection, endurance media, watchdog policy, or measured latency/write
amplification.

## Verification requirements

### Deterministic tests

- all integer types, strings, empty strings, blobs, and zero-length blobs;
- maximum and over-limit namespace, key, value, and transaction sizes;
- type mismatch and WinAPI-style string/blob length queries;
- read-only handles, stale handles, handle exhaustion, and namespace isolation;
- repeated set, no-op set, delete, delete missing key, and delete/recreate
  ordering;
- transaction atomicity with multiple keys in its single associated namespace;
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

Reviewed on 2026-07-29. Phase 0 decisions and the Phase 1 through Phase 6
implementation policies are approved. Later changes must follow these
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

### Memory and performance

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
- Namespace close returns `ESP_ERR_INVALID_STATE` while its transaction is
  active.
- Destruction or abandonment aborts dirty transaction state rather than
  silently committing it.
- Typed integer, string, blob, erase-key, stats, and find-key operations are v1
  requirements.
- General iteration, erase-all, secure purge, encryption, namespace
  enumeration, partition-like multiple stores, and the C wrapper are deferred.

### Phase 3 handles, RAM, WAL, and namespace decisions

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

### Phase 4 immutable SSTable decisions

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

### Phase 5 compaction and reuse decisions

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

### Phase 6 API and diagnostics decisions

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

### Corruption, reset, and integration policy

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

### Remaining integration and qualification decisions

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

## Suggested source-file split

Do not create all files before the relevant phase, but keep this responsibility
split unless implementation evidence suggests a better one:

```text
on9kvdb.hpp                 public C++ API
on9kvdb_defs.hpp            fixed constants and persisted-format definitions
on9kvdb.cpp                 lifecycle, locks, handles, public dispatch
on9kvdb_io.cpp              bounded FATFS/POSIX I/O and provisioning
on9kvdb_manifest.cpp        manifest publication and recovery
on9kvdb_wal.cpp             WAL append, commit, scan, replay, slot transition
on9kvdb_memtable.cpp        bounded staging/memtable/index
on9kvdb_table.cpp           SSTable build, validate, and lookup
on9kvdb_compaction.cpp      merge and slot-reuse policy
Kconfig
CMakeLists.txt
README.md
tests/
```

Avoid circular ownership. The manifest is authoritative, WAL protects recent
commits, the memtable is a RAM view, and SSTables are immutable published data.

## Acceptance criteria

The component is release-qualified only when:

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
