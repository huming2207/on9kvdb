# on9kvdb

`on9kvdb` is planned as a fixed-capacity, namespaced LSM key-value database
over permanent contiguous files on a journaled FATFS volume.

The component is currently at **Phase 1**. It contains public API declarations,
limits, error codes, checked arithmetic, little-endian encoding, CRC-32, name
validation, generation-checked handle encoding, and the common persisted-file
prefix. It does not yet provision files or provide a usable database.

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

The complete manifest, WAL-record, and SSTable formats are not frozen in Phase
1.

## Tests

Native Phase 1 tests:

```sh
cmake -S tests -B /tmp/on9kvdb-tests
cmake --build /tmp/on9kvdb-tests
ctest --test-dir /tmp/on9kvdb-tests --output-on-failure
```

ESP-IDF Unity test registration is under `test/` for integration into an IDF
unit-test application.
