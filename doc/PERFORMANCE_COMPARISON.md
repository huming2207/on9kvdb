# Performance notes

The current design removes filesystem/VFS path traversal, directory metadata,
file descriptor management, allocation, and filesystem synchronization from the
database data path. It does not remove the database's required WAL/manifest
publication barriers or change the amount of 4096-byte durable database work.

Measure the target card rather than assuming a latency figure. Record separate
timings for aligned SDMMC reads, aligned writes, `sync()`, WAL commits, small
`read_value_into()` reads, and streamed large-value reads/writes. Include
median and tail latency, not only averages.

The fixed physical layout should improve ordinary reads because a table/value
address maps directly to known LBA-relative blocks. It also removes heap churn
from component operations; ESP-IDF's configured reusable DMA bounce buffer may
still be used for application/PSRAM transfer alignment.
