# Buffer Fabric

**Buffer Fabric 1.0.0** — an open-source, vendor-neutral C++20 runtime for
generation-bound governance of shared packet-buffer capacity.

Buffer Fabric answers one question, deterministically and explainably:

> Given authoritative packet-buffer capacity, pool topology, queue demands,
> protected headroom, thresholds, policy and current generations, **how much
> buffer may each claimant use, what pressure state exists, what remains
> reclaimable or protected, and when must allocations be reduced, fenced,
> revalidated or refused?**

It governs capacity. It does not carry packets.

---

## System boundary

**Owned by this runtime**

* shared buffer capacity and its authoritative partition into pools;
* pool authority and pool topology;
* allocation state: reservations, commitments, releases, fences;
* soft and hard thresholds and their exact band semantics;
* protected headroom;
* pressure state derived from evidence;
* reclamation intent, victim selection and revocation;
* the accounting identity and its exact closure;
* durable committed state: pools, policy, allocations, lineage, fences, epoch.

**Explicitly not owned**

* queue lifecycle, queue creation or destruction;
* packet scheduling, shaping or pacing;
* rate enforcement;
* congestion synthesis or congestion control;
* microburst detection;
* backpressure propagation;
* path or placement decisions;
* physical device memory programming, except through an explicitly attached
  backend.

Queue demands, queue generations and pressure observations are **ingested as
authoritative inputs from their owners**. Buffer Fabric binds allocations to the
exact generations it was given and reports the consequences of a generation
change. It never creates, schedules or paces a queue.

---

## The accounting identity

Every pool maintains one exact integer identity, checked after every accepted
mutation and re-derivable from the allocation set alone:

    raw + borrowed_in + overcommit_used
        == protected + allocated + free + lent_out

| Term | Meaning |
| --- | --- |
| raw | authoritative raw capacity of this pool's share of a resource |
| protected | protected headroom; never allocatable by general claimants |
| allocated | reserved + committed units held by this pool's allocations |
| free | unallocated, unreserved units under this pool's authority |
| borrowed_in | portion of allocated drawn from ancestors' free capacity |
| lent_out | units of this pool's free capacity committed to descendants |
| overcommit_used | excess of own-capacity usage over usable capacity |

Derived quantities used everywhere else:

    usable          = raw - protected
    own_usage       = allocated - borrowed_in + lent_out
    overcommit_used = max(0, own_usage - usable)
    free            = usable + overcommit_used - own_usage

Two invariants follow and are checked separately:

    free == 0  or  overcommit_used == 0        (never both)
    allocated == reserved + committed == pinned + reclaimable

Globally, across all pools:

    sum(borrowed_in) == sum(lent_out)
    sum(raw) + sum(overcommit_used) == sum(protected) + sum(allocated) + sum(free)

`BufferFabric::validate_accounting()` performs a **deep** verification: it
recomputes every pool aggregate from the allocation records independently of the
incrementally maintained totals and reports any divergence. The test suite calls
it after randomized operation sequences, after every race test, and after every
restart.

---

## Pressure states

`CLEAR`, `ELEVATED`, `HIGH`, `CRITICAL`, `UNKNOWN`, `STALE`.

Thresholds are basis points of usable capacity and are compared with exact
integer multiplication (`used * 10000` against `band * usable`, widened without
overflow), so there is no rounding and no overflow even at the maximum supported
capacity.

| State | Meaning |
| --- | --- |
| CLEAR | fresh, epoch-matched, generation-matched evidence below the elevated band |
| ELEVATED | at or above the elevated band |
| HIGH | at or above the high band |
| CRITICAL | at or above the critical band, or overcommitted |
| UNKNOWN | no usable evidence exists |
| STALE | evidence exists but its epoch, pool generation, age or reported usage makes it unusable |

**Pressure state does not perform congestion control.** It is a governance input
and a reported fact. Buffer Fabric publishes corrective *intent*
(`reduce_to`, `fence`, `revalidate`, `reclaim_target`); acting on that intent is
outside this runtime.

### Evidence discipline

An observation is usable only when **all** of the following hold:

1. its fabric epoch equals the current epoch;
2. its pool generation equals the current pool generation;
3. its age is within its declared lifetime;
4. unless the producer explicitly sets `assert_committed = false`, the
   committed usage it reports equals the fabric's authoritative own usage.

`UNKNOWN` never becomes positive authority. With the default policy
(`pressure.refuse_increase_on_unknown = true`) an allocation *increase* is
refused with `DecisionKind::Revalidate` until fresh evidence exists. A deployment
that genuinely cannot supply evidence sets that flag to `false`, which is
recorded in every decision it affects.

---

## Generation binding and authority

Every entity is addressed by a distinct strong type: `PoolId`, `QueueId`,
`TenantId`, `ClassId`, `ResourceId`, `PolicyId`, `AllocationId`, `BackendId`,
`ProvenanceId`, `AttemptId`, `SnapshotId`, `RequestId`, plus `Generation`,
`EpochId` and `Sequence`. Raw integers are not accepted where an identity is
expected.

Each allocation binds the exact `BoundGenerations` that granted it: pool
generation, queue generation, policy generation, capacity revision, backend
generation, fabric epoch and boot incarnation. An advance in a bound generation
invalidates the dependent allocation: its units are returned to free capacity
and the record is retained as `FENCED` for lineage and audit.

Every decision carries a bounded, sealed `AuthorityVector` naming the exact
inputs it used, plus a 128-bit digest over the canonical encoding of those
inputs. `BufferFabric::explain()` returns a byte-identical explanation — including
the digest — for identical state.

---

## Decisions

| Kind | Meaning |
| --- | --- |
| GRANT | the full request was granted |
| GRANT_PARTIAL | a positive amount smaller than the request was granted |
| REFUSE | nothing was granted |
| REDUCE | committed usage must be reduced to the reported target |
| FENCE | a bound generation advanced; dependent allocations were invalidated |
| REVALIDATE | authority could not be established; fresh evidence is required |
| RECLAIM | reclamation intent was produced or applied |
| NO_OP | the requested outcome already held |
| IDEMPOTENT_REPLAY | the attempt was already applied and its outcome was replayed |

Each refusal reports exactly one binding constraint (capacity,
protected_headroom, overcommit_limit, borrow_limit, queue_max_committed,
queue_share, pressure_refuse, evidence_unknown, evidence_stale,
generation_mismatch, and so on), so an explanation is never ambiguous.

---

## Reclamation

Victim selection is a total order — `(reclaim_priority asc, last_touched asc,
allocation id asc)` — so an identical state always produces a byte-identical plan
and digest.

* Pool **protected headroom is never a reclaim candidate** under any policy.
* **Pinned** allocations are never revoked unless the pool policy explicitly
  sets `reclaim.allow_protected_reclaim`.
* `reclaim.min_hold_ticks` excludes recently touched allocations.
* `reclaim.max_units_per_operation` bounds a single operation.
* `reclaim.reserve_units` extends the plan beyond the requested target so that
  the reserve is satisfied.

---

## Backends

Capacity is only ever taken from an explicit backend. `IBackend` reports
`query_capacity(resource)` and the runtime refuses to found or resize a pool on a
resource with no authoritative capacity. When no backend is attached, capacity is
`UNKNOWN`, never zero.

`NullBackend` reports `Unsupported` for everything. `SyntheticBackend` is a
deterministic in-process source used by the tests and the benchmark; it is
labelled SYNTHETIC everywhere it appears.

Physical device programming is reachable only through
`IBackend::program_reservation` and `program_release`, and only when the embedder
sets `FabricConfig::program_backend_effects = true`. A backend that owns no
programmable memory answers `Unsupported`, which is recorded and tolerated; any
other failure refuses the publication. **No device backend ships with this
release.**

---

## Building

Requires CMake 3.20 or newer and a C++20 compiler.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Options:

| Option | Default | Effect |
| --- | --- | --- |
| BUFFER_FABRIC_BUILD_TOOLS | ON at top level | build bfctl, bfbench, bf_coordinator, bf_worker |
| BUFFER_FABRIC_BUILD_TESTS | ON at top level | build the test suites |
| BUFFER_FABRIC_INSTALL | ON | generate install and export rules |
| BUFFER_FABRIC_STRICT | ON | warnings as errors |
| BUFFER_FABRIC_SANITIZE | empty | address enables the platform sanitizer where available |
| BUFFER_FABRIC_ANALYZE | OFF | run the platform static analyzer on first-party sources |

Debug, RelWithDebInfo and Release all build clean under MSVC `/W4 /WX
/permissive-` and are configured for `-Wall -Wextra -Wpedantic -Werror` on GCC
and Clang.

### Install and consume

    cmake --install build --prefix /some/prefix
    cmake -S examples/downstream -B build-downstream -DCMAKE_PREFIX_PATH=/some/prefix

`examples/downstream` is a completely independent CMake project that consumes only
`find_package(BufferFabric 1.0 REQUIRED CONFIG)` and
`BufferFabric::buffer_fabric`.

---

## Command line tools

    bfctl --state-dir DIR init --capacity N --pool-units N --protected N --queue-id N
    bfctl --state-dir DIR observe --pool 1
    bfctl --state-dir DIR allocate --pool 1 --queue-id 1 --units 4096 --attempt 1
    bfctl --state-dir DIR allocate --observe-first --pool 1 --queue-id 1 --units 4096 --attempt 1
    bfctl --state-dir DIR explain --pool 1
    bfctl --state-dir DIR ledger
    bfctl --state-dir DIR verify
    bfctl capabilities

Every `bfctl` invocation opens the fabric from the state directory, performs one
command and closes it. Because the fabric advances its epoch on every open, each
invocation is a genuine restart: pressure evidence recorded by a previous
invocation is reported as STALE, and an unqualified `allocate` is therefore
refused with `REVALIDATE`. That is the intended behaviour, not a defect — a
one-shot process cannot carry live authority across a process boundary.

`--observe-first` publishes evidence from the fabric's own authoritative state
inside the same invocation, which is exactly what an in-process embedder does,
and is the supported way to exercise allocation from the command line. The
coordinator, which holds one fabric open for its whole lifetime, is the intended
path for sustained multi-process work.

`bfbench` runs the synthetic benchmark. `bf_coordinator` and `bf_worker` are the real
multiprocess pair described below.

---

## Multiprocess coordinator and worker

`bf_coordinator` hosts an authoritative fabric, publishes a minimal explicit
topology and serves framed requests over a real TCP socket on an explicitly
chosen address. It prints exactly one line, `READY <port>`, once it is listening.

`bf_worker` connects, performs a framed handshake and runs allocate/release
cycles, reporting exactly what happened.

**Epoch fencing.** Every request after the handshake must present the current
fabric epoch. A request that presents an older epoch is refused with
`StaleEpoch` and the connection is closed; it can never reach authoritative
state. The handshake itself accepts a claimed epoch of zero ("I hold none") and
refuses any non-zero claim that is not current.

**Framing.** `u32 magic | u32 wire version | u32 payload size | u32 CRC-32C of
payload | payload`. A frame is rejected on bad magic, unsupported version,
oversized payload, truncation or checksum mismatch. Malformed traffic closes the
offending connection and leaves the coordinator serving everyone else.

---

## Durability

Durable state is a versioned snapshot plus an append-only journal of CRC-32C
protected, length-prefixed frames.

**Transactional order for every durable mutation:** validate, bind authority,
plan, verify the planned accounting identity, journal, apply, verify the live
accounting identity, acknowledge. A durable mutation is never acknowledged before
its journal record is on stable storage.

**Bounded growth.** When the journal reaches its compaction threshold the current
state is snapshotted *before* the new record is appended, so the snapshot always
describes a consistent prefix of the mutation sequence.

**Torn tail.** A partial record at the end of the journal ends replay. If no
intact record follows the damage it is treated as an interrupted write, the file
is truncated to the last intact record and the event is reported. If intact
records *do* follow, that is corruption rather than a torn write, and recovery
refuses.

**Recovery distinguishes:**

| Category | Treatment |
| --- | --- |
| durable configuration and history | restored |
| committed authoritative state | restored as committed |
| reservations (leases on capacity) | not restored; reported as unfinished attempts, units returned to free capacity |
| pressure evidence | restored but invalidated: every snapshot is STALE until re-observed under the new epoch |
| live authority, leases, worker authority | never restored |
| physical device effect | never restored or assumed |

The fabric epoch advances on every open and a fresh boot incarnation is minted.
A corrupt snapshot, an unsupported format version, a truncated container or a
short read all refuse recovery with a specific error code.

---

## Concurrency and lock discipline

All public methods are safe to call concurrently.

* One non-recursive `std::mutex` guards all authoritative state. There is no
  shared/read lock, therefore no read-to-write upgrade anywhere.
* The **clock is sampled before the mutex is taken** by the RAII `Lock` type, so
  a user-supplied `IClock` is never invoked while the fabric lock is held.
* Backends are queried before the mutex is taken.
* Events are collected into a local vector and **published after the mutex is
  released**. An event sink may therefore re-enter the fabric from the emitting
  thread; `concurrency.event_callbacks_may_reenter_the_fabric` exercises exactly that.
* Coordinator session threads take no lock the fabric needs; `stop()` closes the
  listener, shuts down client sockets, then joins — never while holding a lock a
  worker needs.
* Shutdown stops accepting new work and refuses new allocations, but **allows
  release, commit and revalidation to complete**, so accounting returns to a
  valid baseline.

Duplicate mutating calls are idempotent by `AttemptId`: replaying the same
attempt with the same canonical request digest returns the recorded outcome
without touching state; reusing an attempt id with a different digest is refused
as `AttemptConflict`. Attempt identities are fabric-wide.

---

## Bounded resources

Every externally influenced size, count, capacity, rate and time unit is bounded
and checked: `kMaxPoolCount`, `kMaxQueueCount`, `kMaxAllocationCount`,
`kMaxPolicyCount`, `kMaxPoolDepth`, `kMaxUnitsPerPool`, `kMaxOvercommitUnits`,
`kMaxAttemptMemory`, `kMaxAuthorityVectorEntries`, `kMaxNameBytes`,
`kMaxStatusMessageBytes`, `kMaxJournalRecordBytes`, `kMaxSnapshotBytes`,
`kMaxFrameBytes`, `kMaxReclaimVictims` and `kMaxLineageDepth`.

The decision history, the attempt memory, the pressure history and the
explanation writer are all bounded ring buffers. Integer arithmetic on externally
influenced values uses checked helpers that report overflow instead of wrapping.

---

## Build and validation configuration used for this release

* Windows 11, MSVC 19.44 (Visual Studio 2022 17.14), CMake 4.3, Ninja 1.13.
* Debug and Release both build warning-clean with `/W4 /WX /permissive-`.
* `BUFFER_FABRIC_ANALYZE=ON` (MSVC `/analyze`) reports **zero first-party
  findings**. The only output is `C6101` inside Windows SDK headers (Mtu,
  Enabled) reached through the socket includes; no first-party function is
  implicated.
* AddressSanitizer was **not available** in this environment: the MSVC x64
  AddressSanitizer runtime (clang_rt.asan_dynamic_runtime_thunk-x86_64.lib) is
  not installed, and no clang-cl is present. The sanitizer configuration is fully
  wired (`-DBUFFER_FABRIC_SANITIZE=address`) and compiles the library, but **no
  sanitizer run is claimed** for this release.
* Tests are registered with CTest **without any timeout property**. A hang is a
  defect to diagnose, not something to terminate.

---

## Test suite

**117 tests: 112 single-process and 5 multiprocess, all passing in Debug and
Release.**

| Suite | Coverage |
| --- | --- |
| types, codec, json, policy, accounting, reclaim | strong identities, checked arithmetic, exact ratio comparison, CRC-32C vectors, frame accept/reject, bounded JSON, threshold validation and boundary classification, closure identity and every fault detector |
| fabric | lifecycle, authoritative-capacity gating, resource partition closure, protected headroom, partial grants, idempotency and conflicts, stale epoch/generation refusal, reservations and expiry, partial release, bounded overcommit, borrowing, queue demand and hard caps, queue retirement and retargeting, pool shrink and floors, policy fencing, pool removal, depth bounds, deterministic explanations, shutdown |
| pressure | unknown evidence never authorises an increase, staleness by age/epoch/generation, future-dated rejection, accounting cross-check, exact band transitions, refusal band, soft limit |
| reclaim | plan determinism and digest equality, pinned protection, explicit protected reclaim, deterministic revocation, reserve, min-hold, disabled policy |
| persistence | journal replay, snapshot round trip, epoch advance, unfinished reservations, pressure revalidation after restart, atomic checkpoint, torn tail repair, mid-file corruption refusal, corrupt/truncated/version-mismatched snapshot refusal |
| property | 25 seeded randomized operation sequences with deep closure verification, attempt replay, baseline restoration, all pressure bands reachable |
| concurrency | parallel allocate/release closure, exactly-once duplicated attempt, re-entrant event sink, clock re-entrancy under the lock, concurrent evaluation never mutates, shutdown, concurrent topology mutation |
| adversarial | zero/oversized/invalid inputs, structural pool validation, unknown backend and resource, backend readiness, null paths, config bounds, malformed and oversized frames, unsupported wire version, malformed pressure, duplicate registrations, bounded history and attempt memory, backend programming opt-in, capability honesty |
| multiprocess | real coordinator/worker allocation over TCP, malformed traffic resilience, hard process kill and restart with epoch advance and stale-worker fencing, concurrent workers, clean shutdown |

**Multiprocess proof.** `bf_mp_tests` launches real `bf_coordinator` and `bf_worker`
operating-system processes over a real framed TCP transport. The restart test
force-terminates the coordinator with no graceful shutdown, relaunches it against
the same state directory, and asserts that a worker still claiming the previous
epoch is refused while a worker accepting the new epoch is served and the new
epoch is strictly greater.

---

## Benchmark

`bfbench` is **SYNTHETIC**. It measures completed work in one process against a
synthetic in-process topology and a synthetic backend. Nothing it reports
observes or programs physical hardware, and no figure represents a physical
network, NIC, switch or device measurement. Enqueue or submission latency is
never reported; an operation is counted only once the fabric has accepted it and
the accounting identity has been verified.

Representative result: Windows 11, MSVC 19.44, Release, 8 pools, 16 queues per
pool, seed 24301, 300 000 iterations. The figures below are one run on an idle
machine; repeat runs on the same machine varied by up to roughly a factor of two
in the mutation-heavy scenarios, so these are indicative of order of magnitude,
not a calibrated measurement.

| Scenario | Completed operations | Seconds | Operations/s |
| --- | --- | --- | --- |
| allocate + release | 600 000 | 1.11 | 541 846 |
| fill then reclaim | 61 440 | 1.94 | 31 611 |
| fragmentation | 6 144 | 0.058 | 106 448 |
| evaluate (read-only) | 300 000 | 0.146 | 2 055 194 |
| pressure transitions | 60 | 0.0004 | 137 836 |

Accounting closure held in every scenario, in every run.

`fill_then_reclaim` is dominated by filling each pool to its limit and then
revoking every commitment; its throughput is bounded by the number of
allocations that fit, not by the rate of requests.

---

## Proof surface

**REAL** — verified by execution in this environment

* single-process runtime behaviour: allocation, release, reservations, commit,
  fencing, reclamation, explanation, exact accounting closure;
* real multiprocess execution: separate coordinator and worker operating-system
  processes over a real TCP socket with real framed transport, real hard process
  kill, real restart, real epoch advancement and real stale-epoch rejection;
* real durable persistence: files written, flushed, atomically replaced, replayed
  and recovered; real torn-tail repair and real corruption refusal;
* real install/export and a real independent downstream `find_package` consumer
  built and executed against the installed prefix;
* real fresh-clone build from committed sources;
* real Release and Debug compilation under `/W4 /WX`;
* real MSVC `/analyze` static analysis of first-party sources with zero
  first-party findings.

**SYNTHETIC** — clearly labelled, never presented as physical

* `SyntheticBackend` capacity authority;
* every benchmark figure;
* the topology used by the multiprocess tests, which the coordinator declares as
  an explicit operator input rather than discovering from hardware.

**UNSUPPORTED** — not implemented and not claimed

* any physical device, NIC, DPU, switch, RDMA, NVLink or optical validation;
* distributed consensus or multi-coordinator agreement;
* AddressSanitizer execution in this environment (the MSVC x64 ASan runtime is
  not installed and no clang-cl is present);
* GCC and Clang builds (the CMake configuration supports them; only MSVC was
  available here);
* 32-bit builds (the accounting layer targets 64-bit `usize` and `u64` and does
  not build warning-clean as 32-bit under `/WX`);
* congestion control, pacing, shaping, scheduling and packet handling of any kind.

---

## Known limitations

* A single fabric instance is authoritative. There is no leader election and no
  multi-coordinator agreement; a second coordinator on the same state directory
  would be a configuration error, not a supported topology.
* Borrowing resolves to the nearest ancestor with free capacity and is capped by
  that single lender; an allocation has at most one lender.
* Reclamation is a governed revocation of capacity commitments, not a transport
  of backpressure. It reports intent; the claimant acts.
* Attempt identity is fabric-wide. Concurrent claimants must use disjoint attempt
  ranges.
* Pressure snapshot freshness is evaluated with the fabric's clock. A deployment
  whose observers run on unsynchronised clocks must set a generous TTL rather
  than rely on tight time bounds.
* Pool shrink invalidates allocations bound to the previous capacity revision.
  Callers are expected to re-allocate against the new revision.

---

## Repository layout

    include/buffer_fabric/   public headers
    src/                     implementation
    tests/                   test suites and the shared framework
    tools/                   bfctl, bfbench, bf_coordinator, bf_worker
    examples/downstream/     independent find_package consumer
    cmake/                   package configuration template
    LICENSE                  Apache License 2.0
    CHANGELOG.md             release history

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
