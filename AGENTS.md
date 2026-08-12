# on9kvdb Agent Instructions

These instructions apply to the complete `components/on9kvdb` tree.

The detailed engineering contract is split by topic under [`doc/`](doc/).
Before changing implementation or persistent-format behavior, read
[`doc/AGENTS.md`](doc/AGENTS.md), [`doc/REQUIREMENTS.md`](doc/REQUIREMENTS.md),
and every additional topic document relevant to the change. Approved decisions
are authoritative; performance ideas in `doc/NEXT_STEPS.md` are proposals
unless their status explicitly says they were implemented or separately
approved.

## Non-negotiable rules

- Operate only on an application-owned exclusive `on9kvdb_io` block range.
  The component does not initialize/deinitialize transports, mount, unmount,
  format, partition, erase, migrate, resize, or silently repair media.
- Persistent geometry and revisions fail closed. Existing geometry must match
  the running configuration exactly.
- Preserve transaction durability: write and validate unreachable state,
  synchronize it through `on9kvdb_io::sync()`, publish it through a separately
  synchronized manifest, and return commit success only after the WAL barrier.
- Never reuse a table or WAL slot while either valid manifest copy can still
  reference it.
- Corruption returns an explicit typed error. It must never silently truncate
  acknowledged transactions, reset the database, or substitute defaults.
- Normal operation performs no component-owned heap allocation. Runtime
  memory, caches, handles, staging, indexes, and scratch space are bounded and
  allocated during initialization.
- Do not introduce runtime STL containers, strings, streams, smart pointers,
  recursion in bounded storage algorithms, lambda expressions, exceptions, or
  RTTI-dependent ownership.
- Preserve checked arithmetic and bounds before every persistent offset,
  length, read, write, seek, and native-width conversion.
- Long scans must periodically block or yield so watchdog and idle tasks can
  run.
- Match the component style: lower-case snake case, four-space indentation,
  130-column source width, `esp_err_t` propagation, and `*_unsafe` methods
  where the required lock is already held.
- Format changed C++ files with the component `.clang-format`, run the host
  tests relevant to the change, build the affected ESP-IDF target, and run
  `git diff --check`.

## Documentation map

- [`doc/REQUIREMENTS.md`](doc/REQUIREMENTS.md): scope, mandatory constraints,
  raw-block ownership, memory rules, and WAL/memtable architecture.
- [`doc/STORAGE_FORMAT.md`](doc/STORAGE_FORMAT.md): raw regions, manifest,
  WAL, SSTable, publication, and reuse ordering.
- [`doc/API_AND_RECOVERY.md`](doc/API_AND_RECOVERY.md): public behavior,
  logical model, provisioning, recovery, and concurrency.
- [`doc/DESIGN_DECISIONS.md`](doc/DESIGN_DECISIONS.md): approved capacity,
  workload, geometry, API, compaction, and integration decisions.
- [`doc/IMPLEMENTATION_AND_TESTING.md`](doc/IMPLEMENTATION_AND_TESTING.md):
  phases, verification requirements, invariants, acceptance criteria, and
  source ownership.
- [`doc/PERFORMANCE_COMPARISON.md`](doc/PERFORMANCE_COMPARISON.md): algorithmic
  complexity, measured ESP-IDF NVS comparison, and bounded-memory context.
- [`doc/NEXT_STEPS.md`](doc/NEXT_STEPS.md): performance measurements,
  completed optimizations, and unapproved follow-up proposals.
