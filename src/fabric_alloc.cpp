#include "fabric_impl.hpp"

#include <algorithm>
#include <utility>

#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/version.hpp"

namespace buffer_fabric {
namespace {

[[nodiscard]] u64 sat_add(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (add_overflow(a, b, &out)) return (std::numeric_limits<u64>::max)();
    return out;
}

[[nodiscard]] u64 sat_mul(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (mul_overflow(a, b, &out)) return (std::numeric_limits<u64>::max)();
    return out;
}

[[nodiscard]] JournalOp journal_op_for(AllocationState state) noexcept {
    switch (state) {
        case AllocationState::Reserved: return JournalOp::AllocationReserved;
        case AllocationState::Committed: return JournalOp::AllocationCommitted;
        case AllocationState::Fenced: return JournalOp::AllocationFenced;
        case AllocationState::Reclaimed: return JournalOp::AllocationReclaimed;
        case AllocationState::Released: return JournalOp::AllocationReleased;
        case AllocationState::Expired: return JournalOp::AllocationExpired;
    }
    return JournalOp::AllocationReleased;
}

[[nodiscard]] EventKind event_for(AllocationState state) noexcept {
    switch (state) {
        case AllocationState::Reserved: return EventKind::AllocationReserved;
        case AllocationState::Committed: return EventKind::AllocationCommitted;
        case AllocationState::Fenced: return EventKind::AllocationFenced;
        case AllocationState::Reclaimed: return EventKind::AllocationReclaimed;
        case AllocationState::Released: return EventKind::AllocationReleased;
        case AllocationState::Expired: return EventKind::AllocationExpired;
    }
    return EventKind::AllocationReleased;
}

[[nodiscard]] bool is_stale_binding(const AllocationRecord& record, const PoolEntry& entry,
                                    const PoolPolicy& policy, EpochId epoch) noexcept {
    return record.bound.pool_generation != entry.descriptor.generation ||
           record.bound.capacity_generation != entry.capacity_generation ||
           record.bound.policy_generation != policy.generation ||
           record.bound.epoch != epoch;
}

}  // namespace

// ---------------------------------------------------------------------------
// Evidence resolution
// ---------------------------------------------------------------------------

PressureEvidence BufferFabric::Impl::resolve_evidence(const PoolEntry& entry) const {
    PressureEvidence evidence;
    evidence.pool_generation = entry.descriptor.generation;
    evidence.epoch = epoch_;

    const auto policy_it = policies_.find(entry.descriptor.policy.raw());
    const Thresholds thresholds = policy_it != policies_.end() ? policy_it->second.pressure.thresholds
                                                               : Thresholds{};
    const PressureObservation observation = classify_pressure(
        thresholds, entry.own_usage, entry.usable(), entry.overcommit_used() != 0);
    evidence.utilization_bp = observation.utilization_bp;
    evidence.binding_bp = observation.binding_bp;
    evidence.overcommitted = observation.overcommitted;

    const auto it = pressure_.find(entry.descriptor.id.raw());
    if (it == pressure_.end() || !it->second.has_latest) {
        evidence.state = PressureState::Unknown;
        evidence.reason = ErrorCode::EvidenceUnknown;
        evidence.detail = "no pressure snapshot has been observed for this pool";
        return evidence;
    }
    const PressureSnapshot& snapshot = it->second.latest;
    const Tick stamp = now();
    evidence.snapshot = snapshot.id;
    evidence.observed_at = snapshot.observed_at;
    evidence.age_ticks = stamp >= snapshot.observed_at ? stamp - snapshot.observed_at : 0;
    evidence.epoch_match = snapshot.epoch == epoch_;
    evidence.generation_match = snapshot.pool_generation == entry.descriptor.generation;
    evidence.fresh = is_fresh(stamp, snapshot.observed_at, snapshot.ttl_ticks);

    if (!evidence.epoch_match) {
        evidence.state = PressureState::Stale;
        evidence.reason = ErrorCode::StaleEpoch;
        evidence.detail = "pressure snapshot was produced under a different fabric epoch";
        return evidence;
    }
    if (!evidence.generation_match) {
        evidence.state = PressureState::Stale;
        evidence.reason = ErrorCode::StaleGeneration;
        evidence.detail = "pressure snapshot describes a different pool generation";
        return evidence;
    }
    if (!evidence.fresh) {
        evidence.state = PressureState::Stale;
        evidence.reason = ErrorCode::StaleEvidence;
        evidence.detail = "pressure snapshot is older than its declared lifetime";
        return evidence;
    }
    // A snapshot whose reported committed usage disagrees with authoritative
    // accounting describes a different state and is therefore not evidence for
    // this decision. A reported value of zero means "not asserted".
    if (snapshot.assert_committed && snapshot.committed_units != entry.own_usage) {
        evidence.state = PressureState::Stale;
        evidence.reason = ErrorCode::StaleEvidence;
        evidence.detail = "pressure snapshot disagrees with authoritative accounting";
        return evidence;
    }
    evidence.state = observation.state;
    evidence.reason = ErrorCode::Ok;
    evidence.detail = "fresh, epoch-matched, generation-matched evidence";
    return evidence;
}

// ---------------------------------------------------------------------------
// Capacity arithmetic
// ---------------------------------------------------------------------------

BufferFabric::Impl::GrantComputation BufferFabric::Impl::compute_grant(const PoolEntry& entry, const PoolPolicy& policy,
                                           u64 requested) const {
    BF_UNUSED(requested);
    GrantComputation computation;
    computation.own_free = entry.free_units();

    u64 overcommit_headroom = 0;
    if (policy.overcommit.mode == OvercommitMode::Bounded &&
        policy.overcommit.limit_units > entry.overcommit_used()) {
        overcommit_headroom = policy.overcommit.limit_units - entry.overcommit_used();
    }
    computation.own_overcommit_headroom = overcommit_headroom;
    const u64 own_allowance = sat_add(computation.own_free, overcommit_headroom);

    if (policy.borrow.mode == BorrowMode::FromAncestorFree) {
        computation.borrow_limit_remaining =
            policy.borrow.limit_units > entry.borrowed_in
                ? policy.borrow.limit_units - entry.borrowed_in
                : 0;
        PoolId cursor = entry.descriptor.parent;
        u64 depth = 0;
        while (cursor.valid() && depth < kMaxPoolDepth) {
            const PoolEntry* ancestor = pool(cursor.raw());
            if (ancestor == nullptr) break;
            if (!ancestor->descriptor.retired && ancestor->free_units() != 0) {
                computation.lender = ancestor->descriptor.id;
                computation.lender_free = ancestor->free_units();
                break;
            }
            cursor = ancestor->descriptor.parent;
            ++depth;
        }
        const u64 lender_available = computation.lender_free;
        const u64 allowed_borrow = std::min(lender_available, computation.borrow_limit_remaining);
        computation.borrowable = allowed_borrow;
    }

    computation.max_grant = sat_add(own_allowance, computation.borrowable);
    if (computation.max_grant == 0) {
        computation.binding = policy.overcommit.mode == OvercommitMode::Bounded
                                  ? BindingConstraint::OvercommitLimit
                                  : BindingConstraint::Capacity;
        computation.binding_limit = 0;
    } else if (own_allowance == 0) {
        computation.binding = BindingConstraint::Capacity;
        computation.binding_limit = 0;
    } else {
        computation.binding = BindingConstraint::Capacity;
        computation.binding_limit = own_allowance;
    }
    return computation;
}

Status BufferFabric::Impl::apply_commit_units(PoolEntry& entry, u64 units, u64 borrow_units, PoolId lender) {
    if (borrow_units > units) {
        return Status(ErrorCode::InvariantViolation, "borrowed units exceed allocated units");
    }
    const u64 own = units - borrow_units;
    u64 next_own = 0;
    if (add_overflow(entry.own_usage, own, &next_own)) {
        return Status(ErrorCode::Overflow, "pool own usage overflowed");
    }
    u64 next_borrowed = 0;
    if (add_overflow(entry.borrowed_in, borrow_units, &next_borrowed)) {
        return Status(ErrorCode::Overflow, "pool borrowed units overflowed");
    }
    entry.own_usage = next_own;
    entry.borrowed_in = next_borrowed;

    if (borrow_units != 0) {
        if (!lender.valid()) {
            return Status(ErrorCode::InvariantViolation, "borrowed units without a lender");
        }
        PoolEntry* lender_entry = pool(lender.raw());
        if (lender_entry == nullptr) {
            return Status(ErrorCode::UnknownPool, "lender pool is not registered");
        }
        u64 next_lent = 0;
        if (add_overflow(lender_entry->lent_out, borrow_units, &next_lent)) {
            return Status(ErrorCode::Overflow, "lender lent units overflowed");
        }
        u64 next_lender_own = 0;
        if (add_overflow(lender_entry->own_usage, borrow_units, &next_lender_own)) {
            return Status(ErrorCode::Overflow, "lender own usage overflowed");
        }
        if (next_lender_own > sat_add(lender_entry->usable(), kMaxOvercommitUnits)) {
            return Status(ErrorCode::CapacityExceeded, "lender has no capacity to lend");
        }
        lender_entry->lent_out = next_lent;
        lender_entry->own_usage = next_lender_own;
    }
    return Status::success();
}

Status BufferFabric::Impl::apply_release_units(PoolEntry& entry, const AllocationRecord& record) {
    if (!allocation_holds_units(record.state)) {
        return Status(ErrorCode::InvariantViolation, "allocation does not hold units");
    }
    const u64 units = record.units;
    const u64 borrowed = record.borrow_units <= units ? record.borrow_units : units;
    const u64 own = units - borrowed;

    if (entry.own_usage < own) {
        return Status(ErrorCode::InvariantViolation, "pool own usage underflow");
    }
    if (entry.borrowed_in < borrowed) {
        return Status(ErrorCode::InvariantViolation, "pool borrowed units underflow");
    }
    if (record.state == AllocationState::Reserved) {
        if (entry.reserved_units < units) {
            return Status(ErrorCode::InvariantViolation, "pool reserved units underflow");
        }
        entry.reserved_units -= units;
    } else {
        if (entry.committed_units < units) {
            return Status(ErrorCode::InvariantViolation, "pool committed units underflow");
        }
        entry.committed_units -= units;
    }
    if (record.reclaim_class == ReclaimClass::Pinned) {
        if (entry.pinned_units < units) {
            return Status(ErrorCode::InvariantViolation, "pool pinned units underflow");
        }
        entry.pinned_units -= units;
    } else {
        if (entry.reclaimable_units < units) {
            return Status(ErrorCode::InvariantViolation, "pool reclaimable units underflow");
        }
        entry.reclaimable_units -= units;
    }
    entry.own_usage -= own;
    entry.borrowed_in -= borrowed;

    if (borrowed != 0) {
        PoolEntry* lender = record.lender.valid() ? pool(record.lender.raw()) : nullptr;
        if (lender == nullptr) {
            return Status(ErrorCode::InvariantViolation, "lender pool is missing");
        }
        if (lender->lent_out < borrowed || lender->own_usage < borrowed) {
            return Status(ErrorCode::InvariantViolation, "lender accounting underflow");
        }
        lender->lent_out -= borrowed;
        lender->own_usage -= borrowed;
    }
    adjust_queue(entry, record.queue, -static_cast<i64>(units),
                 record.state == AllocationState::Reserved);
    entry.members.erase(record.id.raw());
    return Status::success();
}

Status BufferFabric::Impl::fence_allocation_locked(AllocationRecord& record, AllocationState new_state,
                                     ErrorCode reason, std::vector<FabricEvent>& events) {
    PoolEntry* entry = pool(record.pool.raw());
    if (entry == nullptr) {
        return Status(ErrorCode::UnknownPool, "allocation references an unknown pool");
    }
    if (!allocation_holds_units(record.state)) {
        return Status(ErrorCode::NoChange, "allocation no longer holds units");
    }
    BF_TRY(apply_release_units(*entry, record));
    record.state = new_state;
    record.last_touched = now();
    record.revision += 1;
    BF_TRY(journal_allocation(journal_op_for(new_state), record));
    push_event(events, event_for(new_state), ErrorCode::Ok, std::string(to_string(reason)),
               record.pool, record.queue, record.id, record.units);
    switch (new_state) {
        case AllocationState::Fenced: metrics_.allocations_fenced += 1; entry->runtime.fenced_count += 1; break;
        case AllocationState::Reclaimed: metrics_.allocations_reclaimed += 1; entry->runtime.reclaim_count += 1; break;
        case AllocationState::Released: metrics_.allocations_released += 1; break;
        case AllocationState::Expired: metrics_.allocations_expired += 1; break;
        default: break;
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Decision assembly
// ---------------------------------------------------------------------------

namespace {

void bind_authority(AuthorityVector* authority, const PoolEntry& entry,
                    const QueueDescriptor& queue, const PoolPolicy& policy, AttemptId attempt,
                    ProvenanceId provenance, SnapshotId snapshot, EpochId epoch,
                    BootIncarnation boot, PoolId lender) {
    // The capacity element carries the pool's capacity revision, which is the
    // revision a decision about this pool's capacity actually depends on.
    (void)authority->bind(AuthorityKind::Capacity, entry.capacity_generation.raw(),
                          entry.capacity_generation);
    (void)authority->bind_generation(AuthorityKind::Pool, entry.descriptor.generation);
    (void)authority->bind_generation(AuthorityKind::Queue, queue.generation);
    (void)authority->bind_generation(AuthorityKind::Policy, policy.generation);
    (void)authority->bind_id(AuthorityKind::Pressure, snapshot.raw());
    (void)authority->bind_id(AuthorityKind::Epoch, epoch.raw());
    (void)authority->bind_id(AuthorityKind::Backend, entry.descriptor.backend.raw());
    (void)authority->bind_id(AuthorityKind::Provenance, provenance.raw());
    (void)authority->bind_id(AuthorityKind::Attempt, attempt.raw());
    (void)authority->bind(AuthorityKind::BootIncarnation, boot.hi, Generation::from_raw(boot.lo));
    if (lender.valid()) {
        (void)authority->bind_id(AuthorityKind::Reservation, lender.raw());
    }
    authority->seal();
}

[[nodiscard]] Decision make_decision(DecisionKind kind, ErrorCode code, std::string reason,
                                     const PoolEntry& entry, const QueueDescriptor& queue,
                                     u64 requested, u64 granted, AllocationId allocation,
                                     BindingConstraint binding, const PressureEvidence& evidence,
                                     const PoolAccounting& accounting, Tick stamp, u64 sequence) {
    Decision decision;
    decision.kind = kind;
    decision.status = code;
    decision.reason = std::move(reason);
    decision.pool = entry.descriptor.id;
    decision.queue = queue.id;
    decision.requested_units = requested;
    decision.granted_units = granted;
    decision.allocation = allocation;
    decision.binding = binding;
    decision.pressure = evidence.state;
    decision.utilization_bp = evidence.utilization_bp;
    decision.accounting = accounting;
    decision.decided_at = stamp;
    decision.sequence = sequence;
    return decision;
}

}  // namespace

// ---------------------------------------------------------------------------
// Allocate
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::Impl::allocate_locked(const AllocateRequest& request,
                                       std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (shutting_down_ && config_.refuse_work_after_shutdown) {
        return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
    }
    if (!request.attempt.valid()) {
        return Status(ErrorCode::InvalidArgument, "attempt id must be non-zero");
    }
    if (request.requested_units == 0) {
        return Status(ErrorCode::InvalidArgument, "requested units must be non-zero");
    }
    if (request.requested_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "requested units exceed the supported bound");
    }
    BF_TRY(ensure_alloc_capacity());

    const Digest digest = request.request_digest();
    AttemptRecord previous;
    bool replay = false;
    BF_TRY(lookup_attempt(request.attempt, digest, &previous, &replay));

    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(request.pool, &entry));
    QueueDescriptor* queue = nullptr;
    BF_TRY(require_queue(request.queue, &queue));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    const Tick stamp = now();

    if (replay) {
        metrics_.idempotent_replays += 1;
        const PressureEvidence evidence = resolve_evidence(*entry);
        Decision decision = make_decision(DecisionKind::IdempotentReplay, previous.outcome,
                                          "attempt already applied", *entry, *queue,
                                          request.requested_units, previous.granted_units,
                                          previous.allocation, BindingConstraint::None, evidence,
                                          accounting_of(*entry), stamp, next_decision_sequence_);
        decision.authority.bind_id(AuthorityKind::Attempt, request.attempt.raw());
        decision.authority.seal();
        return decision;
    }

    if (request.expected_epoch.valid() && request.expected_epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch, "request epoch does not match the fabric epoch");
    }
    if (request.expected_pool_generation.valid() &&
        request.expected_pool_generation != entry->descriptor.generation) {
        return Status(ErrorCode::StaleGeneration, "request pool generation is not current");
    }
    if (request.expected_policy_generation.valid() &&
        request.expected_policy_generation != policy.generation) {
        return Status(ErrorCode::StaleGeneration, "request policy generation is not current");
    }
    if (request.expected_capacity_generation.valid() &&
        request.expected_capacity_generation != entry->capacity_generation) {
        return Status(ErrorCode::StaleGeneration, "request capacity generation is not current");
    }
    if (request.expected_queue_generation.valid() &&
        request.expected_queue_generation != queue->generation) {
        return Status(ErrorCode::StaleGeneration, "request queue generation is not current");
    }
    if (queue->pool != entry->descriptor.id) {
        return Status(ErrorCode::InvalidArgument, "queue is not bound to the requested pool");
    }
    const PressureEvidence evidence = resolve_evidence(*entry);

    auto finish = [&](DecisionKind kind, ErrorCode code, std::string reason, u64 granted,
                      AllocationId allocation, BindingConstraint binding,
                      const CorrectiveIntent& intent, PoolId lender) -> Result<Decision> {
        Decision decision = make_decision(kind, code, std::move(reason), *entry, *queue,
                                          request.requested_units, granted, allocation, binding,
                                          evidence, accounting_of(*entry), stamp,
                                          next_decision_sequence_);
        decision.intent = intent;
        bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                       request.provenance, evidence.snapshot, epoch_, boot_, lender);
        if (code != ErrorCode::Ok) {
            metrics_.refusals += 1;
            entry->runtime.refuse_count += 1;
        }
        record_decision(decision);
        ++next_decision_sequence_;
        AttemptRecord attempt_record;
        attempt_record.id = request.attempt;
        attempt_record.kind = AttemptKind::Allocate;
        attempt_record.request_digest = digest;
        attempt_record.outcome = code;
        attempt_record.allocation = allocation;
        attempt_record.granted_units = granted;
        attempt_record.sequence = decision.sequence;
        attempt_record.recorded_at = stamp;
        record_attempt(attempt_record);
        entry->runtime.last_decision_at = stamp;
        metrics_.decisions += 1;
        if (granted == 0 && code != ErrorCode::Ok) {
            push_event(events, EventKind::DecisionRefused, code, decision.reason, request.pool,
                       request.queue, allocation, request.requested_units, decision.sequence);
        }
        return decision;
    };

    // 1. A pool that has been retired accepts no new commitments.
    if (entry->descriptor.retired) {
        return finish(DecisionKind::Refuse, ErrorCode::WrongState, "pool is retired", 0,
                      AllocationId{}, BindingConstraint::PoolRetired, CorrectiveIntent{},
                      PoolId{});
    }
    if (queue->retired) {
        return finish(DecisionKind::Refuse, ErrorCode::WrongState, "queue is retired", 0,
                      AllocationId{}, BindingConstraint::QueueRetired, CorrectiveIntent{},
                      PoolId{});
    }

    // 2. Missing or stale evidence is never positive authority for an increase.
    if (!evidence.usable()) {
        if (policy.pressure.refuse_increase_on_unknown) {
            CorrectiveIntent intent;
            intent.revalidate_required = true;
            const BindingConstraint binding = evidence.state == PressureState::Unknown
                                                  ? BindingConstraint::EvidenceUnknown
                                                  : BindingConstraint::EvidenceStale;
            return finish(DecisionKind::Revalidate, evidence.reason,
                          "fresh pressure evidence is required before capacity can be granted", 0,
                          AllocationId{}, binding, intent, PoolId{});
        }
    }

    // 3. Pressure that has reached the refusal band refuses outright.
    if (pressure_is_known(evidence.state) && policy.refuse_grants_at != PressureState::Unknown &&
        pressure_at_least(evidence.state, policy.refuse_grants_at)) {
        CorrectiveIntent intent;
        intent.reduce_required = true;
        intent.reduce_to_units = entry->usable();
        return finish(DecisionKind::Reduce, ErrorCode::PolicyForbids,
                      "pressure is at or above the refusal band", 0, AllocationId{},
                      BindingConstraint::PressureRefuse, intent, PoolId{});
    }

    // 4. Capacity arithmetic.
    GrantComputation computation = compute_grant(*entry, policy, request.requested_units);
    u64 limit = computation.max_grant;
    BindingConstraint binding = computation.binding;
    u64 binding_limit = computation.binding_limit;

    auto apply_cap = [&](u64 cap, BindingConstraint constraint) {
        if (cap < limit) {
            limit = cap;
            binding = constraint;
            binding_limit = cap;
        }
    };

    if (policy.max_single_request_units != 0) {
        apply_cap(policy.max_single_request_units, BindingConstraint::PolicyMaxSingleRequest);
    }
    if (policy.max_single_allocation_units != 0) {
        apply_cap(policy.max_single_allocation_units, BindingConstraint::PolicyMaxSingleAllocation);
    }
    if (queue->max_committed_units != 0) {
        const auto committed_it = entry->queue_committed.find(queue->id.raw());
        const auto reserved_it = entry->queue_reserved.find(queue->id.raw());
        const u64 held = (committed_it == entry->queue_committed.end() ? 0 : committed_it->second) +
                         (reserved_it == entry->queue_reserved.end() ? 0 : reserved_it->second);
        const u64 room = queue->max_committed_units > held ? queue->max_committed_units - held : 0;
        apply_cap(room, BindingConstraint::QueueMaxCommitted);
    } else if (queue->demand_units != 0) {
        // Authoritative demand is an upper bound on the queue's committed units
        // when no explicit hard cap is configured.
        const auto committed_it = entry->queue_committed.find(queue->id.raw());
        const auto reserved_it = entry->queue_reserved.find(queue->id.raw());
        const u64 held = (committed_it == entry->queue_committed.end() ? 0 : committed_it->second) +
                         (reserved_it == entry->queue_reserved.end() ? 0 : reserved_it->second);
        const u64 room = queue->demand_units > held ? queue->demand_units - held : 0;
        apply_cap(room, BindingConstraint::QueueShare);
    }
    if (policy.soft_limit_bp != 0 && entry->usable() != 0) {
        const u64 soft_units = sat_mul(entry->usable(), policy.soft_limit_bp) / Thresholds::kScale;
        const u64 allowance = soft_units > entry->own_usage ? soft_units - entry->own_usage : 0;
        if (pressure_at_least(evidence.state, PressureState::High)) {
            apply_cap(allowance, BindingConstraint::PolicySoftLimit);
        }
    }

    u64 grant = std::min(request.requested_units, limit);
    if (!policy.allow_partial_grant && grant < request.requested_units) {
        // A policy that forbids partial grants refuses outright rather than
        // handing back a smaller commitment the claimant did not ask for.
        if (binding == BindingConstraint::None) binding = BindingConstraint::Capacity;
        grant = 0;
    }
    if (grant == 0) {
        CorrectiveIntent intent;
        DecisionKind kind = DecisionKind::Refuse;
        ErrorCode code = ErrorCode::CapacityExceeded;
        std::string reason = "no capacity is available under the governing policy";
        if (pressure_is_known(evidence.state) &&
            pressure_at_least(evidence.state, policy.reduce_at)) {
            kind = DecisionKind::Reduce;
            code = ErrorCode::PolicyForbids;
            reason = "pressure requires committed usage to be reduced";
            intent.reduce_required = true;
            intent.reduce_to_units = entry->usable();
        }
        return finish(kind, code, std::move(reason), 0, AllocationId{}, binding, intent,
                      PoolId{});
    }

    // 5. Build the allocation and apply it in the transactional order:
    //    plan -> journal -> apply -> verify.
    // Own free capacity is consumed first, then a lender's free capacity, and
    // only then the borrower's explicit overcommit allowance. Borrowing is
    // therefore never used to paper over capacity the borrower already holds.
    const u64 borrow =
        grant > computation.own_free
            ? std::min(grant - computation.own_free, computation.borrowable)
            : 0;
    AllocationRecord record;
    // Allocation identity is derived from the fabric's own id space so that a
    // caller cannot choose or collide with it.
    record.id = AllocationId::from_raw(next_allocation_id_);
    u64 next_alloc = 0;
    if (add_overflow(next_allocation_id_, 1, &next_alloc)) {
        return Status(ErrorCode::Overflow, "allocation id space is exhausted");
    }
    record.pool = entry->descriptor.id;
    record.queue = queue->id;
    record.tenant = queue->tenant;
    record.traffic_class = queue->traffic_class;
    record.units = grant;
    record.state = request.commit_immediately ? AllocationState::Committed
                                              : AllocationState::Reserved;
    record.reclaim_class = request.reclaim_class;
    record.reclaim_priority = request.reclaim_priority;
    record.bound.pool_generation = entry->descriptor.generation;
    record.bound.queue_generation = queue->generation;
    record.bound.policy_generation = policy.generation;
    record.bound.capacity_generation = entry->capacity_generation;
    const auto backend_it = backends_.find(entry->descriptor.backend.raw());
    if (backend_it != backends_.end()) {
        record.bound.backend_generation = backend_it->second.generation;
    }
    record.bound.epoch = epoch_;
    record.bound.boot = boot_;
    record.attempt = request.attempt;
    record.provenance = request.provenance;
    record.borrow_units = borrow;
    record.lender = borrow != 0 ? computation.lender : PoolId{};
    record.created_at = stamp;
    record.last_touched = stamp;
    if (request.ttl_ticks != 0) {
        u64 expiry = 0;
        if (add_overflow(stamp, request.ttl_ticks, &expiry)) {
            return Status(ErrorCode::Overflow, "allocation expiry overflowed");
        }
        record.expires_at = expiry;
    }
    if (request.parent_allocation.valid()) {
        const auto parent_it = allocations_.find(request.parent_allocation.raw());
        if (parent_it == allocations_.end()) {
            return Status(ErrorCode::UnknownAllocation, "parent allocation does not exist");
        }
        if (parent_it->second.lineage.depth + 1 > kMaxLineageDepth) {
            return Status(ErrorCode::BoundedResourceExhausted, "allocation lineage is too deep");
        }
        record.lineage.origin = parent_it->second.lineage.origin.valid()
                                    ? parent_it->second.lineage.origin
                                    : parent_it->second.id;
        record.lineage.parent = parent_it->second.id;
        record.lineage.depth = parent_it->second.lineage.depth + 1;
        ByteWriter chain(96);
        chain.put_id(record.lineage.origin);
        chain.put_id(record.lineage.parent);
        chain.put_u32(record.lineage.depth);
        chain.put_u64(grant);
        chain.put_id(record.pool);
        chain.put_id(record.queue);
        chain.put_digest(parent_it->second.lineage.chain_digest);
        record.lineage.chain_digest = digest_bytes(chain.data().data(), chain.size());
    } else {
        record.lineage.origin = record.id;
        record.lineage.depth = 0;
        ByteWriter chain(64);
        chain.put_id(record.id);
        chain.put_u64(grant);
        chain.put_id(record.pool);
        chain.put_id(record.queue);
        record.lineage.chain_digest = digest_bytes(chain.data().data(), chain.size());
    }
    record.revision = 1;

    // Plan verification: the intended state must close before anything is
    // journaled.
    {
        PoolEntry scratch = *entry;
        scratch.own_usage += grant - borrow;
        scratch.borrowed_in += borrow;
        if (request.commit_immediately) {
            scratch.committed_units += grant;
        } else {
            scratch.reserved_units += grant;
        }
        if (record.reclaim_class == ReclaimClass::Pinned) {
            scratch.pinned_units += grant;
        } else {
            scratch.reclaimable_units += grant;
        }
        scratch.members.insert(record.id.raw());
        PoolAccounting planned = accounting_of(scratch);
        const AccountingReport report =
            check_pool_accounting(planned, policy.overcommit.mode, policy.overcommit.limit_units);
        if (!report.ok()) {
            return Status(ErrorCode::InvariantViolation,
                          "planned allocation would break the accounting identity");
        }
    }

    BF_TRY(journal_allocation(journal_op_for(record.state), record));
    BF_TRY(apply_commit_units(*entry, grant, borrow, record.lender));
    if (request.commit_immediately) {
        entry->committed_units += grant;
    } else {
        entry->reserved_units += grant;
    }
    if (record.reclaim_class == ReclaimClass::Pinned) {
        entry->pinned_units += grant;
    } else {
        entry->reclaimable_units += grant;
    }
    adjust_queue(*entry, record.queue, static_cast<i64>(grant), !request.commit_immediately);
    entry->members.insert(record.id.raw());
    next_allocation_id_ = next_alloc;
    allocations_[record.id.raw()] = record;
    allocations_total_ += 1;
    metrics_.allocations_created += 1;
    if (request.commit_immediately) {
        metrics_.allocations_committed += 1;
    }
    metrics_.live_allocations = allocations_.size();
    metrics_.live_committed_units += grant;
    if (metrics_.live_committed_units > metrics_.peak_committed_units) {
        metrics_.peak_committed_units = metrics_.live_committed_units;
    }
    entry->runtime.grant_count += 1;
    entry->runtime.peak_committed_units =
        std::max(entry->runtime.peak_committed_units, entry->committed_units);

    const Status closure = check_closure(*entry);
    if (!closure.ok()) {
        shutting_down_ = true;
        push_event(events, EventKind::AccountingViolation, closure.code(), closure.message(),
                   entry->descriptor.id);
        return closure;
    }

    const DecisionKind kind =
        grant == request.requested_units ? DecisionKind::Grant : DecisionKind::GrantPartial;
    Result<Decision> decision = finish(kind, ErrorCode::Ok, "capacity granted", grant, record.id,
                                       binding, CorrectiveIntent{}, record.lender);
    if (decision.ok()) {
        if (kind == DecisionKind::Grant) {
            metrics_.grants += 1;
        } else {
            metrics_.partial_grants += 1;
        }
        if (policy.reclaim.enabled && entry->free_units() == 0) {
            decision.value().intent.reclaim_target_units =
                policy.reclaim.reserve_units > entry->free_units()
                    ? policy.reclaim.reserve_units - entry->free_units()
                    : 0;
        }
    }
    return decision;
}

// ---------------------------------------------------------------------------
// Commit
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::Impl::commit_locked(const CommitRequest& request,
                                     std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (!request.attempt.valid()) {
        return Status(ErrorCode::InvalidArgument, "attempt id must be non-zero");
    }
    const Digest digest = request.request_digest();
    AttemptRecord previous;
    bool replay = false;
    BF_TRY(lookup_attempt(request.attempt, digest, &previous, &replay));

    const auto record_it = allocations_.find(request.allocation.raw());
    if (record_it == allocations_.end()) {
        return Status(ErrorCode::UnknownAllocation, "no such allocation");
    }
    AllocationRecord& record = record_it->second;
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(record.pool, &entry));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    QueueDescriptor* queue = nullptr;
    BF_TRY(require_queue(record.queue, &queue));
    const PressureEvidence evidence = resolve_evidence(*entry);
    const Tick stamp = now();

    if (replay) {
        metrics_.idempotent_replays += 1;
        Decision decision = make_decision(DecisionKind::IdempotentReplay, previous.outcome,
                                          "attempt already applied", *entry, *queue, 0,
                                          previous.granted_units, record.id,
                                          BindingConstraint::None, evidence, accounting_of(*entry),
                                          stamp, next_decision_sequence_);
        decision.authority.bind_id(AuthorityKind::Attempt, request.attempt.raw());
        decision.authority.seal();
        return decision;
    }
    if (request.expected_epoch.valid() && request.expected_epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch, "request epoch does not match the fabric epoch");
    }
    if (request.expected_pool_generation.valid() &&
        request.expected_pool_generation != entry->descriptor.generation) {
        return Status(ErrorCode::StaleGeneration, "request pool generation is not current");
    }
    if (record.state != AllocationState::Reserved) {
        return Status(ErrorCode::WrongState, "allocation is not in the reserved state");
    }
    if (is_stale_binding(record, *entry, policy, epoch_)) {
        return Status(ErrorCode::StaleGeneration,
                      "allocation is bound to generations that are no longer current");
    }

    AllocationRecord next = record;
    next.state = AllocationState::Committed;
    next.last_touched = stamp;
    next.revision = record.revision + 1;
    BF_TRY(journal_allocation(JournalOp::AllocationCommitted, next));
    entry->reserved_units -= record.units;
    entry->committed_units += record.units;
    adjust_queue(*entry, record.queue, -static_cast<i64>(record.units), true);
    adjust_queue(*entry, record.queue, static_cast<i64>(record.units), false);
    record = next;
    metrics_.allocations_committed += 1;
    push_event(events, EventKind::AllocationCommitted, ErrorCode::Ok, "allocation committed",
               record.pool, record.queue, record.id, record.units);

    const Status closure = check_closure(*entry);
    if (!closure.ok()) {
        shutting_down_ = true;
        return closure;
    }
    Decision decision = make_decision(DecisionKind::NoOp, ErrorCode::Ok, "allocation committed",
                                      *entry, *queue, record.units, record.units, record.id,
                                      BindingConstraint::None, evidence, accounting_of(*entry),
                                      stamp, next_decision_sequence_);
    bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                   record.provenance, evidence.snapshot, epoch_, boot_, PoolId{});
    record_decision(decision);
    ++next_decision_sequence_;
    AttemptRecord attempt_record;
    attempt_record.id = request.attempt;
    attempt_record.kind = AttemptKind::Commit;
    attempt_record.request_digest = digest;
    attempt_record.outcome = ErrorCode::Ok;
    attempt_record.allocation = record.id;
    attempt_record.granted_units = record.units;
    attempt_record.sequence = decision.sequence;
    attempt_record.recorded_at = stamp;
    record_attempt(attempt_record);
    metrics_.decisions += 1;
    return decision;
}

// ---------------------------------------------------------------------------
// Release
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::Impl::release_locked(const ReleaseRequest& request,
                                      std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (!request.attempt.valid()) {
        return Status(ErrorCode::InvalidArgument, "attempt id must be non-zero");
    }
    const Digest digest = request.request_digest();
    AttemptRecord previous;
    bool replay = false;
    BF_TRY(lookup_attempt(request.attempt, digest, &previous, &replay));

    const auto record_it = allocations_.find(request.allocation.raw());
    if (record_it == allocations_.end()) {
        return Status(ErrorCode::UnknownAllocation, "no such allocation");
    }
    AllocationRecord& record = record_it->second;
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(record.pool, &entry));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    QueueDescriptor* queue = nullptr;
    BF_TRY(require_queue(record.queue, &queue));
    const Tick stamp = now();
    const PressureEvidence evidence = resolve_evidence(*entry);

    if (replay) {
        metrics_.idempotent_replays += 1;
        Decision decision = make_decision(DecisionKind::IdempotentReplay, previous.outcome,
                                          "attempt already applied", *entry, *queue, 0,
                                          previous.granted_units, record.id,
                                          BindingConstraint::None, evidence, accounting_of(*entry),
                                          stamp, next_decision_sequence_);
        decision.authority.bind_id(AuthorityKind::Attempt, request.attempt.raw());
        decision.authority.seal();
        return decision;
    }
    if (request.expected_epoch.valid() && request.expected_epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch, "request epoch does not match the fabric epoch");
    }
    if (allocation_is_terminal(record.state) || record.state == AllocationState::Fenced) {
        // Releasing something that already returned its units is a structural
        // no-op rather than an error, so that retries converge.
        Decision decision = make_decision(DecisionKind::NoOp, ErrorCode::Ok,
                                          "allocation no longer holds units", *entry, *queue, 0,
                                          0, record.id, BindingConstraint::None, evidence,
                                          accounting_of(*entry), stamp, next_decision_sequence_);
        bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                       record.provenance, evidence.snapshot, epoch_, boot_, PoolId{});
        record_decision(decision);
        ++next_decision_sequence_;
        AttemptRecord attempt_record;
        attempt_record.id = request.attempt;
        attempt_record.kind = AttemptKind::Release;
        attempt_record.request_digest = digest;
        attempt_record.outcome = ErrorCode::Ok;
        attempt_record.allocation = record.id;
        attempt_record.granted_units = 0;
        attempt_record.sequence = decision.sequence;
        attempt_record.recorded_at = stamp;
        record_attempt(attempt_record);
        metrics_.decisions += 1;
        return decision;
    }

    u64 release_units = record.units;
    if (!request.release_all) {
        if (request.units == 0) {
            return Status(ErrorCode::InvalidArgument, "partial release requires a non-zero amount");
        }
        if (request.units > record.units) {
            return Status(ErrorCode::InvalidArgument, "release exceeds the allocation");
        }
        release_units = request.units;
    }

    AllocationRecord next = record;
    const u64 previous_units = record.units;
    const u64 previous_borrow = record.borrow_units <= previous_units ? record.borrow_units
                                                                    : previous_units;
    // Borrowed units are returned proportionally when only part is released.
    const u64 next_borrow =
        release_units == previous_units
            ? previous_borrow
            : static_cast<u64>((static_cast<unsigned long long>(previous_borrow) * release_units) /
                               previous_units);
    next.units = release_units;
    next.borrow_units = next_borrow;
    next.revision = record.revision + 1;
    next.last_touched = stamp;

    BF_TRY(apply_release_units(*entry, next));
    AllocationRecord remainder = record;
    remainder.units = previous_units - release_units;
    remainder.borrow_units = previous_borrow - next_borrow;
    remainder.last_touched = stamp;
    remainder.revision = record.revision + 1;
    if (remainder.units == 0) {
        remainder.state = AllocationState::Released;
        remainder.borrow_units = 0;
        remainder.lender = PoolId{};
    }
    // The surviving commitment keeps its identity, generational binding and
    // lineage; only its size changes.
    BF_TRY(journal_allocation(JournalOp::AllocationReleased, remainder));
    record = remainder;
    metrics_.allocations_released += 1;
    if (metrics_.live_committed_units >= release_units) {
        metrics_.live_committed_units -= release_units;
    } else {
        metrics_.live_committed_units = 0;
    }
    push_event(events, EventKind::AllocationReleased, ErrorCode::Ok, "allocation released",
               record.pool, record.queue, record.id, release_units);

    const Status closure = check_closure(*entry);
    if (!closure.ok()) {
        shutting_down_ = true;
        return closure;
    }

    Decision decision = make_decision(DecisionKind::NoOp, ErrorCode::Ok, "allocation released",
                                      *entry, *queue, release_units, release_units, record.id,
                                      BindingConstraint::None, evidence, accounting_of(*entry),
                                      stamp, next_decision_sequence_);
    bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                   record.provenance, evidence.snapshot, epoch_, boot_, PoolId{});
    record_decision(decision);
    ++next_decision_sequence_;
    AttemptRecord attempt_record;
    attempt_record.id = request.attempt;
    attempt_record.kind = AttemptKind::Release;
    attempt_record.request_digest = digest;
    attempt_record.outcome = ErrorCode::Ok;
    attempt_record.allocation = record.id;
    attempt_record.granted_units = release_units;
    attempt_record.sequence = decision.sequence;
    attempt_record.recorded_at = stamp;
    record_attempt(attempt_record);
    metrics_.decisions += 1;
    return decision;
}

// ---------------------------------------------------------------------------
// Revalidate
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::Impl::revalidate_locked(const RevalidateRequest& request,
                                         std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (!request.attempt.valid()) {
        return Status(ErrorCode::InvalidArgument, "attempt id must be non-zero");
    }
    const Digest digest = request.request_digest();
    AttemptRecord previous;
    bool replay = false;
    BF_TRY(lookup_attempt(request.attempt, digest, &previous, &replay));

    const auto record_it = allocations_.find(request.allocation.raw());
    if (record_it == allocations_.end()) {
        return Status(ErrorCode::UnknownAllocation, "no such allocation");
    }
    AllocationRecord& record = record_it->second;
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(record.pool, &entry));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    QueueDescriptor* queue = nullptr;
    BF_TRY(require_queue(record.queue, &queue));
    const Tick stamp = now();
    const PressureEvidence evidence = resolve_evidence(*entry);

    if (replay) {
        metrics_.idempotent_replays += 1;
        Decision decision = make_decision(DecisionKind::IdempotentReplay, previous.outcome,
                                          "attempt already applied", *entry, *queue, 0,
                                          previous.granted_units, record.id,
                                          BindingConstraint::None, evidence, accounting_of(*entry),
                                          stamp, next_decision_sequence_);
        decision.authority.bind_id(AuthorityKind::Attempt, request.attempt.raw());
        decision.authority.seal();
        return decision;
    }
    if (request.expected_epoch.valid() && request.expected_epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch, "request epoch does not match the fabric epoch");
    }
    if (!allocation_holds_units(record.state)) {
        return Status(ErrorCode::Fenced,
                      "a fenced allocation must be re-allocated rather than revalidated");
    }
    if (queue->pool != entry->descriptor.id) {
        return Status(ErrorCode::StaleGeneration, "queue is bound to a different pool");
    }
    if (is_stale_binding(record, *entry, policy, epoch_)) {
        CorrectiveIntent intent;
        intent.fence_required = true;
        Decision decision = make_decision(DecisionKind::Fence, ErrorCode::StaleGeneration,
                                          "allocation is bound to stale generations", *entry,
                                          *queue, 0, 0, record.id,
                                          BindingConstraint::GenerationMismatch, evidence,
                                          accounting_of(*entry), stamp, next_decision_sequence_);
        decision.intent = intent;
        bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                       record.provenance, evidence.snapshot, epoch_, boot_, PoolId{});
        record_decision(decision);
        ++next_decision_sequence_;
        return decision;
    }

    // Re-binding to the current epoch is the transactional effect: the
    // allocation keeps its units and lineage but is now bound to current
    // generations.
    AllocationRecord rebound = record;
    rebound.bound.pool_generation = entry->descriptor.generation;
    rebound.bound.queue_generation = queue->generation;
    rebound.bound.policy_generation = policy.generation;
    rebound.bound.capacity_generation = entry->capacity_generation;
    rebound.bound.epoch = epoch_;
    rebound.bound.boot = boot_;
    rebound.last_touched = stamp;
    rebound.revision = record.revision + 1;
    BF_TRY(journal_allocation(JournalOp::AllocationRevalidated, rebound));
    record = rebound;
    metrics_.revalidations += 1;
    push_event(events, EventKind::AllocationRevalidated, ErrorCode::Ok, "allocation revalidated",
               record.pool, record.queue, record.id, record.units);

    Decision decision = make_decision(DecisionKind::NoOp, ErrorCode::Ok, "allocation revalidated",
                                      *entry, *queue, 0, record.units, record.id,
                                      BindingConstraint::None, evidence, accounting_of(*entry),
                                      stamp, next_decision_sequence_);
    bind_authority(&decision.authority, *entry, *queue, policy, request.attempt,
                   record.provenance, evidence.snapshot, epoch_, boot_, PoolId{});
    record_decision(decision);
    ++next_decision_sequence_;
    AttemptRecord attempt_record;
    attempt_record.id = request.attempt;
    attempt_record.kind = AttemptKind::Revalidate;
    attempt_record.request_digest = digest;
    attempt_record.outcome = ErrorCode::Ok;
    attempt_record.allocation = record.id;
    attempt_record.granted_units = record.units;
    attempt_record.sequence = decision.sequence;
    attempt_record.recorded_at = stamp;
    record_attempt(attempt_record);
    metrics_.decisions += 1;
    return decision;
}

// ---------------------------------------------------------------------------
// Fence and expiry
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::Impl::fence_stale_locked(PoolId pool_id, std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(pool_id, &entry));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    QueueDescriptor placeholder;
    placeholder.id = QueueId::from_raw(0);
    placeholder.pool = pool_id;
    const Tick stamp = now();
    const PressureEvidence evidence = resolve_evidence(*entry);

    u64 fenced_allocations = 0;
    u64 fenced_units = 0;
    u64 fenced_borrow = 0;
    const std::vector<u64> members = sorted_members(*entry);
    for (const u64 raw_id : members) {
        auto it = allocations_.find(raw_id);
        if (it == allocations_.end()) continue;
        AllocationRecord& record = it->second;
        if (!allocation_holds_units(record.state)) continue;
        if (!is_stale_binding(record, *entry, policy, epoch_)) continue;
        const u64 units = record.units;
        const u64 borrow = record.borrow_units;
        BF_TRY(fence_allocation_locked(record, AllocationState::Fenced, ErrorCode::StaleGeneration,
                                       events));
        fenced_allocations += 1;
        fenced_units += units;
        fenced_borrow += borrow;
    }
    metrics_.fences += 1;
    Decision decision = make_decision(fenced_allocations == 0 ? DecisionKind::NoOp
                                                              : DecisionKind::Fence,
                                      ErrorCode::Ok,
                                      fenced_allocations == 0 ? "no stale allocations"
                                                              : "stale allocations fenced",
                                      *entry, placeholder, 0, fenced_units, AllocationId{},
                                      BindingConstraint::GenerationMismatch, evidence,
                                      accounting_of(*entry), stamp, next_decision_sequence_);
    decision.intent.fence_required = fenced_allocations != 0;
    bind_authority(&decision.authority, *entry, placeholder, policy, AttemptId{},
                   ProvenanceId{}, evidence.snapshot, epoch_, boot_, PoolId{});
    record_decision(decision);
    ++next_decision_sequence_;
    metrics_.decisions += 1;
    return decision;
}

Result<Decision> BufferFabric::Impl::expire_reservations_locked(PoolId pool_id,
                                                  std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(pool_id, &entry));
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    const Tick stamp = now();
    const PressureEvidence evidence = resolve_evidence(*entry);
    QueueDescriptor placeholder;
    placeholder.pool = pool_id;

    u64 expired = 0;
    u64 expired_units = 0;
    const std::vector<u64> members = sorted_members(*entry);
    for (const u64 raw_id : members) {
        auto it = allocations_.find(raw_id);
        if (it == allocations_.end()) continue;
        AllocationRecord& record = it->second;
        if (record.state != AllocationState::Reserved) continue;
        Tick expiry = record.expires_at;
        if (expiry == 0 && policy.reservation_ttl_ticks != 0) {
            if (add_overflow(record.created_at, policy.reservation_ttl_ticks, &expiry)) {
                expiry = (std::numeric_limits<u64>::max)();
            }
        }
        if (expiry == 0 || stamp < expiry) continue;
        expired_units += record.units;
        BF_TRY(fence_allocation_locked(record, AllocationState::Expired, ErrorCode::StaleEvidence,
                                       events));
        expired += 1;
    }
    Decision decision = make_decision(expired == 0 ? DecisionKind::NoOp : DecisionKind::Reclaim,
                                      ErrorCode::Ok,
                                      expired == 0 ? "no reservations expired"
                                                   : "reservations expired",
                                      *entry, placeholder, 0, expired_units, AllocationId{},
                                      BindingConstraint::None, evidence, accounting_of(*entry),
                                      stamp, next_decision_sequence_);
    bind_authority(&decision.authority, *entry, placeholder, policy, AttemptId{},
                   ProvenanceId{}, evidence.snapshot, epoch_, boot_, PoolId{});
    record_decision(decision);
    ++next_decision_sequence_;
    metrics_.decisions += 1;
    return decision;
}

}  // namespace buffer_fabric
