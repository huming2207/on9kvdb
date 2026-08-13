# Storage format

## Revision and raw region layout

Storage revision 7 is an exclusive raw block image. Its configured geometry
exactly partitions the database-visible bytes as follows:

```text
offset 0
  manifest region: 2 x 4096-byte alternating manifest copies
  WAL regions:     2 fixed slots
  table regions:   CONFIG_ON9KVDB_SSTABLE_COUNT fixed slots
  value regions:   2 fixed append-only banks
offset CONFIG_ON9KVDB_PROVISIONED_DATABASE_SIZE
```

Every boundary and on-media write is aligned to 4096 bytes. `on9kvdb_io.cpp`
maps `(file_kind, slot, offset)` into this range with checked arithmetic; the
old names remain only as logical format terms, not POSIX files.

## Durable records

Manifest copies, region identities, WAL headers/frames, table headers/blocks/
indexes/footers, external-value chunks, and byte values include format
revision/magic/size checks plus CRC-32. A failed decode is corruption unless a
documented alternate manifest copy is valid.

Each data region begins with two independently encoded identity slots. The
identity binds its logical kind/slot/final size to the random database ID in
the manifest. Provisioning writes missing identities only while the manifest
is in the dedicated provisioning-owned state. Before first initialization
returns, both manifest slots are transitioned to the ready state; a later loss
of either copy can therefore never fall back into provisioning and overwrite
acknowledged WAL data.

## Publication order

1. Write new unreachable table/value/WAL header data in its fixed slot.
2. Call `on9kvdb_io::sync()` and check the result.
3. Write the later alternating manifest copy or WAL commit frame.
4. Call `sync()` again before returning durable success.

The prior manifest copy remains usable after an interrupted write. When only
one copy is valid, initialization writes a new alternating copy before allowing
any physical slot to be reused. Recovery first validates manifests and
identities, then scans the active WAL above the checkpoint.

`sync()` is an interface-level status barrier. It is intentionally separate
from claims about cache flushes, card firmware, capacitor hold-up, or an SD
card's behaviour during sudden power loss.
