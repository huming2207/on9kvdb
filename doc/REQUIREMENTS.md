# Requirements

## Scope

on9kvdb stores a binary key/value model in a caller-owned exclusive raw block
range. The component receives that range through `on9kvdb_io`; it does not
know paths, filesystems, partitions, mounting, formatting, or wear levelling.
The supplied SDMMC adapter is non-owning: application code initializes and
keeps the ESP-IDF host/card alive for the database lifetime.

The public model is byte-oriented. Namespace and key are 1–128 byte explicit
ranges, values are 0–`UINT32_MAX - 1` bytes, and `0x00` has no special
meaning. All comparison must be length-aware byte comparison, never a
C-string operation.

## Storage contract

- The exposed blocks belong exclusively to one on9kvdb instance. They cannot
  overlap a filesystem, partition metadata, or another consumer.
- A native block size must be non-zero, no greater than 4096 bytes, and divide
  4096 exactly. The visible count must hold the configured provisioned size.
- All persistent writes are complete, 4096-byte-aligned logical records.
  Reads of small fields may use the one fixed component bounce block.
- First provision accepts only a uniformly `0x00` or uniformly `0xff` exact
  configured range. Unknown/mixed content fails closed and is never erased.
- `sync()` must report transport/card failure. It is the required software
  barrier, not a claim that physical media survives arbitrary power removal.
- Geometry, limits, revisions, all checksums, and all offset/length arithmetic
  are validated before use. Revision 7 deliberately rejects prior FATFS
  images.

## Correctness and lifetime

- Write/validate unreachable data, synchronize it, then publish reachability
  through a separately synchronized WAL or manifest update.
- Never reuse WAL, table, or value-bank storage that either valid manifest copy
  may still reach. A one-copy recovery state is stabilized before reuse.
- Return explicit errors for corruption, media errors, incompatible formats,
  and invalid handles. Never reset or silently truncate acknowledged data.
- Normal operations perform no component-owned allocation. Allocate bounded
  runtime state only in `init()` and release it only in `deinit()`.
- No runtime STL, strings, streams, smart pointers, recursion in bounded
  algorithms, lambdas, exceptions, or RTTI-dependent ownership.
- Long scans yield periodically for watchdog/idle execution.
