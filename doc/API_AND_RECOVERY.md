# API and recovery

## Lifecycle

Construct `on9kvdb` with a non-null `on9kvdb_io*`, after the application has
initialized the physical transport. `init()` verifies block geometry, allocates
all fixed component memory, provisions an erased range or opens the existing
revision-7 image, and recovers the WAL. Call `deinit()` before invalidating the
transport; non-forced deinitialization refuses open namespace/reader/writer/
transaction resources.

## Binary access

Use `on9kvdb_bytes` for namespaces and keys. It carries both pointer and
length, so embedded zero bytes are valid. `open()` produces a namespace handle,
`begin()` starts its one active transaction, and `commit()` makes staged changes
durable. Closing a transaction commits it; abort discards only unreachable
staged state.

For small values, use `set()` and `read_value_into()`. For a large value use
`begin_value_write()`, zero or more `write_value()` calls, and
`finish_value_write()` before committing the transaction. To consume values
without a full allocation use `open_value()`, then repeated `peek_value()` and
`consume_value()` calls. Reader/writer tokens are generation checked.

## Recovery behaviour

Startup chooses the highest valid manifest generation. Two valid copies must
agree on database identity, geometry, and adjacent-publication constraints. A
single valid copy is stabilized before slot reuse. The component validates the
fixed identities, validates table metadata, establishes the active value bank,
and replays complete CRC-valid WAL commits after the checkpoint. Corruption or
an incompatible revision is reported, never automatically erased or repaired.
