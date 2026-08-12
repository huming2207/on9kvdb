# Design decisions

## Approved vNext storage design

- Storage revision 7 uses an exclusive `on9kvdb_io` block range. There is no
  compatibility layer for prior file-backed images.
- The component owns database layout but not the transport. SDMMC host/card
  setup belongs to application code; `on9kvdb_io_sdmmc` is an adapter over an
  initialized `sdmmc_card_t` and a configured first LBA.
- The raw range has fixed manifest, WAL, SSTable, and value-bank regions.
  This removes allocation, directory, file-descriptor, path, and filesystem
  metadata work from reads and writes.
- The database validates an entire blank range before first provisioning. It
  never automatically erases or reformats a nonblank unknown range.
- 4096 bytes remains the logical durability/I/O unit. A device may use a
  smaller block size that divides 4096; raw reads and writes use whole native
  blocks.
- `sync()` is kept in the abstraction because write completion and physical
  persistence are device-specific. The SDMMC implementation uses a post-write
  card status command and makes no stronger power-loss claim.
- SD controller management and NOR flash wear levelling are outside on9kvdb.
- `on9kvdb_io_fatfs` ships with the main component. It creates one immediately
  allocated contiguous file, validates that file before every open, and then
  uses only its descriptor. This retains the database's fixed 4096-byte block
  contract while deliberately avoiding directory and file allocation work on
  its normal I/O path.

## Memory and read efficiency

- Normal operations allocate no component-owned memory. Init allocates a
  fixed internal-DMA I/O set and bounded runtime arena; deinit releases them.
- Direct aligned storage transfers avoid a copy. The only I/O bounce block is
  for reading small unaligned metadata fields.
- Reader slots have permanent 4096-byte buffers and expose them through
  `peek_value()`/`consume_value()`. `read_value_into()` remains for scalar and
  small structured values.
- Values are progressively written in external 4096-byte chunks, preserving
  bounded RAM even when the public value length is near `UINT32_MAX - 1`.

## Safety decisions

- All namespaces/keys are explicit byte ranges; no null terminator is added,
  required, or interpreted.
- Every persisted logical record is CRC-protected. Manifest-copy fallback and
  stabilization prevent reusing storage reachable from an older valid copy.
- A card/transport failure latches the database storage fault where reachability
  is uncertain, requiring recovery through a new initialization.
