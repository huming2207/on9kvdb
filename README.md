# on9kvdb

`on9kvdb` is planned as a fixed-capacity, namespaced LSM key-value database
over permanent contiguous files on a journaled FATFS volume.

The component is currently at **Phase 2**. It provisions and recovers the
permanent FATFS file set, persists immutable geometry in redundant manifests,
validates file identity/size/contiguity, and fails closed on incompatible or
corrupt layouts. It does not yet implement handles, WAL transactions,
memtables, SSTable contents, lookup, or compaction, so it is not yet a usable
key-value database.

## Default fixed geometry

New databases use build-time Kconfig geometry:

```text
CONFIG_ON9KVDB_MAX_LIVE_DATA_SIZE          2097152 bytes
CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE   4194304 bytes
CONFIG_ON9KVDB_WAL_FILE_SIZE                262144 bytes
CONFIG_ON9KVDB_SSTABLE_FILE_SIZE            610304 bytes
CONFIG_ON9KVDB_SSTABLE_COUNT                     6
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
with three table slots fails with
`ESP_ERR_ON9KVDB_INCOMPATIBLE_GEOMETRY` when opened by firmware configured
for six. The component does not migrate, delete, resize, or recreate it. The
application/user must deliberately remove the complete old database file set
before provisioning a replacement.

`MAX_LIVE_DATA_SIZE` and `PROVISIONED_DATABASE_SIZE` mean different things:

- Live bytes are the encoded newest committed, non-deleted records, including
  namespace, key, type, value, and record metadata.
- Provisioned bytes are the final sizes of all permanent database files,
  including WAL space, stale LSM versions, and compaction workspace.

The 4 MiB value is the exact sum of FATFS file lengths. FAT allocates complete
clusters, so actual volume consumption can be slightly larger: each file's
length is rounded up independently to the mounted cluster size. If the public
free-byte check does not account for enough cluster-tail slack, contiguous
creation fails with `ESP_ERR_ON9KVDB_NOT_ENOUGH_SPACE` rather than changing
the geometry.

For 32-bit values, the final exact key count depends on the Phase 4 entry and
index format. A current planning estimate is roughly 30,000–40,000 live pairs
with short names, or roughly 19,000–21,000 when both namespace and key are at
their 32-byte maxima. These are not format guarantees.

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
6. Publish and sync a newer `ready` manifest generation.

A reset before the first ownership sync leaves no durable ownership proof and
fails closed. After either provisioning manifest is durable, recovery first
restores two durable provisioning copies, then safely resumes missing
WAL/SSTable identity writes. A torn `ready` publication selects the older
provisioning generation and resumes; a valid `ready` publication is
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
  `ESP_ERR_ON9KVDB_BUSY`. Abandonment or destruction aborts rather than
  silently committing.
- Stats and find-key are required. General iteration is deferred.
- Corruption fails closed. The component never auto-formats.

The total number of live keys is bounded by fixed on-disk capacity, not by a
permanent full-database RAM index. A live key is the latest committed,
non-tombstoned `(namespace, key)` pair.

## Memory budget

The default component-owned runtime budget is 100 KiB, intended for PSRAM. The
v1 configuration ceiling is 204799 bytes, strictly below 200 KiB. Phase 3 will
partition this budget among handle tables, transaction staging, memtable
storage, indexes, and compaction scratch space.

Both mutation count and available staged bytes limit a transaction. Ten 8 KiB
values are not promised to fit in the 100 KiB default because the component
also needs committed memtable and index memory.

After successful initialization, component-owned normal operations and
compaction must not allocate heap memory.

## Durability and visibility contract

The future commit order is:

```text
validate/stage transaction
    -> append WAL mutations and commit record
    -> fsync WAL
    -> publish transaction to committed memtable
    -> return ESP_OK
```

Only a complete checksummed WAL transaction with a durable commit record is
replayed. Other handles cannot observe staged data. An acknowledged commit must
recover as committed; an incomplete commit must recover as uncommitted.

Filesystem journaling is supplied by the application/platform. Database-level
atomicity still comes from the WAL, manifest, checksums, and copy-on-write
SSTable publication.

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

Phase 2 additionally defines a 96-byte manifest record and a 56-byte permanent
file-identity record. All fields are explicitly little-endian and both records
have a second CRC covering the complete record.

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
0x40       8  maximum encoded live bytes
0x48       8  total provisioned bytes
0x50      12  reserved, zero in revision 1
0x5c       4  complete-record CRC-32
```

The valid record with the greatest generation is selected, provided all valid
copies agree on database identity and geometry.

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

Phase 3 WAL records and Phase 4 SSTable data begin after this 8192-byte
identity region. Their formats are not frozen by Phase 2.

## Tests

Native Phase 1 and Phase 2 tests:

```sh
cmake -S tests -B /tmp/on9kvdb-tests
cmake --build /tmp/on9kvdb-tests
ctest --test-dir /tmp/on9kvdb-tests --output-on-failure
```

ESP-IDF Unity test registration is under `test/` for integration into an IDF
unit-test application. The native Phase 2 model injects a reset after every
manifest/identity write and sync boundary. It verifies fail-closed behavior
before durable ownership and successful resumable provisioning after durable
ownership.
