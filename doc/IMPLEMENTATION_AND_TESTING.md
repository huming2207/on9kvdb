# on9kvdb Implementation and Verification

> **vNext note:** The active acceptance surface is the binary API and value
> reader/writer design in [`BINARY_VALUE_API.md`](BINARY_VALUE_API.md). Legacy
> typed-accessor phases below are not part of the current implementation.

Phases 0 through 6 are complete. Phase 7 is pending. The phase descriptions
remain as the required implementation and review record.

## Phase 0: freeze requirements and budgets

- Resolve the workload, durability, API, and capacity decisions recorded below.
- Capture expected key count, namespace count, value distribution, maximum
  transaction, update rate, retention expectations, medium, and service life.
- Record RAM, boot-time, commit-latency, and storage-capacity budgets.
- Decide why custom storage is preferred over the dedicated-NVS,
  two-snapshot/control-record design recommended by the Soulcloud document.
  Use an NVS baseline measurement if the reason is performance or RAM.
- Write the complete durability and visibility contract in the README.

No on-disk format implementation starts before this phase is approved.

## Phase 1: component skeleton and pure format code

- Add `CMakeLists.txt`, `Kconfig`, public header, private definitions, README,
  and test targets.
- Define checked arithmetic, endian encoding/decoding, CRC, name validation,
  handle validation, and error codes.
- Keep persistent encoding separate from host-native packed structures where
  necessary.
- Add compile-time geometry checks and golden-byte format tests.
- Add a small reference/model implementation for tests without putting STL in
  production firmware code.

## Phase 2: bounded FATFS I/O and provisioning

- Implement exact bounded reads/writes, sync, path building, contiguity checks,
  canonical-name checks, and permanent-file provisioning.
- Implement redundant provisioning and ready manifests.
- Add interruption tests after every provisioning write/sync.
- Verify that steady-state operations never change file size or directory
  layout.

## Phase 3: handles, staging, WAL, and memtable

- Implement fixed-capacity generation-checked handles and namespaces.
- Implement typed staging with no operation-time allocation.
- Implement WAL encoding, atomic commit records, sync ordering, scan, and
  replay.
- Implement the bounded memtable and newest-value/tombstone lookup.
- Test resets at every WAL byte/write/sync/publication boundary.

At the end of this phase, the store may be WAL-only and capacity-limited. Do
not call it complete until WAL recycling is protected by SSTables.

## Phase 4: immutable SSTable flush

- Define and freeze SSTable entry, block, index, header, and footer formats.
- Sort memtable references in bounded scratch memory.
- Write, sync, validate, and publish an SSTable through a new manifest
  generation.
- Advance the WAL checkpoint only after the table is selected durably.
- Recover every old/new state around flush and manifest publication.

## Phase 5: compaction and fixed-capacity reuse

- Implement the approved level/slot policy and compaction reserve.
- Preserve newest-value semantics and tombstones.
- Publish compaction outputs copy-on-write through the manifest.
- Recycle table and WAL slots only after they are unreachable from every valid
  authoritative generation.
- Return typed full/busy errors when safe progress is impossible.
- Measure write amplification and worst-case synchronous latency.

## Phase 6: complete NVS-familiar API

- Complete all approved integer, string, blob, erase, find, and stats APIs.
- Validate mode, handle, name, type, length, and namespace behavior against the
  written contract.
- Add health/status reporting with bounded counters and no secret data.
- Keep iterators, `erase_all()`, namespace enumeration, encryption, and the C
  wrapper deferred.

## Phase 7: hardware and power-cut qualification

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

Additional demo integration completed on 2026-07-30:

- The ESP32-S3 demo can select either its wear-levelled NOR FATFS partition or
  a native-SDMMC FATFS card through Kconfig.
- Separate NOR, one-bit SDMMC, and four-bit SDMMC configurations compile
  successfully.
- SD-card hardware behavior, latency, endurance, removal handling, and
  power-loss behavior remain unqualified.

Additional recovery optimization completed on 2026-07-30:

- Namespace and logical-statistics reconstruction uses a bounded multiway
  merge over validated sorted SSTables. It no longer performs a cross-table
  lookup for every immutable record.
- The demo functional recovery check compares logical counts and byte totals
  before and after reinitialization.

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
- Existing component style: `../../on9ringstore/`
- ESP-IDF NVS API:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html>
- ESP-IDF filesystem considerations:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/file-system-considerations.html>
- ESP-IDF FATFS API:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/fatfs.html>
- Espressif filesystem journal:
  <https://github.com/espressif/esp_jrnl>
