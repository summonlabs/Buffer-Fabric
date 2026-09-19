# Changelog

All notable changes to Buffer Fabric are recorded here. The format follows
Keep a Changelog; the project uses semantic versioning.

## [1.0.0] - 2026

First release.

### Added

* **Core model.** Strong identities for pools, queues, tenants, traffic classes,
  resources, policies, allocations, backends, provenance, attempts, snapshots
  and requests; `Generation`, `EpochId` and `Sequence`; durable
  `BootIncarnation`.
* **Exact accounting.** Per-pool identity
  `raw + borrowed_in + overcommit_used == protected + allocated + free + lent_out`,
  with derived `usable`, `own_usage`, `overcommit_used` and `free`, plus
  `free-or-overcommit` and `reserved+committed == pinned+reclaimable`
  invariants and the global borrow balance. Deep verification recomputes every
  aggregate from the allocation set and reports divergence.
* **Pressure governance.** `CLEAR`, `ELEVATED`, `HIGH`, `CRITICAL`,
  `UNKNOWN` and `STALE`; basis-point thresholds compared with exact widened
  integer multiplication; evidence validity gated on epoch, pool generation,
  freshness and (by default) agreement with authoritative accounting.
* **Generation binding.** Every allocation binds pool, queue, policy, capacity
  and backend generations plus the fabric epoch and boot incarnation; an advance
  invalidates dependents and returns their units to free capacity while keeping
  the record for lineage and audit.
* **Decisions.** `GRANT`, `GRANT_PARTIAL`, `REFUSE`, `REDUCE`, `FENCE`,
  `REVALIDATE`, `RECLAIM`, `NO_OP` and `IDEMPOTENT_REPLAY`, each with a single
  binding constraint, a bounded sealed authority vector and a 128-bit digest over
  a canonical encoding.
* **Reclamation.** Deterministic victim ordering, protected headroom never
  reclaimable, pinned allocations revocable only under explicit policy, hold
  time, per-operation bound and reserve.
* **Backends.** Explicit capacity authority through `IBackend`; `NullBackend`
  and `SyntheticBackend`; opt-in physical programming hooks that are never
  assumed to have succeeded.
* **Durability.** Versioned snapshot container and append-only CRC-32C journal
  with atomic replace, stable-storage flush before acknowledgement, bounded
  growth through threshold compaction, torn-tail repair, mid-file corruption
  refusal, unfinished-reservation reporting and mandatory pressure revalidation
  after restart.
* **Multiprocess.** `Coordinator` and worker over a real framed TCP transport with
  fabric-epoch fencing of every post-handshake request, bounded client capacity
  and resilient handling of malformed traffic.
* **Tools.** `bfctl` operator CLI, `bfbench` synthetic benchmark, `bf_coordinator` and
  `bf_worker`.
* **Packaging.** Installable and exported CMake package plus an independent
  `find_package` consumer under `examples/downstream`.
* **Tests.** 117 tests: unit, integration, seeded randomized property,
  adversarial, concurrency, persistence and restart, and real multiprocess,
  registered with CTest without any timeout property.

### Hardening findings fixed before release

* The snapshot image kind byte was written twice, shifting every snapshot record
  by one byte and making every snapshot unreadable.
* After repairing a torn journal tail, the store restored the pre-repair file
  length and the next append re-extended the file with zeros, silently
  corrupting the record that followed the gap.
* The clock was read while the authoritative mutex was held, so a caller-supplied
  `IClock` that re-entered the fabric would deadlock. The clock is now sampled
  before the lock is taken.
* Backend accessors were invoked while the authoritative mutex was held; they are
  now read before the lock.
* Borrowing drew from a lender before consuming the borrower's own free capacity,
  overstating `borrowed_in` and skipping the borrower's own capacity.
* `allow_partial_grant = false` was not honoured; a partial grant was returned
  where the policy required a refusal.
* A pool with only retired queues could never be removed; retired queues are now
  removed with their pool.
* The JSON writer appended separators outside its byte budget, so an object with
  many members could grow unbounded.
* The coordinator re-derived a full transport frame from an already unframed
  payload, so every handshake failed with a frame-magic mismatch.
* Coordinator bootstrap was not idempotent, so a restart against an existing
  durable image failed to start.
* `bfctl` treated the value of a leading option as its command name.
* The concurrency suite could stall on a Microsoft C runtime assertion dialog;
  the test harness now routes CRT reports to standard error and fails fast.

[1.0.0]: https://github.com/summonlabs/Buffer-Fabric/releases/tag/v1.0.0
