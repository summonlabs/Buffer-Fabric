#include "fabric_impl.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

#include "buffer_fabric/hash.hpp"

namespace buffer_fabric {
namespace {

[[nodiscard]] u64 mix64(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

[[nodiscard]] std::atomic<u64>& boot_counter() noexcept {
    static std::atomic<u64> counter{0};
    return counter;
}

[[nodiscard]] u64 saturating_add(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (add_overflow(a, b, &out)) return (std::numeric_limits<u64>::max)();
    return out;
}

}  // namespace

BootIncarnation BufferFabric::Impl::mint_boot() noexcept {
    const u64 sequence = boot_counter().fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 ticks = clock().now_ticks();
    const u64 wall = clock().wall_seconds();
    const u64 entropy = static_cast<u64>(reinterpret_cast<std::uintptr_t>(this));
    BootIncarnation boot;
    boot.hi = mix64(ticks ^ mix64(wall) ^ mix64(entropy));
    boot.lo = mix64(sequence ^ mix64(ticks + 0x9E3779B97F4A7C15ull) ^ entropy);
    if (boot.hi == 0 && boot.lo == 0) boot.lo = 1;
    return boot;
}

std::vector<u64> BufferFabric::Impl::sorted_members(const PoolEntry& entry) const {
    std::vector<u64> ids(entry.members.begin(), entry.members.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

void BufferFabric::Impl::adjust_queue(PoolEntry& entry, QueueId queue, i64 delta, bool reserved) {
    auto& bucket = reserved ? entry.queue_reserved : entry.queue_committed;
    const auto it = bucket.find(queue.raw());
    if (delta >= 0) {
        const u64 amount = static_cast<u64>(delta);
        if (it == bucket.end()) {
            bucket.emplace(queue.raw(), amount);
        } else {
            it->second += amount;
        }
        return;
    }
    const u64 amount = static_cast<u64>(-delta);
    if (it == bucket.end()) return;
    if (it->second <= amount) {
        bucket.erase(it);
    } else {
        it->second -= amount;
    }
}

// ---------------------------------------------------------------------------
// Capacity authority
// ---------------------------------------------------------------------------

Result<Generation> BufferFabric::set_capacity(ResourceId resource, BackendId backend_id,
                                              u64 expected_raw_units, Generation expected) {
    if (!resource.valid()) {
        return Status(ErrorCode::InvalidArgument, "resource id must be non-zero");
    }
    if (!backend_id.valid()) {
        return Status(ErrorCode::InvalidArgument, "backend id must be non-zero");
    }
    IBackend* backend = nullptr;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        const auto it = impl_->backends_.find(backend_id.raw());
        if (it == impl_->backends_.end()) {
            return Status(ErrorCode::UnknownBackend, "backend is not attached");
        }
        backend = it->second.backend;
    }
    if (backend == nullptr) {
        return Status(ErrorCode::UnknownBackend, "backend is not attached");
    }
    // The backend is queried with no fabric lock held so that a slow or
    // blocking backend can never stall unrelated fabric operations.
    if (!backend->ready()) {
        return Status(ErrorCode::BackendNotReady, "backend is not ready; capacity is UNKNOWN");
    }
    Result<CapacityAuthority> authority = backend->query_capacity(resource);
    if (!authority.ok()) {
        return authority.status();
    }
    if (!authority.value().usable()) {
        return Status(ErrorCode::EvidenceUnknown,
                      "backend did not supply an authoritative capacity for this resource");
    }

    // Optional explicit physical programming. This is the only place the
    // runtime can touch device memory, it happens with no fabric lock held,
    // and the attempt is recorded rather than assumed to have worked.
    bool program_effects = false;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        program_effects = impl_->config_.program_backend_effects;
        if (program_effects) impl_->metrics_.backend_effects_attempted += 1;
    }
    if (program_effects) {
        const VoidResult effect = backend->program_reservation(
            resource, authority.value().raw_units, authority.value().generation);
        if (!effect.ok()) {
            if (effect.code() == ErrorCode::Unsupported) {
                // The backend owns no programmable memory. That is an honest
                // answer, not a failure: no hardware effect was applied.
                BufferFabric::Impl::Lock guard(*impl_);
                impl_->metrics_.backend_effects_unsupported += 1;
            } else {
                return effect.status();
            }
        }
    }

    std::vector<FabricEvent> events;
    Status status;
    Generation generation{};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        if (expected_raw_units != 0 && authority.value().raw_units != expected_raw_units) {
            return Status(ErrorCode::StaleGeneration,
                          "capacity authority disagrees with the asserted raw capacity");
        }
        if (expected.valid() && authority.value().generation != expected) {
            return Status(ErrorCode::StaleGeneration,
                          "capacity authority generation does not match the assertion");
        }
        u64 committed_by_pools = 0;
        for (const auto& [id, entry] : impl_->pools_) {
            BF_UNUSED(id);
            if (entry.descriptor.resource != resource) continue;
            u64 next = 0;
            if (add_overflow(committed_by_pools, entry.descriptor.raw_units, &next)) {
                return Status(ErrorCode::Overflow, "resource capacity total overflowed");
            }
            committed_by_pools = next;
        }
        if (committed_by_pools > authority.value().raw_units) {
            return Status(ErrorCode::CapacityExceeded,
                          "authoritative capacity is below the capacity already partitioned into "
                          "pools; shrink or remove those pools first");
        }
        CapacityEntry entry;
        entry.resource = resource;
        entry.backend = backend_id;
        entry.generation = authority.value().generation;
        entry.raw_units = authority.value().raw_units;
        entry.observed_at = impl_->now();
        status = impl_->journal_capacity(entry);
        if (!status.ok()) return status;
        impl_->capacities_[resource.raw()] = entry;
        generation = entry.generation;
        impl_->push_event(events, EventKind::PressureUpdated, ErrorCode::Ok,
                          "capacity authority published");
    }
    impl_->publish(events);
    return generation;
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

Result<Generation> BufferFabric::publish_policy(const PoolPolicy& request) {
    std::vector<FabricEvent> events;
    Status status;
    Generation published{};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        PoolPolicy policy = request;
        if (!policy.id.valid()) {
            policy.id = PolicyId::from_raw(impl_->next_policy_id_);
        }
        if (policy.id.raw() >= impl_->next_policy_id_) {
            u64 next = 0;
            if (add_overflow(policy.id.raw(), 1, &next)) {
                return Status(ErrorCode::Overflow, "policy id space is exhausted");
            }
            impl_->next_policy_id_ = next;
        }
        if (!policy.name.empty()) {
            const auto named = impl_->policy_names_.find(policy.name);
            if (named != impl_->policy_names_.end() && named->second != policy.id.raw()) {
                return Status(ErrorCode::DuplicateName, "another policy already uses this name");
            }
        }
        const auto existing = impl_->policies_.find(policy.id.raw());
        if (existing != impl_->policies_.end()) {
            PoolPolicy candidate = policy;
            candidate.generation = existing->second.generation;
            candidate.id = existing->second.id;
            if (candidate.digest() == existing->second.digest()) {
                // Re-publishing an identical policy is a no-op: the generation
                // does not advance and dependent allocations stay valid.
                return existing->second.generation;
            }
            u64 next = 0;
            if (add_overflow(existing->second.generation.raw(), 1, &next) || next == 0) {
                return Status(ErrorCode::Overflow, "policy generation is exhausted");
            }
            policy.generation = Generation::from_raw(next);
        } else {
            policy.generation = Generation::from_raw(1);
            if (impl_->policies_.size() >= kMaxPolicyCount) {
                return Status(ErrorCode::BoundedResourceExhausted,
                              "the policy table has reached its configured bound");
            }
        }
        status = policy.validate();
        if (!status.ok()) return status;
        status = impl_->journal_policy(policy);
        if (!status.ok()) return status;
        impl_->policy_names_[policy.name] = policy.id.raw();
        const Generation generation = policy.generation;
        published = generation;
        impl_->policies_[policy.id.raw()] = policy;
        impl_->push_event(events, EventKind::PolicyPublished, ErrorCode::Ok, policy.name);

        // A policy advance invalidates every allocation that was granted under
        // the previous revision.
        if (policy.fence_on_generation_change) {
            for (auto& [id, entry] : impl_->pools_) {
                BF_UNUSED(id);
                if (entry.descriptor.policy != policy.id) continue;
                const std::vector<u64> members = impl_->sorted_members(entry);
                for (const u64 raw_id : members) {
                    auto record_it = impl_->allocations_.find(raw_id);
                    if (record_it == impl_->allocations_.end()) continue;
                    AllocationRecord& record = record_it->second;
                    if (!allocation_holds_units(record.state)) continue;
                    if (record.bound.policy_generation == generation) continue;
                    status = impl_->fence_allocation_locked(record, AllocationState::Fenced,
                                                            ErrorCode::StaleGeneration, events);
                    if (!status.ok()) return status;
                }
            }
        }
    }
    impl_->publish(events);
    return published;
}

Result<PoolPolicy> BufferFabric::policy(PolicyId id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    if (!id.valid()) return Status(ErrorCode::InvalidArgument, "policy id must be non-zero");
    const auto it = impl_->policies_.find(id.raw());
    if (it == impl_->policies_.end()) return Status(ErrorCode::UnknownPolicy, "no such policy");
    return it->second;
}

// ---------------------------------------------------------------------------
// Pool topology
// ---------------------------------------------------------------------------

Result<PoolId> BufferFabric::register_pool(const PoolRegistration& registration) {
    std::vector<FabricEvent> events;
    Status status;
    PoolId created{};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        if (impl_->pools_.size() >= kMaxPoolCount) {
            return Status(ErrorCode::BoundedResourceExhausted,
                          "the pool table has reached its configured bound");
        }
        PoolDescriptor descriptor;
        descriptor.id = registration.id;
        descriptor.name = registration.name;
        descriptor.resource = registration.resource;
        descriptor.backend = registration.backend;
        descriptor.raw_units = registration.raw_units;
        descriptor.protected_units = registration.protected_units;
        descriptor.parent = registration.parent;
        descriptor.policy = registration.policy;
        descriptor.tenant = registration.tenant;
        descriptor.provenance = registration.provenance;
        descriptor.created_at = impl_->now();
        descriptor.generation = Generation::from_raw(1);

        if (!descriptor.id.valid()) {
            descriptor.id = PoolId::from_raw(impl_->next_pool_id_);
        }
        if (descriptor.id.raw() >= impl_->next_pool_id_) {
            u64 next = 0;
            if (add_overflow(descriptor.id.raw(), 1, &next)) {
                return Status(ErrorCode::Overflow, "pool id space is exhausted");
            }
            impl_->next_pool_id_ = next;
        }
        status = descriptor.validate();
        if (!status.ok()) return status;
        if (impl_->pools_.find(descriptor.id.raw()) != impl_->pools_.end()) {
            return Status(ErrorCode::DuplicatePool, "pool id is already registered");
        }
        const auto named = impl_->pool_names_.find(descriptor.name);
        if (named != impl_->pool_names_.end()) {
            return Status(ErrorCode::DuplicateName, "another pool already uses this name");
        }
        const auto backend_it = impl_->backends_.find(descriptor.backend.raw());
        if (backend_it == impl_->backends_.end()) {
            return Status(ErrorCode::UnknownBackend, "pool backend is not attached");
        }
        if (!impl_->policies_.count(descriptor.policy.raw())) {
            return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
        }
        if (descriptor.parent.valid()) {
            PoolEntry* parent = impl_->pool(descriptor.parent.raw());
            if (parent == nullptr) {
                return Status(ErrorCode::UnknownPool, "pool parent is not registered");
            }
            u64 depth = 1;
            PoolEntry* cursor = parent;
            while (cursor != nullptr && cursor->descriptor.parent.valid()) {
                ++depth;
                if (depth > kMaxPoolDepth) {
                    return Status(ErrorCode::PoolDepthExceeded, "pool tree is deeper than the bound");
                }
                cursor = impl_->pool(cursor->descriptor.parent.raw());
            }
            if (cursor == nullptr) {
                return Status(ErrorCode::UnknownPool, "pool ancestry is broken");
            }
        }

        const auto capacity_it = impl_->capacities_.find(descriptor.resource.raw());
        if (capacity_it == impl_->capacities_.end()) {
            return Status(ErrorCode::UnknownResource,
                          "no authoritative capacity has been published for this resource");
        }
        u64 partitioned = 0;
        for (const auto& [id, entry] : impl_->pools_) {
            BF_UNUSED(id);
            if (entry.descriptor.resource != descriptor.resource) continue;
            u64 next = 0;
            if (add_overflow(partitioned, entry.descriptor.raw_units, &next)) {
                return Status(ErrorCode::Overflow, "resource partition total overflowed");
            }
            partitioned = next;
        }
        u64 total = 0;
        if (add_overflow(partitioned, descriptor.raw_units, &total)) {
            return Status(ErrorCode::Overflow, "resource partition total overflowed");
        }
        if (total > capacity_it->second.raw_units) {
            return Status(ErrorCode::CapacityExceeded,
                          "pool raw capacity exceeds the authoritative capacity remaining on the "
                          "resource");
        }

        PoolEntry entry;
        entry.descriptor = descriptor;
        entry.capacity_generation = Generation::from_raw(1);
        status = impl_->journal_pool(entry);
        if (!status.ok()) return status;
        impl_->pool_names_[descriptor.name] = descriptor.id.raw();
        created = descriptor.id;
        impl_->pools_[descriptor.id.raw()] = std::move(entry);
        impl_->push_event(events, EventKind::PoolRegistered, ErrorCode::Ok, descriptor.name,
                          descriptor.id);
    }
    impl_->publish(events);
    return created;
}

Status BufferFabric::remove_pool(PoolId pool_id, Generation expected_pool_generation) {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        PoolEntry* entry = nullptr;
        status = impl_->require_pool(pool_id, &entry);
        if (!status.ok()) return status;
        if (expected_pool_generation.valid() &&
            entry->descriptor.generation != expected_pool_generation) {
            return Status(ErrorCode::StaleGeneration, "pool generation does not match the assertion");
        }
        for (const auto& [id, other] : impl_->pools_) {
            BF_UNUSED(id);
            if (other.descriptor.parent == pool_id) {
                return Status(ErrorCode::LifecycleViolation,
                              "pool still has child pools; remove them first");
            }
        }
        for (const auto& [id, queue] : impl_->queues_) {
            BF_UNUSED(id);
            if (queue.pool != pool_id) continue;
            if (!queue.retired) {
                return Status(ErrorCode::LifecycleViolation,
                              "pool still has live bound queues; retire them first");
            }
        }
        if (entry->lent_out != 0) {
            return Status(ErrorCode::LifecycleViolation,
                          "pool capacity is lent to descendants; resolve borrowing first");
        }
        // Impossible in practice, but the check keeps the identity honest.
        if (entry->allocated() != 0) {
            return Status(ErrorCode::LifecycleViolation,
                          "pool still holds allocations; release or fence them first");
        }
        const std::string name = entry->descriptor.name;
        ByteWriter writer(16);
        writer.put_u64(pool_id.raw());
        status = impl_->journal(JournalOp::PoolRemoved, writer.data().data(), writer.size());
        if (!status.ok()) return status;
        impl_->pool_names_.erase(name);
        impl_->pools_.erase(pool_id.raw());
        impl_->pressure_.erase(pool_id.raw());
        // Retired queues carry no authority; they are removed with their pool
        // so that the topology does not accumulate unreachable records.
        for (auto it = impl_->queues_.begin(); it != impl_->queues_.end();) {
            if (it->second.pool == pool_id && it->second.retired) {
                impl_->queue_names_.erase(it->second.name);
                it = impl_->queues_.erase(it);
            } else {
                ++it;
            }
        }
        impl_->push_event(events, EventKind::PoolRemoved, ErrorCode::Ok, name, pool_id);
    }
    impl_->publish(events);
    return Status::success();
}

Status BufferFabric::Impl::enforce_capacity(PoolEntry& entry, bool fence_everything, ShrinkResult* result,
                              std::vector<FabricEvent>& events) {
    ShrinkResult local;
    const auto policy_it = policies_.find(entry.descriptor.policy.raw());
    const u64 overcommit_limit =
        policy_it != policies_.end() && policy_it->second.overcommit.mode == OvercommitMode::Bounded
            ? policy_it->second.overcommit.limit_units
            : 0;
    const u64 allowance = saturating_add(entry.usable(), overcommit_limit);

    // Victims are chosen in a total order so the outcome is reproducible:
    // reclaimable allocations first, then by id.
    std::vector<u64> victims = sorted_members(entry);
    std::sort(victims.begin(), victims.end(), [this](u64 a, u64 b) {
        const auto ita = allocations_.find(a);
        const auto itb = allocations_.find(b);
        if (ita == allocations_.end() || itb == allocations_.end()) return a < b;
        const AllocationRecord& ra = ita->second;
        const AllocationRecord& rb = itb->second;
        if (ra.reclaim_class != rb.reclaim_class) {
            return ra.reclaim_class == ReclaimClass::Reclaimable;
        }
        if (ra.reclaim_priority != rb.reclaim_priority) {
            return ra.reclaim_priority < rb.reclaim_priority;
        }
        return ra.id.raw() < rb.id.raw();
    });

    for (const u64 raw_id : victims) {
        if (!fence_everything && entry.own_usage <= allowance) break;
        auto it = allocations_.find(raw_id);
        if (it == allocations_.end()) continue;
        AllocationRecord& record = it->second;
        if (!allocation_holds_units(record.state)) continue;
        const u64 units = record.units;
        Status status = fence_allocation_locked(record, AllocationState::Fenced,
                                               ErrorCode::CapacityExceeded, events);
        if (!status.ok()) return status;
        local.fenced_allocations += 1;
        local.fenced_units += units;
        if (fence_everything) {
            local.reduced_units += units;
        } else {
            local.reduced_allocations += 1;
            local.reduced_units += units;
        }
    }

    // Descendants may still be drawing on this pool's capacity through a
    // lending. Those commitments are fenced next, again in a deterministic
    // order, because the pool no longer has the capacity to back them.
    if (entry.own_usage > allowance) {
        std::vector<u64> candidates;
        for (const auto& [id, record] : allocations_) {
            if (!allocation_holds_units(record.state)) continue;
            if (record.lender != entry.descriptor.id) continue;
            if (record.borrow_units == 0) continue;
            candidates.push_back(id);
        }
        std::sort(candidates.begin(), candidates.end());
        for (const u64 raw_id : candidates) {
            if (entry.own_usage <= allowance) break;
            auto it = allocations_.find(raw_id);
            if (it == allocations_.end()) continue;
            AllocationRecord& record = it->second;
            if (!allocation_holds_units(record.state)) continue;
            const u64 borrowed = record.borrow_units;
            Status status = fence_allocation_locked(record, AllocationState::Fenced,
                                                    ErrorCode::CapacityExceeded, events);
            if (!status.ok()) return status;
            local.fenced_allocations += 1;
            local.fenced_units += record.units;
            local.borrow_returned_units += borrowed;
        }
    }
    if (result != nullptr) *result = local;
    return Status::success();
}

Result<ResizeOutcome> BufferFabric::resize_pool(PoolId pool_id, u64 new_raw_units,
                                                u64 new_protected_units,
                                                Generation expected_pool_generation) {
    std::vector<FabricEvent> events;
    Status status;
    ResizeOutcome outcome;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        PoolEntry* entry = nullptr;
        status = impl_->require_pool(pool_id, &entry);
        if (!status.ok()) return status;
        if (expected_pool_generation.valid() &&
            entry->descriptor.generation != expected_pool_generation) {
            return Status(ErrorCode::StaleGeneration, "pool generation does not match the assertion");
        }
        if (new_raw_units == 0 || new_raw_units > kMaxUnitsPerPool) {
            return Status(ErrorCode::InvalidArgument, "pool raw capacity is out of range");
        }
        if (new_protected_units >= new_raw_units) {
            return Status(ErrorCode::ProtectedHeadroomViolation,
                          "protected headroom must leave allocatable capacity");
        }
        const u64 new_usable = new_raw_units - new_protected_units;
        const auto policy_it = impl_->policies_.find(entry->descriptor.policy.raw());
        if (policy_it == impl_->policies_.end()) {
            return Status(ErrorCode::UnknownPolicy, "pool policy is not published");
        }
        if (new_usable < policy_it->second.usable_floor_units) {
            return Status(ErrorCode::PolicyForbids,
                          "resize would push usable capacity below the policy floor");
        }
        const auto capacity_it = impl_->capacities_.find(entry->descriptor.resource.raw());
        if (capacity_it == impl_->capacities_.end()) {
            return Status(ErrorCode::UnknownResource,
                          "no authoritative capacity has been published for this resource");
        }
        u64 others = 0;
        for (const auto& [id, other] : impl_->pools_) {
            BF_UNUSED(id);
            if (other.descriptor.resource != entry->descriptor.resource) continue;
            if (other.descriptor.id == pool_id) continue;
            u64 next = 0;
            if (add_overflow(others, other.descriptor.raw_units, &next)) {
                return Status(ErrorCode::Overflow, "resource partition total overflowed");
            }
            others = next;
        }
        u64 total = 0;
        if (add_overflow(others, new_raw_units, &total)) {
            return Status(ErrorCode::Overflow, "resource partition total overflowed");
        }
        if (total > capacity_it->second.raw_units) {
            return Status(ErrorCode::CapacityExceeded,
                          "resize exceeds the authoritative capacity of the resource");
        }

        outcome.previous_raw_units = entry->descriptor.raw_units;
        outcome.new_raw_units = new_raw_units;
        outcome.previous_protected_units = entry->descriptor.protected_units;
        outcome.new_protected_units = new_protected_units;
        outcome.previous_generation = entry->descriptor.generation;

        u64 next_generation = 0;
        if (add_overflow(entry->descriptor.generation.raw(), 1, &next_generation) ||
            next_generation == 0) {
            return Status(ErrorCode::Overflow, "pool generation is exhausted");
        }
        u64 next_capacity = 0;
        if (add_overflow(entry->capacity_generation.raw(), 1, &next_capacity) ||
            next_capacity == 0) {
            return Status(ErrorCode::Overflow, "pool capacity generation is exhausted");
        }
        entry->descriptor.raw_units = new_raw_units;
        entry->descriptor.protected_units = new_protected_units;
        entry->descriptor.generation = Generation::from_raw(next_generation);
        entry->capacity_generation = Generation::from_raw(next_capacity);
        outcome.new_generation = entry->descriptor.generation;

        status = impl_->journal_pool(*entry);
        if (!status.ok()) return status;
        impl_->push_event(events, EventKind::PoolResized, ErrorCode::Ok, entry->descriptor.name,
                          pool_id, QueueId{}, AllocationId{}, new_raw_units);

        BufferFabric::Impl::ShrinkResult shrink;
        status = impl_->enforce_capacity(*entry, policy_it->second.fence_on_generation_change,
                                         &shrink, events);
        if (!status.ok()) return status;
        outcome.fenced_allocations = shrink.fenced_allocations;
        outcome.fenced_units = shrink.fenced_units;
        outcome.reduced_allocations = shrink.reduced_allocations;
        outcome.reduced_units = shrink.reduced_units;
        outcome.borrow_returned_units = shrink.borrow_returned_units;

        status = impl_->check_closure(*entry);
        if (!status.ok()) {
            impl_->shutting_down_ = true;
            impl_->push_event(events, EventKind::AccountingViolation, status.code(),
                              status.message(), pool_id);
            return status;
        }
        outcome.accounting = impl_->accounting_of(*entry);
    }
    impl_->publish(events);
    return outcome;
}

// ---------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------

Result<QueueId> BufferFabric::register_queue(const QueueRegistration& registration) {
    std::vector<FabricEvent> events;
    Status status;
    QueueId created{};
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        if (impl_->queues_.size() >= kMaxQueueCount) {
            return Status(ErrorCode::BoundedResourceExhausted,
                          "the queue table has reached its configured bound");
        }
        QueueDescriptor descriptor;
        descriptor.id = registration.id;
        descriptor.name = registration.name;
        descriptor.pool = registration.pool;
        descriptor.tenant = registration.tenant;
        descriptor.traffic_class = registration.traffic_class;
        descriptor.demand_units = registration.demand_units;
        descriptor.weight = registration.weight;
        descriptor.max_committed_units = registration.max_committed_units;
        descriptor.registered_at = impl_->now();
        descriptor.demand_updated_at = descriptor.registered_at;
        descriptor.generation = Generation::from_raw(1);

        if (!descriptor.id.valid()) {
            descriptor.id = QueueId::from_raw(impl_->next_queue_id_);
        }
        if (descriptor.id.raw() >= impl_->next_queue_id_) {
            u64 next = 0;
            if (add_overflow(descriptor.id.raw(), 1, &next)) {
                return Status(ErrorCode::Overflow, "queue id space is exhausted");
            }
            impl_->next_queue_id_ = next;
        }
        if (!is_valid_name(descriptor.name)) {
            return Status(ErrorCode::NameTooLong,
                          "queue name must be 1..64 characters of [A-Za-z0-9_.-]");
        }
        if (!descriptor.pool.valid()) {
            return Status(ErrorCode::InvalidArgument, "queue pool must be set");
        }
        if (descriptor.demand_units > kMaxUnitsPerPool ||
            descriptor.max_committed_units > kMaxUnitsPerPool) {
            return Status(ErrorCode::Overflow, "queue demand exceeds the supported bound");
        }
        if (impl_->queues_.find(descriptor.id.raw()) != impl_->queues_.end()) {
            return Status(ErrorCode::DuplicateQueue, "queue id is already registered");
        }
        const auto named = impl_->queue_names_.find(descriptor.name);
        if (named != impl_->queue_names_.end()) {
            return Status(ErrorCode::DuplicateName, "another queue already uses this name");
        }
        if (impl_->pool(descriptor.pool.raw()) == nullptr) {
            return Status(ErrorCode::UnknownPool, "queue pool is not registered");
        }
        status = impl_->journal_queue(descriptor);
        if (!status.ok()) return status;
        impl_->queue_names_[descriptor.name] = descriptor.id.raw();
        created = descriptor.id;
        impl_->queues_[descriptor.id.raw()] = std::move(descriptor);
        impl_->push_event(events, EventKind::QueueRegistered, ErrorCode::Ok, registration.name,
                          registration.pool, created);
    }
    impl_->publish(events);
    return created;
}

Result<Generation> BufferFabric::update_queue_demand(QueueId queue_id, u64 demand_units,
                                                     Generation expected_queue_generation) {
    BufferFabric::Impl::Lock guard(*impl_);
    if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    QueueDescriptor* queue = nullptr;
    BF_TRY(impl_->require_queue(queue_id, &queue));
    if (expected_queue_generation.valid() &&
        queue->generation != expected_queue_generation) {
        return Status(ErrorCode::StaleGeneration, "queue generation does not match the assertion");
    }
    if (queue->retired) {
        return Status(ErrorCode::WrongState, "queue is retired");
    }
    if (demand_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "queue demand exceeds the supported bound");
    }
    // Demand is an authoritative input supplied by the queue's owner; it is not
    // a structural change and therefore does not advance the queue generation.
    queue->demand_units = demand_units;
    queue->demand_updated_at = impl_->now();
    BF_TRY(impl_->journal_queue(*queue));
    return queue->generation;
}

Status BufferFabric::retire_queue(QueueId queue_id, Generation expected_queue_generation) {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        QueueDescriptor* queue = nullptr;
        status = impl_->require_queue(queue_id, &queue);
        if (!status.ok()) return status;
        if (expected_queue_generation.valid() &&
            queue->generation != expected_queue_generation) {
            return Status(ErrorCode::StaleGeneration, "queue generation does not match the assertion");
        }
        if (queue->retired) return Status::success();
        u64 next = 0;
        if (add_overflow(queue->generation.raw(), 1, &next) || next == 0) {
            return Status(ErrorCode::Overflow, "queue generation is exhausted");
        }
        queue->retired = true;
        queue->generation = Generation::from_raw(next);
        status = impl_->journal_queue(*queue);
        if (!status.ok()) return status;

        if (impl_->config_.auto_fence_on_queue_retirement) {
            std::vector<u64> victims;
            for (const auto& [id, record] : impl_->allocations_) {
                if (record.queue != queue_id) continue;
                if (!allocation_holds_units(record.state)) continue;
                victims.push_back(id);
            }
            std::sort(victims.begin(), victims.end());
            for (const u64 raw_id : victims) {
                auto it = impl_->allocations_.find(raw_id);
                if (it == impl_->allocations_.end()) continue;
                AllocationRecord& record = it->second;
                if (!allocation_holds_units(record.state)) continue;
                status = impl_->fence_allocation_locked(record, AllocationState::Fenced,
                                                        ErrorCode::StaleGeneration, events);
                if (!status.ok()) return status;
            }
        }
        impl_->push_event(events, EventKind::QueueRetired, ErrorCode::Ok, queue->name,
                          queue->pool, queue_id);
    }
    impl_->publish(events);
    return Status::success();
}

Status BufferFabric::retarget_queue(QueueId queue_id, PoolId new_pool,
                                    Generation expected_queue_generation) {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        QueueDescriptor* queue = nullptr;
        status = impl_->require_queue(queue_id, &queue);
        if (!status.ok()) return status;
        if (expected_queue_generation.valid() &&
            queue->generation != expected_queue_generation) {
            return Status(ErrorCode::StaleGeneration, "queue generation does not match the assertion");
        }
        if (queue->retired) {
            return Status(ErrorCode::WrongState, "queue is retired");
        }
        if (impl_->pool(new_pool.raw()) == nullptr) {
            return Status(ErrorCode::UnknownPool, "target pool is not registered");
        }
        if (queue->pool == new_pool) return Status::success();
        u64 next = 0;
        if (add_overflow(queue->generation.raw(), 1, &next) || next == 0) {
            return Status(ErrorCode::Overflow, "queue generation is exhausted");
        }
        // Retargeting is a structural change: every allocation granted under
        // the previous binding is invalidated before the new binding is
        // published.
        std::vector<u64> victims;
        for (const auto& [id, record] : impl_->allocations_) {
            if (record.queue != queue_id) continue;
            if (!allocation_holds_units(record.state)) continue;
            victims.push_back(id);
        }
        std::sort(victims.begin(), victims.end());
        for (const u64 raw_id : victims) {
            auto it = impl_->allocations_.find(raw_id);
            if (it == impl_->allocations_.end()) continue;
            AllocationRecord& record = it->second;
            if (!allocation_holds_units(record.state)) continue;
            status = impl_->fence_allocation_locked(record, AllocationState::Fenced,
                                                    ErrorCode::StaleGeneration, events);
            if (!status.ok()) return status;
        }
        queue->pool = new_pool;
        queue->generation = Generation::from_raw(next);
        status = impl_->journal_queue(*queue);
        if (!status.ok()) return status;
    }
    impl_->publish(events);
    return Status::success();
}

Result<PoolGenerationStatus> BufferFabric::pool_generation_status(PoolId pool_id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    PoolGenerationStatus status;
    const auto it = impl_->pools_.find(pool_id.raw());
    if (it == impl_->pools_.end()) {
        return Status(ErrorCode::UnknownPool, "no such pool");
    }
    const PoolEntry& entry = it->second;
    status.pool = entry.descriptor.id;
    status.pool_generation = entry.descriptor.generation;
    status.capacity_generation = entry.capacity_generation;
    const auto policy_it = impl_->policies_.find(entry.descriptor.policy.raw());
    status.policy_bound = policy_it != impl_->policies_.end();
    if (status.policy_bound) {
        status.policy_generation = policy_it->second.generation;
    }
    for (const u64 raw_id : entry.members) {
        const auto record_it = impl_->allocations_.find(raw_id);
        if (record_it == impl_->allocations_.end()) continue;
        const AllocationRecord& record = record_it->second;
        if (!allocation_holds_units(record.state)) continue;
        status.allocations_bound += 1;
        const bool stale = record.bound.pool_generation != entry.descriptor.generation ||
                           record.bound.capacity_generation != entry.capacity_generation ||
                           (status.policy_bound &&
                            record.bound.policy_generation != status.policy_generation) ||
                           record.bound.epoch != impl_->epoch_;
        if (stale) status.allocations_stale += 1;
    }
    return status;
}

// ---------------------------------------------------------------------------
// Pressure evidence
// ---------------------------------------------------------------------------

Status BufferFabric::observe_pressure(const PressureSnapshot& request) {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status(ErrorCode::LifecycleViolation, "fabric is not open");
        if (impl_->shutting_down_ && impl_->config_.refuse_work_after_shutdown) {
            return Status(ErrorCode::ShuttingDown, "fabric is shutting down");
        }
        PoolEntry* entry = nullptr;
        status = impl_->require_pool(request.pool, &entry);
        if (!status.ok()) return status;
        PressureSnapshot snapshot = request;
        if (!snapshot.id.valid()) {
            snapshot.id = SnapshotId::from_raw(impl_->next_snapshot_id_);
        }
        if (snapshot.id.raw() >= impl_->next_snapshot_id_) {
            u64 next = 0;
            if (add_overflow(snapshot.id.raw(), 1, &next)) {
                return Status(ErrorCode::Overflow, "snapshot id space is exhausted");
            }
            impl_->next_snapshot_id_ = next;
        }
        if (snapshot.epoch != impl_->epoch_) {
            impl_->metrics_.pressure_snapshots_rejected += 1;
            impl_->push_event(events, EventKind::PressureRejected, ErrorCode::StaleEpoch,
                              "pressure snapshot carries a stale fabric epoch", request.pool);
            impl_->publish(events);
            return Status(ErrorCode::StaleEpoch,
                          "pressure snapshot epoch does not match the current fabric epoch");
        }
        if (snapshot.pool_generation != entry->descriptor.generation) {
            impl_->metrics_.pressure_snapshots_rejected += 1;
            impl_->push_event(events, EventKind::PressureRejected, ErrorCode::StaleGeneration,
                              "pressure snapshot carries a stale pool generation", request.pool);
            impl_->publish(events);
            return Status(ErrorCode::StaleGeneration,
                          "pressure snapshot pool generation is not current");
        }
        if (snapshot.observed_at == 0) {
            snapshot.observed_at = impl_->now();
        }
        if (snapshot.observed_at > impl_->now()) {
            impl_->metrics_.pressure_snapshots_rejected += 1;
            impl_->push_event(events, EventKind::PressureRejected, ErrorCode::InvalidArgument,
                              "pressure snapshot is future dated", request.pool);
            impl_->publish(events);
            return Status(ErrorCode::InvalidArgument,
                          "pressure snapshot observation time is in the future");
        }
        if (snapshot.demand_units > kMaxUnitsPerPool ||
            snapshot.committed_units > (kMaxUnitsPerPool * 2) ||
            snapshot.reclaimable_units > kMaxUnitsPerPool) {
            return Status(ErrorCode::Overflow, "pressure snapshot values exceed the supported bound");
        }
        if (snapshot.ttl_ticks == 0) {
            const auto policy_it = impl_->policies_.find(entry->descriptor.policy.raw());
            if (policy_it != impl_->policies_.end()) {
                snapshot.ttl_ticks = policy_it->second.pressure.evidence_ttl_ticks;
            }
        }
        if (snapshot.boot != impl_->boot_) {
            snapshot.boot = impl_->boot_;
        }
        status = impl_->journal_pressure(snapshot);
        if (!status.ok()) return status;
        PressureEntry& stored = impl_->pressure_[request.pool.raw()];
        stored.latest = snapshot;
        stored.has_latest = true;
        stored.latest_revalidated = false;
        stored.history.push_back(snapshot);
        while (stored.history.size() > impl_->config_.pressure_history_capacity) {
            stored.history.pop_front();
        }
        impl_->metrics_.pressure_snapshots_accepted += 1;
        impl_->push_event(events, EventKind::PressureUpdated, ErrorCode::Ok, "pressure observed",
                          request.pool);
    }
    impl_->publish(events);
    return Status::success();
}

Result<PressureEvidence> BufferFabric::pressure(PoolId pool_id) const {
    BufferFabric::Impl::Lock guard(*impl_);
    PoolEntry* entry = nullptr;
    BF_TRY(impl_->require_pool(pool_id, &entry));
    return impl_->resolve_evidence(*entry);
}

}  // namespace buffer_fabric

