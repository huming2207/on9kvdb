# on9kvdb

`on9kvdb` is a fixed-memory, LSM-style binary KV database for ESP-IDF. It
stores records in an application-supplied raw block range; it does not depend
on FATFS, VFS, paths, file descriptors, mounting, formatting, or a storage
driver's wear-levelling policy.

## Integration

Initialize the physical transport first, map an exclusive raw range with an
`on9kvdb_io` implementation, and keep both alive until `deinit()` finishes.

```cpp
on9kvdb_io_sdmmc device;
ESP_ERROR_CHECK(device.init(&card, first_lba));

on9kvdb database(&device, nullptr);
ESP_ERROR_CHECK(database.init());
```

`on9kvdb_io` exposes only complete database-visible blocks. Block zero is the
start of the exclusive database range, not necessarily physical LBA zero.
`on9kvdb_io_sdmmc` is the supplied native-SDMMC adapter; the application still
owns host/slot/card initialization and deinitialization.

The range must be at least `CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE` and use
a block size that divides 4096 bytes. It must not be mounted as a filesystem
or shared with any other consumer. First use accepts only a range uniformly
filled with `0x00` or `0xff`; any other unrecognized content fails closed.

## Binary API

- `on9kvdb_bytes` is an explicit `(data, size)` range for namespaces and keys.
  Each is 1–128 bytes, and embedded zero bytes are ordinary data.
- Values are opaque byte sequences up to `UINT32_MAX - 1` bytes. The caller
  supplies any string terminator it wants stored.
- `begin_value_write()`, `write_value()`, and `finish_value_write()` stream a
  value without a full-value allocation.
- `open_value()`, `peek_value()`, and `consume_value()` offer a fixed-buffer
  reader. `read_value_into()` is the direct small-value copy path.
- Namespace handles and transaction/value-reader/value-writer tokens are
  generation checked and cannot be used after close/abort/deinit.

All name comparison is length-aware `memcmp()` semantics. Inline values,
external-value chunks, WAL frames, manifest copies, SSTables, and identities
are CRC-protected before use.

## Raw layout and recovery

The exact provisioned range is partitioned in order as:

```text
two manifest copies | two WAL slots | fixed SSTable slots | two value banks
```

The manifest stores geometry, logical limits, active WAL/table/value-bank
selection, and reachability metadata. Recovery validates both manifest copies,
stabilizes a surviving newest copy before reuse, validates fixed-region
identities, then replays durable WAL transactions above the checkpoint.

All writes are 4096-byte aligned. Unreachable state is written and passed to
the backend `sync()` barrier before a later synchronized WAL/manifest update
makes it reachable. A backend must return transport failure from `sync()`;
the physical power-loss guarantee of SD cards, flash, and power systems is a
platform-qualification responsibility.

Revision 7 is intentionally incompatible with the previous FATFS image.

## Memory model

The component allocates its complete bounded working set in `init()` and frees
it in `deinit()`. Normal reads, writes, commits, WAL recovery, table flushing,
and compaction make no component-owned heap allocations. The I/O allocation
contains an encode frame, one raw-block bounce frame, one reader frame per
configured reader, and one writer frame; all are internal DMA-capable memory.

See [`on9kvdb.hpp`](on9kvdb.hpp) for the documented public API and
[`doc/`](doc/) for format, testing, and design details.
