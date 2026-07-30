# on9kvdb Requirements and Constraints

## Status and scope

This document records the mandatory scope, constraints, platform assumptions,
and runtime architecture for `on9kvdb`. Sections explicitly marked approved
are authoritative. Any future proposal or open decision still requires user
approval before implementation.

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

- `../../on9ringstore/on9rstore.hpp`
- `../../on9ringstore/on9rstore_defs.hpp`
- `../../on9ringstore/on9rstore.cpp`
- `../../on9ringstore/on9rstore_io.cpp`
- `../../on9ringstore/on9rstore_manifest.cpp`
- `../../on9ringstore/on9rstore_segment.cpp`
- `../../on9ringstore/on9rstore_entry.cpp`
- `../../on9ringstore/README.md`
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

The 2026-07-30 performance pass adds an optional decoded sparse-index cache
for every configured physical SSTable slot plus one stable maximum-value-sized
lookup buffer. Both are carved during `init()` only when the caller's
configured total runtime budget has enough space beyond all required
partitions. The approved 100-KiB default keeps the checked disk-index and
winning-table reread fallback. The cache has no dynamic allocation or
eviction, never substitutes for data-block CRC validation, and is populated
only after complete table validation.

Foreground synchronous flush/compaction is the approved v1 policy. A second
immutable memtable or background worker would add concurrency, memory,
shutdown, and recovery states and requires explicit approval.
