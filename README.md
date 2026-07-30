# on9kvdb

`on9kvdb` is planned as a fixed-capacity, namespaced LSM key-value database
over permanent contiguous files on a journaled FATFS volume.

## Development provenance

This project is vibe-coded by GPT-5 Codex and reviewed by Kimi K3, tested by human (me).

The component is currently at **Phase 6**. It has fixed-capacity namespace and
transaction handles, typed staging, read-your-writes, recoverable atomic WAL
transactions, automatic immutable SSTable flush, and committed lookup across
the memtable and every published table.

Phase 5 adds foreground full compaction and safe table/WAL reuse. The six
SSTables form two banks of three files. Compaction writes and validates the
inactive bank before atomically selecting it through the manifest; the old
bank is not reusable until both redundant manifest copies no longer reference
it.

## Engineering documentation

The audit-oriented engineering contract is split by topic under
[`doc/`](doc/):

- [`doc/REQUIREMENTS.md`](doc/REQUIREMENTS.md) records mandatory constraints.
- [`doc/STORAGE_FORMAT.md`](doc/STORAGE_FORMAT.md) records persistent layout
  and durability ordering.
- [`doc/API_AND_RECOVERY.md`](doc/API_AND_RECOVERY.md) records public behavior
  and recovery.
- [`doc/DESIGN_DECISIONS.md`](doc/DESIGN_DECISIONS.md) records approved design
  choices.
- [`doc/IMPLEMENTATION_AND_TESTING.md`](doc/IMPLEMENTATION_AND_TESTING.md)
  records invariants and qualification requirements.
- [`doc/PERFORMANCE_COMPARISON.md`](doc/PERFORMANCE_COMPARISON.md) records
  complexity, the measured ESP-IDF NVS comparison, and bounded RAM usage.
- [`doc/NEXT_STEPS.md`](doc/NEXT_STEPS.md) separates performance follow-up
  work from the approved durability contract.

Start with [`doc/AGENTS.md`](doc/AGENTS.md) for the audit order and authority
rules.

## Default fixed geometry

New databases use build-time Kconfig geometry:

```text
CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE           786432 bytes
CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE   4194304 bytes
CONFIG_ON9KVDB_WAL_FILE_SIZE                262144 bytes
CONFIG_ON9KVDB_SSTABLE_FILE_SIZE            610304 bytes
CONFIG_ON9KVDB_SSTABLE_COUNT                     6
CONFIG_ON9KVDB_SSTABLE_BLOCK_SIZE            12288 bytes
```

The default file-size arithmetic is exact:

```text
manifest.db                 8192 bytes
wal_0.db + wal_1.db       524288 bytes
six SSTables             3661824 bytes
                         -------------
total                    4194304 bytes
```

Every size is a multiple of 4096 bytes. Configuration is rejected at build
time and again during `init()` if the file sum differs from the provisioned
total, alignment is invalid, a file exceeds the FAT32 limit, or the geometry
cannot reserve its identity region.

All geometry fields are persisted. V1 requires the persisted geometry to
match the running firmware's Kconfig exactly. For example, a database created
with three table slots fails with `ESP_ERR_INVALID_SIZE` when opened by
firmware configured for six. The component does not migrate, delete, resize,
or recreate it. The application/user must deliberately remove the complete
old database file set before provisioning a replacement.

Phase 5 uses storage revision 4. Earlier development-format databases are not
migrated and return `ESP_ERR_INVALID_VERSION`; remove the complete database
file set deliberately before provisioning a replacement.

`MAX_LIVE_DATA_SIZE` and `PROVISIONED_DATABASE_SIZE` mean different things:

- Logical-state bytes are the encoded newest committed records, including
  tombstones, namespace, key, type, value, and record metadata. The historical
  Kconfig field name remains `MAX_LIVE_DATA_SIZE`.
- Provisioned bytes are the final sizes of all permanent database files,
  including WAL space, stale LSM versions, and compaction workspace.

The 4 MiB value is the exact sum of FATFS file lengths. FAT allocates complete
clusters, so actual volume consumption can be slightly larger: each file's
length is rounded up independently to the mounted cluster size. If the public
free-byte check does not account for enough cluster-tail slack, contiguous
creation fails with `ESP_ERR_NO_MEM` rather than changing the geometry.

The 768 KiB default is a conservative hard admission bound for arbitrary
permitted record-size mixtures. Records do not span 12 KiB blocks, so the
bound includes worst-case block packing and leaves both deletion state and
publication safety governed by the same limit.

## Phase 2 file provisioning

`init()` operates on an already-mounted FATFS base path and creates:

```text
<base path>/
├── manifest.db
├── wal_0.db
├── wal_1.db
├── table_0.db
├── table_1.db
├── table_2.db
├── table_3.db
├── table_4.db
└── table_5.db
```

Each file is created once at its final size using
`esp_vfs_fat_create_contiguous_file(..., true)`, then checked with `stat()` and
`esp_vfs_fat_test_contiguous_file()`. After provisioning, the component never
grows, truncates, renames, unlinks, or replaces a healthy file.

The manifest, both WALs, and every SSTable remain open after successful
initialization. The FATFS mount therefore needs at least
`CONFIG_ON9KVDB_SSTABLE_COUNT + 3` file descriptors for one database, plus
descriptors used by the application.

For the intended low-write-rate production build, disable
`CONFIG_FATFS_PER_FILE_CACHE`; otherwise FATFS may allocate one sector cache
for every permanently open file. The current demo disables that option.

Before new-store creation, canonical `manifest.db`, `wal_*.db`, and
`table_*.db` collisions are rejected. A ready store fails closed on missing,
extra canonical, wrongly sized, fragmented, wrong-identity, unsupported, or
structurally inconsistent files.

Provisioning order is:

1. Check build geometry, the mounted volume, free space, paths, and canonical
   filename ownership.
2. Allocate the 8 KiB manifest contiguously.
3. Write and sync two independently checksummed
   `provisioning_owned` manifest generations.
4. Allocate every WAL/SSTable file and write two independently synced identity
   copies.
5. Re-read and validate every identity.
6. Write and sync two WAL-generation header copies in WAL slot 0.
7. Publish and sync a newer `ready` manifest generation that references WAL
   generation 1.

A reset before the first ownership sync leaves no durable ownership proof and
fails closed. If the reset interrupted allocation of `manifest.db` itself and
left a nonzero partial canonical file, maintenance must remove the incomplete
database file set while the component is closed; the component cannot safely
infer ownership and truncate it. After either provisioning manifest is
durable, recovery first restores two durable provisioning copies, then safely
resumes missing WAL/SSTable identity writes. A torn `ready` publication selects
the older provisioning generation and resumes; a valid `ready` publication is
authoritative.

## FATFS platform contract

V1 supports FAT32 on ESP32-family targets with 512-byte or 4096-byte logical
sectors. The current project build and initial qualification target are
ESP32-S3 with ESP-IDF v6.0.2.

The application owns mounting, journaling, media lifetime, and maintenance. It
must provide:

- FAT32 with two FAT copies (`use_one_fat=false` when formatting);
- 512-byte or 4096-byte logical sectors;
- a suitable filesystem journaling layer;
- enough contiguous free space; and
- at least `SSTABLE_COUNT + 3` database descriptors plus application margin.

ESP-IDF's public mounted-FATFS API does not expose FAT subtype, actual sector
or cluster size, FAT-copy count, or the mount's `max_files`. The component
validates what the public API exposes through `esp_vfs_fat_info()`, `stat()`,
and the contiguous-file test. The remaining properties are an explicit
platform contract rather than a dependency on private FatFs/VFS structures.

## Approved v1 behavior

- One namespace handle selects one namespace.
- Opaque handles keep their 32-bit ABI but use nine slot-token bits and a
  non-wrapping 23-bit generation. Exhausting a generation counter returns
  `ESP_ERR_INVALID_STATE` rather than aliasing an old handle; the default
  workload would take more than a millennium to reach that boundary.
- Namespace and key names contain 1 through 32 bytes, excluding the C-string
  terminator.
- Integers, strings, and blobs follow an NVS-familiar typed API.
- Strings and blobs are limited to 8192 bytes. Streaming and fragmented values
  are not supported in v1. A persisted string length includes its terminating
  NUL, matching NVS-style string length queries; a string therefore contains
  at most 8191 non-NUL bytes.
- A transaction contains at most 10 mutations and is associated with one
  namespace handle.
- A transaction sees its own staged writes. Other handles see only committed
  state.
- A transaction commit is atomic across all its staged mutations.
- Explicit transaction `close()` is an alias for commit and returns
  `esp_err_t`. A failed close leaves the transaction valid for retry or abort.
  Closing a namespace with an active transaction returns
  `ESP_ERR_INVALID_STATE`. Abandonment or destruction aborts rather than
  silently committing.
- Stats and find-key are required. General iteration is deferred.
- Corruption fails closed. The component never auto-formats.

The total number of live keys is bounded by fixed on-disk capacity, not by a
permanent full-database RAM index. A live key is the latest committed,
non-tombstoned `(namespace, key)` pair.

## Memory budget

The default component-owned runtime budget is 100 KiB. One 4096-byte
DMA-capable I/O frame uses internal RAM; the remaining arena is allocated from
PSRAM by default. Initialization returns `ESP_ERR_NOT_SUPPORTED` when
`CONFIG_ON9KVDB_REQUIRE_PSRAM=y` but the application has not enabled PSRAM.
It does not silently consume the arena from internal RAM.

Default runtime partitions include 64 namespaces, 8 open handles, one active
transaction with 10 mutation slots and 24576 staged value bytes, a 512-bucket
memtable index, 36864 bytes of committed record storage, two 12288-byte table
blocks, 512 sortable 32-bit record offsets, and bounded compaction cursors. The
v1 configuration ceiling is 204799 bytes, strictly below 200 KiB.

Both mutation count and available staged bytes limit a transaction. Ten 8 KiB
values are not promised to fit in the 100 KiB default because the component
also needs committed memtable and index memory.

After successful initialization, component-owned normal operations and
compaction must not allocate heap memory.

## Durability and visibility contract

The commit/flush order is:

```text
validate/stage transaction
    -> if required, write and validate a free active-bank table
    -> publish the table and checkpoint through the manifest
    -> append WAL mutations and commit record
    -> fsync WAL
    -> publish transaction to committed memtable
    -> return ESP_OK
```

When the active table bank is full, foreground compaction merges its tables
and the committed memtable into the inactive bank. Every output is synced and
validated before one manifest publication selects the new bank. A second
manifest publication stabilizes the selection before old table or WAL files
can be reused.

Only a complete checksummed WAL transaction with a durable commit record is
replayed. Other handles cannot observe staged data. An acknowledged commit must
recover as committed; an incomplete commit must recover as uncommitted.

Filesystem journaling is supplied by the application/platform. Database-level
atomicity still comes from the WAL, manifest, checksums, and copy-on-write
SSTable publication.

### Performance model

Every non-empty commit appends at least one complete 4096-byte WAL frame and
calls `fsync()` before publishing the mutations in RAM. A one-byte set and an
erase tombstone therefore have almost the same fixed write cost. On SPI NOR
behind FATFS and wear levelling, that path is expected to be much slower than
NVS appending a compact entry directly to its partition.

The default 256 KiB WAL has 240 KiB of frame space, or 60 one-frame
transactions. WAL rotation can synchronously flush or compact tables and
publish manifests, producing the multi-second tail-latency samples seen in the
demo. Applications can select larger persisted WAL geometry; the performance
demo uses two 1 MiB WALs.

After records move out of the memtable, reads may also scan SSTables. A caller
that supplies enough arena space beyond the approved 100 KiB default gets a
bounded decoded sparse-index cache for every physical table slot and a stable
maximum-value-sized lookup buffer. The checked disk-index and winning-table
reread path remains available when the optional arena partition does not fit.
The cache never replaces block CRC verification and is rebuilt only from a
fully validated table.

Without weakening durability or changing storage revision 4, applications can
amortize the fixed WAL sync by grouping up to ten related mutations in one
transaction. SSTable bodies and both redundant metadata copies are written
while unreachable, synchronized once as a complete file, validated, and only
then published by a separately synchronized manifest. Redundant headers for a
new unreachable WAL generation are likewise synchronized together.

A smaller WAL frame, deferred/group commit, or a raw-partition WAL remains a
future design requiring separate measurement and approval. Removing the WAL
or manifest durability `fsync()` is not a valid optimization for durable
commits.

## Common persisted prefix

Phase 1 defines a 28-byte little-endian prefix. Code must encode and decode it
explicitly; it must not `memcpy()` the host-native packed structure to disk.

```text
offset  size  field
0x00       4  magic
0x04       2  storage revision
0x06       2  prefix size = 28
0x08       2  file kind
0x0a       2  flags
0x0c       8  logical generation
0x14       4  payload size
0x18       4  CRC-32
```

The CRC covers all 28 bytes with the CRC field treated as zero.

Current magic bytes are:

```text
manifest  "KVM9"
WAL       "KVW9"
SSTable   "KVT9"
```

The component uses the complete 4096-byte manifest slot and a 56-byte permanent
file-identity record. All fields are explicitly little-endian and both records
have a CRC covering the complete authoritative encoding.

`manifest.db` contains two 4096-byte slots. The beginning of each slot is:

```text
offset  size  field
0x00      28  common file prefix
0x1c       8  database identity
0x24       2  state: provisioning_owned or ready
0x26       2  geometry revision
0x28       4  format alignment
0x2c       4  manifest file size
0x30       4  WAL file size
0x34       4  WAL file count
0x38       4  SSTable file size
0x3c       4  SSTable file count
0x40       8  maximum encoded logical-state bytes
0x48       8  total provisioned bytes
0x50       2  logical-limits revision
0x52       2  reserved, zero
0x54       4  WAL frame bytes
0x58       4  maximum durable namespaces
0x5c       4  maximum open handles
0x60       4  memtable bucket count
0x64       4  memtable data bytes
0x68       4  maximum transaction mutations
0x6c       4  transaction staging bytes
0x70       4  SSTable logical block bytes
0x74       4  active WAL slot
0x78       8  WAL slot 0 generation
0x80       8  WAL slot 1 generation
0x88       8  safe checkpoint sequence
0x90       8  next logical table generation
0x98       4  active table count
0x9c       4  active table bank: 0 or 1
0xa0    2944  sixteen fixed table-reference descriptors
0xc20    988  reserved, zero
0xffc      4  complete-record CRC-32
```

The valid record with the greatest generation is selected, provided all valid
copies agree on database identity and geometry. Adjacent generations may
temporarily select different banks or WAL sets only across an interrupted
copy-on-write publication; recovery validates the selected files and writes a
stabilizing manifest before reuse.

Every WAL and SSTable reserves its first two 4096-byte slots for permanent
identity copies:

```text
offset  size  field
0x00      28  common file prefix
0x1c       8  database identity
0x24       8  physical file size
0x2c       4  physical slot number
0x30       4  reserved, zero in revision 1
0x34       4  complete-record CRC-32
```

Each WAL reserves two additional 4096-byte WAL-generation header slots after
the identity region. Transaction frames start at byte 16384. Every frame is
exactly 4096 bytes, has a 64-byte header, and is protected by payload and
whole-frame CRCs. Multi-frame transactions carry a transaction-wide CRC and
only the last frame carries the commit flag. Recovery publishes a transaction
only after validating all frames and the transaction checksum.

Each default 596 KiB SSTable has this fixed layout:

```text
offset       size  purpose
0x00000      8192  two permanent file identities
0x02000      8192  two redundant table headers
0x04000    577536  47 fixed 12 KiB data-block positions
0x91000     12288  sparse index block
0x94000      4096  footer slot
```

Records are sorted by `(namespace bytes, key bytes)`. Each record carries its
transaction sequence, type, tombstone flag, explicit lengths, and value. A
record never spans blocks. Data blocks, the index, footer, and headers are
checksummed; the manifest reference also binds their generation, physical
slot, key/sequence ranges, counts, and aggregate content checksum.

A level-0 flush writes an unreferenced slot in the active bank, syncs and
readback-validates it, then publishes one manifest generation that activates
the table and advances the safe checkpoint. A full compaction writes only the
opposite bank and keeps the newest version of every composite key, including
the newest tombstone. The old bank remains untouched until the new selection
exists in both manifest copies.

## Minimal API flow

```cpp
on9kvdb database("/sdcard/config", nullptr);
ESP_ERROR_CHECK(database.init());

on9kvdb_handle settings;
ESP_ERROR_CHECK(database.open(
    "settings", on9kvdb_open_mode::read_write, &settings));

on9kvdb_transaction_handle transaction;
ESP_ERROR_CHECK(database.begin(settings, &transaction));
ESP_ERROR_CHECK(database.set_u32(transaction, "boot_count", 1));
ESP_ERROR_CHECK(database.set_str(transaction, "mode", "normal"));
ESP_ERROR_CHECK(database.commit(transaction));

uint32_t boot_count = 0;
ESP_ERROR_CHECK(database.get_u32(
    settings, "boot_count", &boot_count));
ESP_ERROR_CHECK(database.close(settings));
ESP_ERROR_CHECK(database.deinit());
```

Only standard ESP-IDF errors are returned. In particular, missing data uses
`ESP_ERR_NOT_FOUND`, invalid handles/names use `ESP_ERR_INVALID_ARG`, type
mismatch uses `ESP_ERR_INVALID_RESPONSE`, capacity exhaustion uses
`ESP_ERR_NO_MEM`, and corrupt or unsupported storage uses
`ESP_ERR_INVALID_CRC` or `ESP_ERR_INVALID_VERSION`.

`get_stats()` returns a lock-consistent, non-secret diagnostic snapshot. It
includes logical counts and byte use, compaction totals/latency, configured
logical and provisioned capacity, runtime/scratch memory, active table/WAL
topology, manifest generation/checkpoint, redundant-copy count, and whether a
manifest stabilization barrier or latched storage fault remains. Compaction
totals are saturating per-boot counters; they are not persisted.

If a manifest write or sync fails, the component latches a storage fault because
it cannot prove whether the new generation became durable. Subsequent normal
operations return `ESP_ERR_INVALID_STATE`; call `deinit(true)` and then
`init()` so recovery can select and validate the authoritative generation
before reuse. `get_stats()` remains available while this fault is latched.

General iterators and `erase_all()` are intentionally deferred. Removing the
complete fixed file set is an application maintenance operation performed only
while the database is closed; it is not equivalent to a transactional API
erase.

## Tests

Native Phase 1 through Phase 5 tests:

```sh
cmake -S tests -B /tmp/on9kvdb-tests
cmake --build /tmp/on9kvdb-tests
ctest --test-dir /tmp/on9kvdb-tests --output-on-failure
```

ESP-IDF Unity test registration is under `test/` for integration into an IDF
unit-test application. The Phase 2 model injects a reset after every
manifest/identity write and sync boundary. Phase 3 tests every interrupted byte
prefix across single- and multi-frame WAL transactions and every single-byte
corruption in a complete frame. They verify that recovery can recognize the
whole transaction or no transaction, never a partial transaction. Phase 4
adds immutable-table codec and publication tests. Phase 5 verifies the
hard-capacity geometry and the stabilizing-manifest boundary required before
table-bank or WAL reuse.

The ESP-IDF Unity component test also compiles the Phase 6 overloaded getter
signatures and checks the default opaque-handle and diagnostic structures.
Runtime media and power-cut testing remains Phase 7 work.

## License

`on9kvdb` is released under the [MIT License](LICENSE).
