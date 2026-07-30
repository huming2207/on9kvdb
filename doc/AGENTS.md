# on9kvdb Audit and Agent Guide

This directory is the canonical topic-split engineering record for
`on9kvdb`. The short `../AGENTS.md` file is intentionally retained as an
automatic-discovery router for coding agents; it does not replace these
detailed documents.

The former 1,033-line `AGENTS.md` was split on 2026-07-30 to make independent
audits practical. Its normative content was preserved and grouped by concern.
No requirement became optional merely because it moved to another file.

## Authority

- Statements marked **required** or **approved** are authoritative.
- A proposed or open decision requires user approval before implementation.
- `NEXT_STEPS.md` is a performance roadmap. Only items explicitly recorded as
  implemented or approved may be treated as current design.
- When documents overlap, the more specific approved rule applies. An actual
  contradiction is a blocker: report it instead of guessing.

## Audit map

| Document | Audit focus | Read when changing |
| --- | --- | --- |
| [`REQUIREMENTS.md`](REQUIREMENTS.md) | Scope, hard constraints, FATFS/journal assumptions, allocation and coding rules, WAL/memtable architecture | Any production code or behavior |
| [`STORAGE_FORMAT.md`](STORAGE_FORMAT.md) | File layout, persistent metadata, synchronization, manifest publication, physical-slot reuse | Encoding, I/O, recovery, flush, compaction, geometry |
| [`API_AND_RECOVERY.md`](API_AND_RECOVERY.md) | Logical-key model, public API and errors, provisioning, recovery, locks and visibility | Public APIs, lifecycle, handles, replay, concurrency |
| [`DESIGN_DECISIONS.md`](DESIGN_DECISIONS.md) | Approved capacities, workload, memory, transaction, table-bank, corruption and integration policies | Kconfig, budgets, format policy, feature scope |
| [`IMPLEMENTATION_AND_TESTING.md`](IMPLEMENTATION_AND_TESTING.md) | Phase record, fault model, invariants, acceptance criteria and source ownership | Tests, qualification, release claims, refactors |
| [`NEXT_STEPS.md`](NEXT_STEPS.md) | Measured baseline, completed performance work and future experiments | Performance work and benchmark interpretation |

## Suggested audit order

1. Read `REQUIREMENTS.md` for the non-negotiable contract.
2. Audit `STORAGE_FORMAT.md` together with the encoders/decoders and I/O
   implementation.
3. Audit `API_AND_RECOVERY.md` together with public dispatch, WAL replay, and
   lifecycle code.
4. Compare implementation choices with `DESIGN_DECISIONS.md`.
5. Use `IMPLEMENTATION_AND_TESTING.md` as the fault-injection and release
   checklist.
6. Treat `NEXT_STEPS.md` separately so performance proposals are not confused
   with approved durability behavior.

## Review discipline

- Trace every persistent field through encode, checksum, decode, validation,
  publication, and recovery.
- For every write, identify whether the object is reachable and which later
  synchronized manifest makes it authoritative.
- For every corruption branch, prove that later acknowledged transactions
  cannot be silently discarded.
- For every arena carve and persistent arithmetic expression, verify alignment,
  overflow handling, and the exact failure code.
- For every optimization, compare ordinary latency, tail latency, recovery
  cost, memory cost, and write amplification; do not rely on averages alone.

