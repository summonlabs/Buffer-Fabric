#include "fabric_impl.hpp"

#include <algorithm>
#include <utility>

#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/version.hpp"

namespace buffer_fabric {
namespace {

[[nodiscard]] std::string build_id() {
    return std::string(BUFFER_FABRIC_VERSION_STRING);
}

}  // namespace

// ---------------------------------------------------------------------------
// Allocation entry points
//
// Every mutating entry point follows the same shape: take the authoritative
// lock, perform the mutation and collect events, release the lock, then publish
// the events. No user-supplied code is ever invoked while the lock is held.
// ---------------------------------------------------------------------------

Result<Decision> BufferFabric::allocate(const AllocateRequest& request) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        result = impl_->allocate_locked(request, events);
    }
    impl_->publish(events);
    return result;
}

Result<Decision> BufferFabric::commit(const CommitRequest& request) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        result = impl_->commit_locked(request, events);
    }
    impl_->publish(events);
    return result;
}

Result<Decision> BufferFabric::release(const ReleaseRequest& request) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        result = impl_->release_locked(request, events);
    }
    impl_->publish(events);
    return result;
}

Result<Decision> BufferFabric::revalidate(const RevalidateRequest& request) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        result = impl_->revalidate_locked(request, events);
    }
    impl_->publish(events);
    return result;
}

Result<Decision> BufferFabric::fence_stale(PoolId pool_id) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        result = impl_->fence_stale_locked(pool_id, events);
    }
    impl_->publish(events);
    return result;
}

Result<Decision> BufferFabric::expire_reservations(PoolId pool_id) {
    std::vector<FabricEvent> events;
    Result<Decision> result{Decision{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        result = impl_->expire_reservations_locked(pool_id, events);
    }
    impl_->publish(events);
    return result;
}

Result<EvaluationResult> BufferFabric::evaluate(const EvaluateRequest& request) const {
    BufferFabric::Impl::Lock guard(*impl_);
    if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (request.requested_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "requested units exceed the supported bound");
    }
    PoolEntry* entry = nullptr;
    BF_TRY(impl_->require_pool(request.pool, &entry));
    QueueDescriptor* queue = nullptr;
    BF_TRY(impl_->require_queue(request.queue, &queue));
    if (queue->pool != entry->descriptor.id) {
        return Status(ErrorCode::InvalidArgument, "queue is not bound to the requested pool");
    }
    const auto policy_it = impl_->policies_.find(entry->descriptor.policy.raw());
    if (policy_it == impl_->policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    const PressureEvidence evidence = impl_->resolve_evidence(*entry);

    EvaluationResult result;
    result.accounting = impl_->accounting_of(*entry);
    result.evidence = evidence;
    result.decision.pool = entry->descriptor.id;
    result.decision.queue = queue->id;
    result.decision.requested_units = request.requested_units;
    result.decision.pressure = evidence.state;
    result.decision.utilization_bp = evidence.utilization_bp;
    result.decision.accounting = result.accounting;
    result.decision.decided_at = impl_->now();
    result.decision.sequence = impl_->next_decision_sequence_;

    if (request.expected_epoch.valid() && request.expected_epoch != impl_->epoch_) {
        result.decision.kind = DecisionKind::Revalidate;
        result.decision.status = ErrorCode::StaleEpoch;
        result.decision.reason = "request epoch does not match the fabric epoch";
        result.decision.binding = BindingConstraint::GenerationMismatch;
        result.decision.intent.revalidate_required = true;
        return result;
    }
    if (entry->descriptor.retired) {
        result.decision.kind = DecisionKind::Refuse;
        result.decision.status = ErrorCode::WrongState;
        result.decision.reason = "pool is retired";
        result.decision.binding = BindingConstraint::PoolRetired;
        return result;
    }
    if (queue->retired) {
        result.decision.kind = DecisionKind::Refuse;
        result.decision.status = ErrorCode::WrongState;
        result.decision.reason = "queue is retired";
        result.decision.binding = BindingConstraint::QueueRetired;
        return result;
    }
    if (request.requested_units == 0) {
        result.decision.kind = DecisionKind::NoOp;
        result.decision.reason = "no capacity requested";
        result.decision.binding = BindingConstraint::RequestZero;
        return result;
    }
    if (!evidence.usable() && policy.pressure.refuse_increase_on_unknown) {
        result.decision.kind = DecisionKind::Revalidate;
        result.decision.status = evidence.reason;
        result.decision.reason = "fresh pressure evidence is required";
        result.decision.binding = evidence.state == PressureState::Unknown
                                      ? BindingConstraint::EvidenceUnknown
                                      : BindingConstraint::EvidenceStale;
        result.decision.intent.revalidate_required = true;
        return result;
    }
    if (pressure_is_known(evidence.state) && policy.refuse_grants_at != PressureState::Unknown &&
        pressure_at_least(evidence.state, policy.refuse_grants_at)) {
        result.decision.kind = DecisionKind::Reduce;
        result.decision.status = ErrorCode::PolicyForbids;
        result.decision.reason = "pressure is at or above the refusal band";
        result.decision.binding = BindingConstraint::PressureRefuse;
        result.decision.intent.reduce_required = true;
        result.decision.intent.reduce_to_units = entry->usable();
        return result;
    }

    BufferFabric::Impl::GrantComputation computation =
        impl_->compute_grant(*entry, policy, request.requested_units);
    u64 limit = computation.max_grant;
    BindingConstraint binding = computation.binding;
    if (policy.max_single_request_units != 0 && policy.max_single_request_units < limit) {
        limit = policy.max_single_request_units;
        binding = BindingConstraint::PolicyMaxSingleRequest;
    }
    if (policy.max_single_allocation_units != 0 && policy.max_single_allocation_units < limit) {
        limit = policy.max_single_allocation_units;
        binding = BindingConstraint::PolicyMaxSingleAllocation;
    }
    const auto committed_it = entry->queue_committed.find(queue->id.raw());
    const auto reserved_it = entry->queue_reserved.find(queue->id.raw());
    const u64 held = (committed_it == entry->queue_committed.end() ? 0 : committed_it->second) +
                     (reserved_it == entry->queue_reserved.end() ? 0 : reserved_it->second);
    if (queue->max_committed_units != 0) {
        const u64 room = queue->max_committed_units > held ? queue->max_committed_units - held : 0;
        if (room < limit) {
            limit = room;
            binding = BindingConstraint::QueueMaxCommitted;
        }
    } else if (queue->demand_units != 0) {
        const u64 room = queue->demand_units > held ? queue->demand_units - held : 0;
        if (room < limit) {
            limit = room;
            binding = BindingConstraint::QueueShare;
        }
    }
    u64 grant = std::min(request.requested_units, limit);
    if (!policy.allow_partial_grant && grant < request.requested_units) {
        grant = 0;
    }
    if (grant == 0) {
        result.decision.kind = DecisionKind::Refuse;
        result.decision.status = ErrorCode::CapacityExceeded;
        result.decision.reason = "no capacity is available under the governing policy";
        result.decision.binding = binding;
        return result;
    }
    result.decision.kind =
        grant == request.requested_units ? DecisionKind::Grant : DecisionKind::GrantPartial;
    result.decision.reason = "capacity would be granted";
    result.decision.granted_units = grant;
    result.decision.binding = binding;
    result.decision.authority.bind_generation(AuthorityKind::Pool, entry->descriptor.generation);
    result.decision.authority.bind_generation(AuthorityKind::Queue, queue->generation);
    result.decision.authority.bind_generation(AuthorityKind::Policy, policy.generation);
    result.decision.authority.bind(AuthorityKind::Capacity, entry->capacity_generation.raw(),
                                   entry->capacity_generation);
    result.decision.authority.bind_id(AuthorityKind::Epoch, impl_->epoch_.raw());
    result.decision.authority.bind_id(AuthorityKind::Pressure, evidence.snapshot.raw());
    result.decision.authority.bind(AuthorityKind::BootIncarnation, impl_->boot_.hi,
                                   Generation::from_raw(impl_->boot_.lo));
    result.decision.authority.seal();
    return result;
}

// ---------------------------------------------------------------------------
// Reclamation
// ---------------------------------------------------------------------------

Result<ReclamationPlan> BufferFabric::Impl::plan_reclaim_locked(const ReclaimRequest& request,
                                                  std::vector<FabricEvent>& events) {
    if (!open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    if (request.target_units == 0) {
        return Status(ErrorCode::InvalidArgument, "reclamation target must be non-zero");
    }
    if (request.target_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "reclamation target exceeds the supported bound");
    }
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(request.pool, &entry));
    if (request.expected_epoch.valid() && request.expected_epoch != epoch_) {
        return Status(ErrorCode::StaleEpoch, "request epoch does not match the fabric epoch");
    }
    if (request.expected_pool_generation.valid() &&
        request.expected_pool_generation != entry->descriptor.generation) {
        return Status(ErrorCode::StaleGeneration, "request pool generation is not current");
    }
    const auto policy_it = policies_.find(entry->descriptor.policy.raw());
    if (policy_it == policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    if (!policy.reclaim.enabled) {
        return Status(ErrorCode::PolicyForbids, "reclamation is disabled for this pool");
    }
    const Tick stamp = now();
    const PressureEvidence evidence = resolve_evidence(*entry);

    struct Candidate {
        AllocationId id{};
        QueueId queue{};
        u64 units{0};
        u64 borrow{0};
        u64 priority{0};
        Tick last_touched{0};
        ReclaimClass klass{ReclaimClass::Reclaimable};
    };
    std::vector<Candidate> reclaimable;
    std::vector<Candidate> pinned;
    for (const u64 raw_id : entry->members) {
        const auto it = allocations_.find(raw_id);
        if (it == allocations_.end()) continue;
        const AllocationRecord& record = it->second;
        if (!allocation_holds_units(record.state)) continue;
        if (policy.reclaim.min_hold_ticks != 0) {
            const Tick age = stamp >= record.last_touched ? stamp - record.last_touched : 0;
            if (age < policy.reclaim.min_hold_ticks) continue;
        }
        Candidate candidate;
        candidate.id = record.id;
        candidate.queue = record.queue;
        candidate.units = record.units;
        candidate.borrow = record.borrow_units;
        candidate.priority = record.reclaim_priority;
        candidate.last_touched = record.last_touched;
        candidate.klass = record.reclaim_class;
        if (record.reclaim_class == ReclaimClass::Reclaimable) {
            reclaimable.push_back(candidate);
        } else {
            pinned.push_back(candidate);
        }
    }
    auto ordering = [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.last_touched != b.last_touched) return a.last_touched < b.last_touched;
        return a.id.raw() < b.id.raw();
    };
    std::sort(reclaimable.begin(), reclaimable.end(), ordering);
    std::sort(pinned.begin(), pinned.end(), ordering);

    ReclamationPlan plan;
    plan.pool = entry->descriptor.id;
    plan.pool_generation = entry->descriptor.generation;
    plan.epoch = epoch_;
    plan.target_units = request.target_units;
    for (const auto& candidate : reclaimable) plan.reclaimable_available_units += candidate.units;
    for (const auto& candidate : pinned) plan.pinned_available_units += candidate.units;

    const u64 starting_free = entry->free_units();
    u64 planned = 0;
    auto needs_more = [&]() {
        if (planned < request.target_units) return true;
        const u64 resulting_free = starting_free + planned;
        return resulting_free < policy.reclaim.reserve_units;
    };
    auto accept = [&](const Candidate& candidate) {
        if (plan.victims.size() >= kMaxReclaimVictims) {
            plan.truncated = true;
            return false;
        }
        if (planned + candidate.units > policy.reclaim.max_units_per_operation) {
            plan.truncated = true;
            return false;
        }
        ReclaimVictim victim;
        victim.allocation = candidate.id;
        victim.queue = candidate.queue;
        victim.units = candidate.units;
        victim.borrowed_units = candidate.borrow;
        victim.reclaim_priority = candidate.priority;
        victim.last_touched = candidate.last_touched;
        victim.reclaim_class = candidate.klass;
        plan.victims.push_back(victim);
        planned += candidate.units;
        return true;
    };

    for (const auto& candidate : reclaimable) {
        if (!needs_more()) break;
        if (!accept(candidate)) break;
    }
    // Protected commitments are only reachable when the policy says so
    // explicitly, and only after every reclaimable allocation is exhausted.
    if (needs_more() && policy.reclaim.allow_protected_reclaim) {
        for (const auto& candidate : pinned) {
            if (!needs_more()) break;
            if (!accept(candidate)) break;
            plan.protected_reclaim_used = true;
        }
    }
    plan.planned_units = planned;
    plan.shortfall_units = request.target_units > planned ? request.target_units - planned : 0;
    plan.built_at = stamp;
    plan.authority.bind_generation(AuthorityKind::Pool, entry->descriptor.generation);
    plan.authority.bind_generation(AuthorityKind::Policy, policy.generation);
    plan.authority.bind_id(AuthorityKind::Epoch, epoch_.raw());
    plan.authority.bind_id(AuthorityKind::Pressure, evidence.snapshot.raw());
    plan.authority.seal();

    ByteWriter digest_writer(256);
    digest_writer.put_id(plan.pool);
    digest_writer.put_id(plan.pool_generation);
    digest_writer.put_id(plan.epoch);
    digest_writer.put_u64(plan.target_units);
    digest_writer.put_u64(plan.planned_units);
    digest_writer.put_u32(static_cast<u32>(plan.victims.size()));
    for (const auto& victim : plan.victims) {
        digest_writer.put_id(victim.allocation);
        digest_writer.put_u64(victim.units);
        digest_writer.put_u64(victim.reclaim_priority);
        digest_writer.put_u64(victim.last_touched);
        digest_writer.put_u8(static_cast<u8>(victim.reclaim_class));
    }
    plan.digest = digest_bytes(digest_writer.data().data(), digest_writer.size());
    push_event(events, EventKind::DecisionRefused, ErrorCode::Ok, "reclamation plan built",
               plan.pool, QueueId{}, AllocationId{}, plan.planned_units);
    return plan;
}

Result<ReclaimResult> BufferFabric::Impl::apply_reclaim_locked(const ReclaimRequest& request,
                                                 std::vector<FabricEvent>& events) {
    std::vector<FabricEvent> planning_events;
    Result<ReclamationPlan> plan_result = plan_reclaim_locked(request, planning_events);
    if (!plan_result.ok()) return plan_result.status();
    ReclamationPlan plan = std::move(plan_result).value();
    PoolEntry* entry = nullptr;
    BF_TRY(require_pool(plan.pool, &entry));

    ReclaimResult result;
    result.plan = plan;
    for (const auto& victim : plan.victims) {
        auto it = allocations_.find(victim.allocation.raw());
        if (it == allocations_.end()) continue;
        AllocationRecord& record = it->second;
        if (!allocation_holds_units(record.state)) continue;
        const u64 units = record.units;
        const u64 borrow = record.borrow_units;
        BF_TRY(fence_allocation_locked(record, AllocationState::Reclaimed,
                                       ErrorCode::PolicyForbids, events));
        result.reclaimed_units += units;
        result.revoked_allocations += 1;
        if (borrow != 0) {
            result.borrower_units_returned += borrow;
            result.lender_units_returned += borrow;
        }
    }
    result.accounting = accounting_of(*entry);
    result.applied_at = now();
    metrics_.reclaims += 1;
    const Status closure = check_closure(*entry);
    if (!closure.ok()) {
        shutting_down_ = true;
        push_event(events, EventKind::AccountingViolation, closure.code(), closure.message(),
                   entry->descriptor.id);
        return closure;
    }
    return result;
}

Result<ReclamationPlan> BufferFabric::plan_reclaim(const ReclaimRequest& request) const {
    BufferFabric::Impl::Lock guard(*impl_);
    std::vector<FabricEvent> scratch;
    return impl_->plan_reclaim_locked(request, scratch);
}

Result<ReclaimResult> BufferFabric::apply_reclaim(const ReclaimRequest& request) {
    std::vector<FabricEvent> events;
    Result<ReclaimResult> result{ReclaimResult{}};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        result = impl_->apply_reclaim_locked(request, events);
    }
    impl_->publish(events);
    return result;
}

// ---------------------------------------------------------------------------
// Explanation and inspection
// ---------------------------------------------------------------------------

Result<Explanation> BufferFabric::explain(PoolId pool_id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    PoolEntry* entry = nullptr;
    BF_TRY(impl_->require_pool(pool_id, &entry));
    const auto policy_it = impl_->policies_.find(entry->descriptor.policy.raw());
    if (policy_it == impl_->policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    const PoolPolicy& policy = policy_it->second;
    const PoolAccounting accounting = impl_->accounting_of(*entry);
    const PressureEvidence evidence = impl_->resolve_evidence(*entry);

    Explanation explanation;
    explanation.pool = entry->descriptor.id;
    explanation.pool_generation = entry->descriptor.generation;
    explanation.epoch = impl_->epoch_;
    explanation.boot = impl_->boot_;
    explanation.sequence = impl_->next_decision_sequence_;
    explanation.generated_at = impl_->now();
    explanation.raw_units = accounting.raw;
    explanation.protected_headroom_units = accounting.protected_units;
    explanation.usable_units = accounting.usable;
    explanation.allocated_units = accounting.allocated;
    explanation.reserved_units = accounting.reserved;
    explanation.committed_units = accounting.committed;
    explanation.pinned_units = accounting.pinned;
    explanation.reclaimable_units = accounting.reclaimable;
    explanation.free_units = accounting.free;
    explanation.borrowed_in_units = accounting.borrowed_in;
    explanation.lent_out_units = accounting.lent_out;
    explanation.overcommit_used_units = accounting.overcommit_used;
    explanation.overcommit_limit_units = accounting.overcommit_limit;
    explanation.pressure = evidence.state;
    explanation.utilization_bp = evidence.utilization_bp;
    explanation.binding_bp = evidence.binding_bp;
    explanation.thresholds = policy.pressure.thresholds;
    explanation.evidence = evidence;
    explanation.binding = BindingConstraint::None;
    explanation.accounting = check_pool_accounting(accounting, policy.overcommit.mode,
                                                  policy.overcommit.limit_units);
    if (pressure_is_known(evidence.state) && pressure_at_least(evidence.state, policy.reduce_at)) {
        explanation.intent.reduce_required = true;
        explanation.intent.reduce_to_units = accounting.usable;
    }
    if (!evidence.usable()) {
        explanation.intent.revalidate_required = true;
    }
    explanation.authority.bind(AuthorityKind::Capacity, entry->capacity_generation.raw(),
                               entry->capacity_generation);
    explanation.authority.bind_generation(AuthorityKind::Pool, entry->descriptor.generation);
    explanation.authority.bind_generation(AuthorityKind::Policy, policy.generation);
    explanation.authority.bind_id(AuthorityKind::Pressure, evidence.snapshot.raw());
    explanation.authority.bind_id(AuthorityKind::Epoch, impl_->epoch_.raw());
    explanation.authority.bind_id(AuthorityKind::Backend, entry->descriptor.backend.raw());
    explanation.authority.bind(AuthorityKind::BootIncarnation, impl_->boot_.hi,
                               Generation::from_raw(impl_->boot_.lo));
    explanation.authority.seal();

    for (const u64 raw_id : entry->members) {
        const auto it = impl_->allocations_.find(raw_id);
        if (it == impl_->allocations_.end()) continue;
        const AllocationRecord& record = it->second;
        if (!allocation_holds_units(record.state)) continue;
        bool found = false;
        for (auto& usage : explanation.queues) {
            if (usage.queue != record.queue) continue;
            usage.committed_units += record.state == AllocationState::Committed ? record.units : 0;
            usage.reserved_units += record.state == AllocationState::Reserved ? record.units : 0;
            usage.allocation_count += 1;
            found = true;
            break;
        }
        if (!found) {
            QueueUsage usage;
            usage.queue = record.queue;
            usage.pool = entry->descriptor.id;
            const auto queue_it = impl_->queues_.find(record.queue.raw());
            if (queue_it != impl_->queues_.end()) usage.queue_generation = queue_it->second.generation;
            usage.committed_units = record.state == AllocationState::Committed ? record.units : 0;
            usage.reserved_units = record.state == AllocationState::Reserved ? record.units : 0;
            usage.allocation_count = 1;
            explanation.queues.push_back(usage);
        }
    }
    std::sort(explanation.queues.begin(), explanation.queues.end(),
              [](const QueueUsage& a, const QueueUsage& b) { return a.queue.raw() < b.queue.raw(); });

    ByteWriter writer(512);
    writer.put_id(explanation.pool);
    writer.put_id(explanation.pool_generation);
    writer.put_id(explanation.epoch);
    writer.put_u64(explanation.raw_units);
    writer.put_u64(explanation.protected_headroom_units);
    writer.put_u64(explanation.allocated_units);
    writer.put_u64(explanation.free_units);
    writer.put_u64(explanation.borrowed_in_units);
    writer.put_u64(explanation.lent_out_units);
    writer.put_u64(explanation.overcommit_used_units);
    writer.put_u8(static_cast<u8>(explanation.pressure));
    writer.put_u32(explanation.utilization_bp);
    writer.put_u64(explanation.thresholds.elevated_bp);
    writer.put_u64(explanation.thresholds.high_bp);
    writer.put_u64(explanation.thresholds.critical_bp);
    writer.put_digest(explanation.authority.digest());
    writer.put_u32(static_cast<u32>(explanation.queues.size()));
    for (const auto& usage : explanation.queues) {
        writer.put_id(usage.queue);
        writer.put_u64(usage.committed_units);
        writer.put_u64(usage.reserved_units);
    }
    explanation.digest = digest_bytes(writer.data().data(), writer.size());
    return explanation;
}

Result<FabricSummary> BufferFabric::summary() const {
    BufferFabric::Impl::Lock guard(*impl_);
    FabricSummary summary;
    summary.epoch = impl_->epoch_;
    summary.boot = impl_->boot_;
    summary.pool_count = impl_->pools_.size();
    summary.queue_count = impl_->queues_.size();
    summary.allocation_count = impl_->allocations_.size();
    std::vector<PoolAccounting> ledger;
    ledger.reserve(impl_->pools_.size());
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(id);
        ledger.push_back(impl_->accounting_of(entry));
    }
    for (const auto& accounting : ledger) {
        summary.raw_total += accounting.raw;
        summary.protected_total += accounting.protected_units;
        summary.allocated_total += accounting.allocated;
        summary.free_total += accounting.free;
        summary.overcommit_total += accounting.overcommit_used;
        summary.borrowed_total += accounting.borrowed_in;
        summary.lent_total += accounting.lent_out;
        summary.live_allocations += accounting.reserved + accounting.committed;
    }
    summary.accounting = check_global_accounting(ledger);
    return summary;
}

Result<std::vector<PoolAccounting>> BufferFabric::ledger() const {
    BufferFabric::Impl::Lock guard(*impl_);
    std::vector<PoolAccounting> out;
    out.reserve(impl_->pools_.size());
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(id);
        out.push_back(impl_->accounting_of(entry));
    }
    std::sort(out.begin(), out.end(),
              [](const PoolAccounting& a, const PoolAccounting& b) { return a.pool < b.pool; });
    return out;
}

Result<PoolView> BufferFabric::pool_view(PoolId pool_id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    PoolEntry* entry = nullptr;
    BF_TRY(impl_->require_pool(pool_id, &entry));
    const auto policy_it = impl_->policies_.find(entry->descriptor.policy.raw());
    if (policy_it == impl_->policies_.end()) {
        return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
    }
    PoolView view;
    view.descriptor = entry->descriptor;
    view.policy = policy_it->second;
    view.runtime = entry->runtime;
    view.raw_units = entry->descriptor.raw_units;
    view.protected_units = entry->descriptor.protected_units;
    view.usable_units = entry->usable();
    view.allocated_units = entry->allocated();
    view.free_units = entry->free_units();
    for (const auto& [queue_id, units] : entry->queue_committed) {
        QueueUsage usage;
        usage.queue = QueueId::from_raw(queue_id);
        usage.pool = entry->descriptor.id;
        usage.committed_units = units;
        const auto queue_it = impl_->queues_.find(queue_id);
        if (queue_it != impl_->queues_.end()) {
            usage.queue_generation = queue_it->second.generation;
        }
        view.queues.push_back(usage);
    }
    std::sort(view.queues.begin(), view.queues.end(),
              [](const QueueUsage& a, const QueueUsage& b) { return a.queue < b.queue; });
    return view;
}

Result<std::vector<PoolView>> BufferFabric::pool_views() const {
    BufferFabric::Impl::Lock guard(*impl_);
    std::vector<PoolView> out;
    out.reserve(impl_->pools_.size());
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(id);
        PoolView view;
        view.descriptor = entry.descriptor;
        const auto policy_it = impl_->policies_.find(entry.descriptor.policy.raw());
        if (policy_it != impl_->policies_.end()) view.policy = policy_it->second;
        view.runtime = entry.runtime;
        view.raw_units = entry.descriptor.raw_units;
        view.protected_units = entry.descriptor.protected_units;
        view.usable_units = entry.usable();
        view.allocated_units = entry.allocated();
        view.free_units = entry.free_units();
        out.push_back(std::move(view));
    }
    std::sort(out.begin(), out.end(), [](const PoolView& a, const PoolView& b) {
        return a.descriptor.id < b.descriptor.id;
    });
    return out;
}

Result<AllocationRecord> BufferFabric::allocation(AllocationId id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    if (!id.valid()) return Status(ErrorCode::InvalidArgument, "allocation id must be non-zero");
    const auto it = impl_->allocations_.find(id.raw());
    if (it == impl_->allocations_.end()) {
        return Status(ErrorCode::UnknownAllocation, "no such allocation");
    }
    return it->second;
}

Result<std::vector<AllocationRecord>> BufferFabric::allocations(PoolId pool_id, u64 limit) const {
    BufferFabric::Impl::Lock guard(*impl_);
    PoolEntry* entry = nullptr;
    BF_TRY(impl_->require_pool(pool_id, &entry));
    const u64 bound = std::min(limit == 0 ? impl_->config_.max_inspection_rows : limit,
                               impl_->config_.max_inspection_rows);
    std::vector<u64> ids(entry->members.begin(), entry->members.end());
    std::sort(ids.begin(), ids.end());
    std::vector<AllocationRecord> out;
    out.reserve(static_cast<usize>(std::min<u64>(bound, ids.size())));
    for (const u64 raw_id : ids) {
        if (out.size() >= bound) break;
        const auto it = impl_->allocations_.find(raw_id);
        if (it == impl_->allocations_.end()) continue;
        out.push_back(it->second);
    }
    return out;
}

Result<std::vector<DecisionRecord>> BufferFabric::decision_history(u64 limit) const {
    BufferFabric::Impl::Lock guard(*impl_);
    const u64 bound = std::min(limit == 0 ? impl_->config_.decision_history_capacity : limit,
                               impl_->config_.decision_history_capacity);
    std::vector<DecisionRecord> out;
    const u64 total = impl_->decisions_.size();
    const u64 start = total > bound ? total - bound : 0;
    for (u64 i = start; i < total; ++i) {
        out.push_back(impl_->decisions_[static_cast<usize>(i)]);
    }
    return out;
}

Result<std::vector<AttemptRecord>> BufferFabric::attempt_history(u64 limit) const {
    BufferFabric::Impl::Lock guard(*impl_);
    const u64 bound = std::min(limit == 0 ? impl_->config_.attempt_memory_capacity : limit,
                               impl_->config_.attempt_memory_capacity);
    std::vector<AttemptRecord> out;
    out.reserve(impl_->attempts_.size());
    for (const auto& [id, record] : impl_->attempts_) {
        BF_UNUSED(id);
        out.push_back(record);
    }
    std::sort(out.begin(), out.end(), [](const AttemptRecord& a, const AttemptRecord& b) {
        if (a.sequence != b.sequence) return a.sequence < b.sequence;
        return a.id.raw() < b.id.raw();
    });
    if (out.size() > bound) {
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(out.size() - bound));
    }
    return out;
}

FabricStatistics BufferFabric::statistics() const {
    BufferFabric::Impl::Lock guard(*impl_);
    FabricStatistics stats;
    stats.pools = impl_->pools_.size();
    stats.policies = impl_->policies_.size();
    stats.queues = impl_->queues_.size();
    stats.allocations_total = impl_->allocations_total_;
    stats.decisions = impl_->metrics_.decisions;
    stats.attempts_retained = impl_->attempts_.size();
    stats.pressure_snapshots = impl_->pressure_.size();
    stats.durable_records = impl_->metrics_.journal_records_written;
    stats.durable_bytes = impl_->metrics_.journal_bytes_written;
    u64 live = 0;
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(id);
        live += entry.members.size();
    }
    stats.allocations_live = live;
    return stats;
}

FabricMetrics BufferFabric::metrics() const {
    BufferFabric::Impl::Lock guard(*impl_);
    FabricMetrics metrics = impl_->metrics_;
    u64 live = 0;
    u64 committed = 0;
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(id);
        live += entry.members.size();
        committed += entry.committed_units;
    }
    metrics.live_allocations = live;
    metrics.live_committed_units = committed;
    return metrics;
}

AccountingReport BufferFabric::validate_accounting() const {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->deep_verify();
}

Result<std::string> BufferFabric::accounting_text() const {
    BufferFabric::Impl::Lock guard(*impl_);
    std::string out;
    std::vector<u64> ids;
    ids.reserve(impl_->pools_.size());
    for (const auto& [id, entry] : impl_->pools_) {
        BF_UNUSED(entry);
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (const u64 raw_id : ids) {
        const auto it = impl_->pools_.find(raw_id);
        if (it == impl_->pools_.end()) continue;
        out.append(format_pool_accounting(impl_->accounting_of(it->second)));
        out.push_back('\n');
    }
    const AccountingReport report = impl_->deep_verify();
    out.append("closure=");
    out.append(report.closed ? "closed" : "VIOLATED");
    out.append(" pools=");
    out.append(std::to_string(report.pools_checked));
    out.push_back('\n');
    return out;
}

Status BufferFabric::checkpoint() {
    BufferFabric::Impl::Lock guard(*impl_);
    if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    // A durable image is only ever written from a state that closes. A
    // violation here means the in-memory ledger is already inconsistent, and
    // persisting it would make the inconsistency durable.
    const AccountingReport report = impl_->deep_verify();
    if (!report.closed) return report.status();
    return impl_->write_full_snapshot();
}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

std::string Capabilities::to_json() const {
    JsonWriter writer(1024);
    writer.begin_object();
    writer.key("durability");
    writer.value_bool(durability);
    writer.key("multiprocess_transport");
    writer.value_bool(multiprocess_transport);
    writer.key("address_sanitizer");
    writer.value_bool(address_sanitizer);
    writer.key("physical_device_backend");
    writer.value_bool(physical_device_backend);
    writer.key("distributed_consensus");
    writer.value_bool(distributed_consensus);
    writer.key("build_type");
    writer.value_string(build_type);
    writer.key("compiler");
    writer.value_string(compiler);
    writer.key("version");
    writer.value_string(build_id());
    writer.end_object();
    return writer.str();
}

Capabilities capabilities() {
    Capabilities caps;
#if defined(__SANITIZE_ADDRESS__)
    caps.address_sanitizer = true;
#endif
#if defined(_MSC_VER)
    caps.compiler = "msvc";
#elif defined(__clang__)
    caps.compiler = "clang";
#elif defined(__GNUC__)
    caps.compiler = "gcc";
#else
    caps.compiler = "unknown";
#endif
#if defined(NDEBUG)
    caps.build_type = "release";
#else
    caps.build_type = "debug";
#endif
    return caps;
}

}  // namespace buffer_fabric
