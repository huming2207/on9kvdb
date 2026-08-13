# Implementation and testing

## Source ownership

| Source | Responsibility |
| --- | --- |
| `on9kvdb_io.hpp` | Public raw block-device contract |
| `on9kvdb_io_sdmmc.*` | Non-owning ESP-IDF SDMMC implementation |
| `on9kvdb_io_fatfs.*` | One-contiguous-file FATFS implementation |
| `on9kvdb_io.cpp` | Checked logical-region mapping and fixed bounce reads |
| `on9kvdb_manifest.cpp` | Raw provisioning, identities, manifest copies/recovery gate |
| `on9kvdb_wal.cpp` | WAL creation, append, scan, and replay |
| `on9kvdb_table.cpp`, `on9kvdb_compaction.cpp` | Fixed SSTable regions and copy-on-write compaction |
| `on9kvdb_value.cpp` | Fixed-buffer reader/writer and value-bank chunks |

## Required checks

Run after changing storage, format, or public API:

```sh
cmake -S components/on9kvdb/tests -B /tmp/on9kvdb_vnext_tests
cmake --build /tmp/on9kvdb_vnext_tests -j4
ctest --test-dir /tmp/on9kvdb_vnext_tests --output-on-failure

. /home/hu/esp/esp-idf/export.sh
idf.py build
git diff --check
```

The host tests verify encoders/decoders, API-facing binary invariants, and a
durable-image fault matrix. The matrix fails every individual write and sync
call in external-value commit, table-flush publication, and full compaction,
then simulates reboot from the last successful sync and verifies acknowledged
data. The ESP-IDF build verifies the full raw-SDMMC component path. Hardware
qualification must additionally test the selected SDMMC wiring, a known
exclusive LBA range, card removal/error propagation, reboot recovery, and real
power interruptions at every write/publication boundary.

When changing the FATFS adapter, verify an empty mounted volume creates one
contiguous file, then that a second boot reopens it without creating or touching
any other file. Test rejection of a non-contiguous, wrong-sized, or externally
modified target file.

## Review checklist

- Confirm every raw offset is checked and remains within the configured range.
- Confirm every write is native-block/4096-byte aligned and every published
  object was synchronized first.
- Confirm all allocation remains within initialization/deinitialization.
- Confirm both manifest copies are considered before table/WAL/value-bank reuse.
- Confirm an unrecognized nonblank range fails without modifying the device.
