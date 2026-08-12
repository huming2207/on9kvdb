# on9kvdb API, Logical Model, and Recovery

> **Superseded API note:** The v1 typed C-string public API described in older
> sections of this document has been replaced by the binary vNext API in
> [`BINARY_VALUE_API.md`](BINARY_VALUE_API.md). Use only byte slices and value
> readers/writers; there are no typed scalar/string/blob accessors or C-string
> termination rules.

The logical identity and ordering key is:

```text
(namespace bytes, key bytes)
```

Each memtable or SSTable source contains at most one version of that composite
key. When multiple published sources contain it, the greatest committed
transaction sequence wins. Persistent encoding is explicitly little-endian and
does not depend on C++ object layout, locale, pointer size, or structure
padding.

Lookup considers:

1. the calling transaction's staged overlay for transaction getter overloads;
2. the committed mutable memtable;
3. every manifest-selected immutable SSTable whose key range may match.

The greatest matching committed sequence decides the result. A tombstone means
not found.

## Approved public API surface

Keep the lifetime of the database instance explicit because storage is a FATFS
path rather than an NVS partition.

The v1 API is a C++ instance API with opaque generation-checked namespace and
transaction handles. It provides:

- explicit `init()` and `deinit()`;
- namespace `open()` and `close()`;
- one explicit atomic transaction at a time with `begin()`, `commit()`,
  transaction `close()`, and `abort()`;
- signed and unsigned 8-, 16-, 32-, and 64-bit setters/getters;
- string and blob setters/getters, including length queries;
- `erase_key()` and `find_key()`; and
- `get_stats()` for bounded counters, capacities, topology, and manifest
  recovery state.

Required NVS-familiar behavior:

- opening a namespace returns an opaque generation-checked handle;
- a read-only handle rejects mutations;
- setters are typed and type mismatch is reported;
- `get_str()` and `get_blob()` support a null output buffer length query;
- too-small output reports required length without overflow;
- `erase_key()`, `commit()`, transaction `close()`, `abort()`, and handle
  `close()` have documented uncommitted-change behavior;
- closing a handle does not silently convert an unsuccessful commit into
  success;
- handles have fixed capacity and stale handles cannot alias newly opened ones
  without a generation check.

One `commit()` is atomic across every staged mutation. General iterators,
`erase_all()`, namespace enumeration, secure purge, encryption, and a C wrapper
are deferred.

Do not reuse `ESP_ERR_NVS_*` values for a different engine and do not define
new component errors. V1 maps outcomes to existing values:

- invalid name, handle, or null argument: `ESP_ERR_INVALID_ARG`;
- type mismatch or structurally invalid decoded data:
  `ESP_ERR_INVALID_RESPONSE`;
- missing namespace or key: `ESP_ERR_NOT_FOUND`;
- read-only mutation: `ESP_ERR_NOT_ALLOWED`;
- value, transaction, geometry, or destination-buffer size mismatch:
  `ESP_ERR_INVALID_SIZE`;
- fixed RAM, memtable, namespace, or storage capacity exhausted:
  `ESP_ERR_NO_MEM`;
- checksum corruption: `ESP_ERR_INVALID_CRC`;
- unsupported stored format revision: `ESP_ERR_INVALID_VERSION`; and
- invalid lifecycle, busy handle, or sequence exhaustion:
  `ESP_ERR_INVALID_STATE`.

## Provisioning and recovery

### New-store provisioning

Follow the fail-closed ownership approach used by `on9ringstore`:

1. Validate all requested geometry, arithmetic, path lengths, FATFS free space,
   `off_t` limits, RAM budgets, and descriptor limits.
2. Scan the base directory and reject colliding canonical database filenames.
3. Create `manifest.db` contiguously at final size.
4. Write and sync redundant `provisioning_owned` manifest slots before
   creating data files.
5. Create each WAL and table file contiguously at final size, initialize its
   identity/slot metadata, sync it, and validate size and contiguity.
6. Resume safely if reset occurs after ownership is durable.
7. Publish a `ready` manifest only after all permanent files validate.

If data files exist but no valid ownership manifest exists, do not claim or
erase them automatically.

### Existing-store recovery

1. Validate all canonical file names, types, exact sizes, and contiguity.
2. Read both/all manifest slots and select a valid consistent generation.
3. Validate every referenced table header/footer, generation, bounds, ordering,
   and integrity metadata.
4. Recover referenced WAL generations in logical-generation order up to each
   last valid record.
5. Replay only complete committed transactions newer than the manifest's safe
   checkpoint sequence.
6. Rebuild bounded RAM indexes without allocating after the init allocation
   phase.
7. Detect capacity or geometry inconsistencies before publishing
   `initialized=true`.
8. Leave orphaned but valid unselected tables/WAL generations unselected; they
   may become reusable only after the authoritative state proves it is safe.

Recovery must not write repairs until the read-only analysis has identified one
unambiguous safe state. Separate analysis from repair so fault tests can reason
about both.

## Concurrency and visibility

The implemented v1 uses one lifecycle mutex and one serialized operation mutex.
There is one active writer transaction globally and no background compaction.
Lock ordering is lifecycle then operation; no application callback or unrelated
network I/O runs under either lock.

Public getters copy into caller-owned buffers. A transaction reads its own
staged overlay. Every other handle, including another handle for the same
namespace, sees only committed state. Committed visibility never precedes the
successful WAL durability barrier.
