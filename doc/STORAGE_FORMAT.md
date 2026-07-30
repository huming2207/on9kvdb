# on9kvdb Storage Format and Durability

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
manifest publication fields documented below and in `../README.md`.

## Manifest

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

## WAL slots

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

## SSTable slots

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
