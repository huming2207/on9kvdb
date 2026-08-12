# Binary Key and Value API Design

This document records the breaking revision that replaces the typed C-string
API. There is no compatibility path for earlier storage revisions.

## Public model

- Namespace and key are opaque byte slices from one through 128 bytes. They
  may contain zero bytes; callers retain ownership of the input memory.
- Values are opaque byte streams from zero through `UINT32_MAX - 1` bytes.
  The configured database capacity remains the practical upper bound.
- `set()` accepts one contiguous byte range as a convenience. For progressive
  writes, call `begin_value_write(transaction, key, total_size)`, call
  `write_value(writer, bytes, size)` as often as needed, then call
  `finish_value_write(writer)` or `abort_value_write(writer)`. A writer always
  has a known final length, accepts zero-length and inline values as well as
  larger streams, and never retains the whole large value in RAM.
- A reader handle is an opaque, fixed-capacity alternative to `FILE *` with
  ring-buffer semantics. `open_value()` resolves a namespace/key to one
  committed version. `peek_value()` exposes the next contiguous byte span
  already in that reader's preallocated buffer, `consume_value()` advances the
  cursor by a caller-selected amount, `seek_value()` changes its byte offset,
  and `close()` releases the pin. The span remains valid only until the next
  reader operation on that handle. It never exposes a C stdio object, a
  filesystem path, or a copied-to-heap value buffer.

  `read_value_into()` may copy directly into a caller-owned buffer, but it has
  the same streaming semantics and performs no allocation. This is the normal
  small-value path (for example, a four-byte integer encoded by the caller).

Writer handles are sequential and exact-length in this revision. Random
in-place writes would require a second copy-on-write extent map, inflate
metadata, and make the small ESP32 target substantially less predictable.

## Storage layout

Small values remain inline in WAL and SSTable records. This preserves the
one-read small-value path and avoids consuming a full chunk for common scalar
and configuration values.

Larger values are stored in a pair of fixed, append-only value banks. Every
external value is a sequence of fixed-size chunks. A chunk has a header with
database identity, bank generation, value offset, payload length, payload
CRC-32, and record CRC-32. Its descriptor stores the first chunk, total byte
length, and whole-value CRC-32.

The active bank accepts new value streams. Normal compaction copies only live
external values into the inactive bank while rebuilding the SSTables, validates
each copied chunk, then publishes the new bank in the separately synchronized
manifest. Only after both manifest copies select the new bank may the old bank
be reused. This gives one compact value storage area per bank rather than one
file or allocation per value.

An aborted write or a reset before the WAL commit can leave unreachable chunks
at the active bank tail. They are never visible and are reclaimed by the next
bank-switching compaction. No automatic erase is required.

## Memory and reader pins

Reader and writer handles occupy fixed slots allocated during `init()`. Each
reader slot owns a bounded DMA-capable chunk buffer, so a `peek()` span cannot
be invalidated by another reader. A reader of an external value pins its bank.
Compaction refuses to recycle that bank until the reader closes, rather than
copying data into a heap buffer or allowing a stale handle to read rewritten
storage. The reader-count and per-reader buffer size are explicit configuration
costs; normal database operations make no component-owned allocation after
`init()`.

Every durable map record and every external-value chunk carries its own CRC-32.
Block, WAL-frame, and manifest checksums remain as independent higher-level
checks.
