# Follow-up measurements

The raw-SDMMC migration is a performance experiment, not a completed hardware
qualification. Before changing the storage protocol, capture:

1. Single/multi-block SDMMC read and write latency at the chosen bus width and
   clock rate.
2. `sync()` card-status cost and failure behaviour for the selected cards.
3. Database commit latency versus a representative record/value distribution.
4. Reader throughput for inline and external values.
5. Reboot recovery duration after an orderly shutdown and after injected
   failures at each manifest/WAL/value publication boundary.

Possible future work requires separate approval: torn native-block fault
simulation, card-specific cache-flush support if ESP-IDF exposes a safe
portable contract, larger sequential table reads, and a raw NOR adapter over a
wear-levelled partition. The host suite now injects complete-call write and
sync failures across external commits, table publication, and compaction.
