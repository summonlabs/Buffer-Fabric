#include "fabric_impl.hpp"

#include <algorithm>
#include <utility>

#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/version.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] u64 saturating_add(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (add_overflow(a, b, &out)) return (std::numeric_limits<u64>::max)();
    return out;
}

[[nodiscard]] u64 mix64(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

}  // namespace

IClock& BufferFabric::Impl::clock() const { return clock_ != nullptr ? *clock_ : default_clock_; }

PoolEntry* BufferFabric::Impl::pool(u64 raw) {
    const auto it = pools_.find(raw);
    return it == pools_.end() ? nullptr : &it->second;
}

const PoolEntry* BufferFabric::Impl::pool(u64 raw) const {
    const auto it = pools_.find(raw);
    return it == pools_.end() ? nullptr : &it->second;
}

Status BufferFabric::Impl::require_pool(PoolId id, PoolEntry** out) {
    if (out == nullptr) return Status(ErrorCode::InvalidArgument, "output pointer is null");
    *out = nullptr;
    if (!id.valid()) return Status(ErrorCode::InvalidArgument, "pool id must be non-zero");
    PoolEntry* entry = pool(id.raw());
    if (entry == nullptr) return Status(ErrorCode::UnknownPool, "no such pool");
    *out = entry;
    return Status::success();
}

Status BufferFabric::Impl::require_queue(QueueId id, QueueDescriptor** out) {
    if (out == nullptr) return Status(ErrorCode::InvalidArgument, "output pointer is null");
    *out = nullptr;
    if (!id.valid()) return Status(ErrorCode::InvalidArgument, "queue id must be non-zero");
    const auto it = queues_.find(id.raw());
    if (it == queues_.end()) return Status(ErrorCode::UnknownQueue, "no such queue");
    *out = &it->second;
    return Status::success();
}

PoolAccounting BufferFabric::Impl::accounting_of(const PoolEntry& entry) const {
    PoolAccounting accounting;
    accounting.pool = entry.descriptor.id;
    accounting.pool_generation = entry.descriptor.generation;
    accounting.raw = entry.descriptor.raw_units;
    accounting.protected_units = entry.descriptor.protected_units;
    accounting.usable = entry.usable();
    accounting.reserved = entry.reserved_units;
    accounting.committed = entry.committed_units;
    accounting.allocated = entry.allocated();
    accounting.pinned = entry.pinned_units;
    accounting.reclaimable = entry.reclaimable_units;
    accounting.borrowed_in = entry.borrowed_in;
    accounting.lent_out = entry.lent_out;
    accounting.overcommit_used = entry.overcommit_used();
    accounting.free = entry.free_units();
    const auto policy_it = policies_.find(entry.descriptor.policy.raw());
    if (policy_it != policies_.end()) {
        accounting.overcommit_limit = policy_it->second.overcommit.mode == OvercommitMode::Bounded
                                          ? policy_it->second.overcommit.limit_units
                                          : 0;
    }
    return accounting;
}

Status BufferFabric::Impl::check_closure(const PoolEntry& entry) const {
    const auto policy_it = policies_.find(entry.descriptor.policy.raw());
    const OvercommitMode mode = policy_it != policies_.end() ? policy_it->second.overcommit.mode
                                                             : OvercommitMode::Forbid;
    const u64 limit = policy_it != policies_.end() ? policy_it->second.overcommit.limit_units : 0;
    const AccountingReport report = check_pool_accounting(accounting_of(entry), mode, limit);
    return report.status();
}

AccountingReport BufferFabric::Impl::check_all_closures() const {
    std::vector<PoolAccounting> all;
    all.reserve(pools_.size());
    for (const auto& [id, entry] : pools_) {
        BF_UNUSED(id);
        all.push_back(accounting_of(entry));
    }
    return check_global_accounting(all);
}

void BufferFabric::Impl::rebuild_aggregates() {
    for (auto& [id, entry] : pools_) {
        BF_UNUSED(id);
        entry.reserved_units = 0;
        entry.committed_units = 0;
        entry.pinned_units = 0;
        entry.reclaimable_units = 0;
        entry.own_usage = 0;
        entry.borrowed_in = 0;
        entry.lent_out = 0;
        entry.queue_committed.clear();
        entry.queue_reserved.clear();
        entry.members.clear();
    }
    for (const auto& [raw_id, record] : allocations_) {
        BF_UNUSED(raw_id);
        if (!allocation_holds_units(record.state)) continue;
        PoolEntry* entry = pool(record.pool.raw());
        if (entry == nullptr) continue;
        if (record.reclaim_class == ReclaimClass::Pinned) {
            entry->pinned_units += record.units;
        } else {
            entry->reclaimable_units += record.units;
        }
        if (record.state == AllocationState::Reserved) {
            entry->reserved_units += record.units;
        } else {
            entry->committed_units += record.units;
        }
        const u64 borrowed = record.borrow_units <= record.units ? record.borrow_units : record.units;
        entry->borrowed_in += borrowed;
        entry->own_usage += record.units - borrowed;
        entry->members.insert(record.id.raw());
        auto& bucket = record.state == AllocationState::Reserved ? entry->queue_reserved
                                                                 : entry->queue_committed;
        bucket[record.queue.raw()] += record.units;
        if (borrowed != 0 && record.lender.valid()) {
            PoolEntry* lender = pool(record.lender.raw());
            if (lender != nullptr) {
                lender->lent_out += borrowed;
                lender->own_usage += borrowed;
            }
        }
    }
    for (auto& [id, entry] : pools_) {
        BF_UNUSED(id);
        entry.runtime.borrowed_in = entry.borrowed_in;
        entry.runtime.lent_out = entry.lent_out;
        entry.runtime.overcommit_used = entry.overcommit_used();
        entry.runtime.free_units = entry.free_units();
        entry.runtime.reserved_units = entry.reserved_units;
        entry.runtime.committed_units = entry.committed_units;
        entry.runtime.pinned_units = entry.pinned_units;
        entry.runtime.reclaimable_units = entry.reclaimable_units;
    }
}

AccountingReport BufferFabric::Impl::deep_verify() const {
    // Recompute independently from the allocation set.
    std::unordered_map<u64, PoolAccounting> recomputed;
    for (const auto& [id, entry] : pools_) {
        BF_UNUSED(id);
        PoolAccounting accounting;
        accounting.pool = entry.descriptor.id;
        accounting.pool_generation = entry.descriptor.generation;
        accounting.raw = entry.descriptor.raw_units;
        accounting.protected_units = entry.descriptor.protected_units;
        accounting.usable = entry.usable();
        const auto policy_it = policies_.find(entry.descriptor.policy.raw());
        if (policy_it != policies_.end() && policy_it->second.overcommit.mode == OvercommitMode::Bounded) {
            accounting.overcommit_limit = policy_it->second.overcommit.limit_units;
        }
        recomputed.emplace(entry.descriptor.id.raw(), accounting);
    }
    for (const auto& [raw_id, record] : allocations_) {
        BF_UNUSED(raw_id);
        if (!allocation_holds_units(record.state)) continue;
        const auto it = recomputed.find(record.pool.raw());
        if (it == recomputed.end()) continue;
        PoolAccounting& accounting = it->second;
        accounting.allocated += record.units;
        if (record.reclaim_class == ReclaimClass::Pinned) {
            accounting.pinned += record.units;
        } else {
            accounting.reclaimable += record.units;
        }
        if (record.state == AllocationState::Reserved) {
            accounting.reserved += record.units;
        } else {
            accounting.committed += record.units;
        }
        const u64 borrowed = record.borrow_units <= record.units ? record.borrow_units : record.units;
        accounting.borrowed_in += borrowed;
        if (borrowed != 0 && record.lender.valid()) {
            const auto lender_it = recomputed.find(record.lender.raw());
            if (lender_it != recomputed.end()) {
                lender_it->second.lent_out += borrowed;
            }
        }
    }
    for (auto& [id, accounting] : recomputed) {
        BF_UNUSED(id);
        const u64 own = saturating_add(
            accounting.allocated >= accounting.borrowed_in
                ? accounting.allocated - accounting.borrowed_in
                : 0,
            accounting.lent_out);
        const u64 usable = accounting.usable;
        accounting.overcommit_used = own > usable ? own - usable : 0;
        accounting.free = usable + accounting.overcommit_used - own;
    }

    std::vector<PoolAccounting> recomputed_vector;
    recomputed_vector.reserve(recomputed.size());
    for (const auto& [id, accounting] : recomputed) {
        BF_UNUSED(id);
        recomputed_vector.push_back(accounting);
    }
    return check_global_accounting(recomputed_vector);
}

// ---------------------------------------------------------------------------
// Attempt de-duplication
// ---------------------------------------------------------------------------

Status BufferFabric::Impl::lookup_attempt(AttemptId id, Digest digest, AttemptRecord* previous,
                            bool* replay) const {
    if (replay != nullptr) *replay = false;
    if (!id.valid()) {
        return Status(ErrorCode::InvalidArgument, "attempt id must be non-zero");
    }
    const auto it = attempts_.find(id.raw());
    if (it == attempts_.end()) return Status::success();
    if (it->second.request_digest != digest) {
        return Status(ErrorCode::AttemptConflict,
                      "attempt id was already used for a different request");
    }
    if (previous != nullptr) *previous = it->second;
    if (replay != nullptr) *replay = true;
    return Status::success();
}

void BufferFabric::Impl::record_attempt(const AttemptRecord& record) {
    if (config_.attempt_memory_capacity == 0) return;
    const auto it = attempts_.find(record.id.raw());
    if (it == attempts_.end()) {
        attempt_order_.push_back(record.id.raw());
    }
    attempts_[record.id.raw()] = record;
    evict_attempts();
}

void BufferFabric::Impl::evict_attempts() {
    while (attempts_.size() > config_.attempt_memory_capacity && !attempt_order_.empty()) {
        const u64 oldest = attempt_order_.front();
        attempt_order_.pop_front();
        attempts_.erase(oldest);
    }
}

void BufferFabric::Impl::record_decision(const Decision& decision) {
    DecisionRecord record;
    record.sequence = decision.sequence;
    record.at = decision.decided_at;
    record.kind = decision.kind;
    record.status = decision.status;
    record.pool = decision.pool;
    record.queue = decision.queue;
    record.allocation = decision.allocation;
    record.requested_units = decision.requested_units;
    record.granted_units = decision.granted_units;
    record.binding = decision.binding;
    record.pressure = decision.pressure;
    record.authority_digest = decision.authority.digest();
    record.reason = decision.reason;
    decisions_.push_back(std::move(record));
    while (decisions_.size() > config_.decision_history_capacity) {
        decisions_.pop_front();
    }
}

void BufferFabric::Impl::push_event(std::vector<FabricEvent>& events, EventKind kind, ErrorCode outcome,
                      std::string detail, PoolId pool_id, QueueId queue_id,
                      AllocationId allocation, u64 units, u64 sequence) const {
    if (events.size() >= 64) return;
    FabricEvent event;
    event.kind = kind;
    event.at = now();
    event.epoch = epoch_;
    event.sequence = sequence;
    event.pool = pool_id;
    event.queue = queue_id;
    event.allocation = allocation;
    event.units = units;
    event.outcome = outcome;
    event.detail = std::move(detail);
    events.push_back(std::move(event));
}

void BufferFabric::Impl::publish(std::vector<FabricEvent>& events) {
    if (events.empty()) return;
    IEventSink* sink = nullptr;
    {
        std::lock_guard<std::mutex> guard(sink_mutex_);
        sink = sink_;
    }
    // No fabric lock is held here. A sink therefore cannot deadlock against
    // authoritative state and cannot observe a half-applied mutation.
    for (const auto& event : events) {
        local_sink_.on_event(event);
    }
    if (sink != nullptr) {
        for (const auto& event : events) {
            sink->on_event(event);
        }
    }
    events.clear();
}

// ---------------------------------------------------------------------------
// Durable mutation plumbing
// ---------------------------------------------------------------------------

Status BufferFabric::Impl::journal(JournalOp op, const void* payload, usize size) {
    if (store_.config().mode == DurabilityMode::None) return Status::success();
    // Durable growth is bounded: when the journal reaches its compaction
    // threshold the current state is snapshotted first. The snapshot is taken
    // before the new record is appended, so it always describes a consistent
    // prefix of the mutation sequence and replaying the remaining journal over
    // it is a no-op.
    if (store_.needs_compaction()) {
        BF_TRY(write_full_snapshot());
    }
    ByteWriter writer(size + 1);
    writer.put_u8(static_cast<u8>(op));
    writer.put_bytes(payload, size);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "journal payload could not be encoded");
    }
    Status status = store_.append_record(writer.data().data(), writer.size());
    if (status.ok()) {
        metrics_.journal_records_written += 1;
        metrics_.journal_bytes_written += writer.size();
    }
    return status;
}

Status BufferFabric::Impl::journal_policy(const PoolPolicy& policy) {
    ByteWriter writer(512);
    detail::encode_policy(writer, policy);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "policy record could not be encoded");
    }
    return journal(JournalOp::PolicyUpsert, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_pool(const PoolEntry& entry) {
    ByteWriter writer(512);
    detail::encode_pool(writer, entry.descriptor);
    writer.put_id(entry.capacity_generation);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "pool record could not be encoded");
    }
    return journal(JournalOp::PoolUpsert, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_queue(const QueueDescriptor& queue) {
    ByteWriter writer(512);
    detail::encode_queue(writer, queue);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "queue record could not be encoded");
    }
    return journal(JournalOp::QueueUpsert, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_allocation(JournalOp op, const AllocationRecord& record) {
    ByteWriter writer(512);
    detail::encode_allocation(writer, record);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "allocation record could not be encoded");
    }
    return journal(op, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_capacity(const CapacityEntry& capacity) {
    ByteWriter writer(128);
    writer.put_id(capacity.resource);
    writer.put_id(capacity.backend);
    writer.put_id(capacity.generation);
    writer.put_u64(capacity.raw_units);
    writer.put_u64(capacity.observed_at);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "capacity record could not be encoded");
    }
    return journal(JournalOp::CapacitySet, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_attempt(const AttemptRecord& record) {
    ByteWriter writer(256);
    detail::encode_attempt(writer, record);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "attempt record could not be encoded");
    }
    return journal(JournalOp::AttemptRecorded, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::journal_pressure(const PressureSnapshot& snapshot) {
    ByteWriter writer(256);
    detail::encode_pressure_snapshot(writer, snapshot);
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "pressure record could not be encoded");
    }
    return journal(JournalOp::PressureObserved, writer.data().data(), writer.size());
}

Status BufferFabric::Impl::ensure_alloc_capacity() const {
    if (allocations_.size() >= kMaxAllocationCount) {
        return Status(ErrorCode::BoundedResourceExhausted,
                      "the allocation table has reached its configured bound");
    }
    return Status::success();
}

// ---------------------------------------------------------------------------
// Recovery and snapshots
// ---------------------------------------------------------------------------

Status BufferFabric::Impl::apply_snapshot_payload(const SnapshotHeader& header, const u8* payload, usize size) {
    BF_UNUSED(header);
    ByteReader reader(payload, size);
    u8 kind = 0;
    if (!reader.get_u8(&kind) || kind != 0xFFu) {
        return Status(ErrorCode::CorruptRecord, "snapshot payload is not a full image");
    }
    u64 value = 0;
    if (!reader.get_u64(&value)) return Status(ErrorCode::CorruptRecord, "snapshot image header");
    epoch_ = EpochId::from_raw(value);
    if (!reader.get_u64(&boot_.hi)) return Status(ErrorCode::CorruptRecord, "snapshot boot");
    if (!reader.get_u64(&boot_.lo)) return Status(ErrorCode::CorruptRecord, "snapshot boot");
    u64 counters[7] = {0, 0, 0, 0, 0, 0, 0};
    for (u64& counter : counters) {
        if (!reader.get_u64(&counter)) return Status(ErrorCode::CorruptRecord, "snapshot counters");
    }
    next_pool_id_ = counters[0] == 0 ? 1 : counters[0];
    next_queue_id_ = counters[1] == 0 ? 1 : counters[1];
    next_allocation_id_ = counters[2] == 0 ? 1 : counters[2];
    next_policy_id_ = counters[3] == 0 ? 1 : counters[3];
    next_snapshot_id_ = counters[4] == 0 ? 1 : counters[4];
    next_decision_sequence_ = counters[5] == 0 ? 1 : counters[5];
    next_attempt_sequence_ = counters[6] == 0 ? 1 : counters[6];

    u32 count = 0;
    if (!reader.get_u32(&count) || count > kMaxPolicyCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot policy count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        PoolPolicy policy;
        if (!detail::decode_policy(reader, &policy)) {
            return Status(ErrorCode::CorruptRecord, "snapshot policy record is not decodable");
        }
        if (!policy.validate().ok()) {
            return Status(ErrorCode::CorruptRecord, "snapshot contains an invalid policy");
        }
        policy_names_[policy.name] = policy.id.raw();
        policies_[policy.id.raw()] = std::move(policy);
    }

    if (!reader.get_u32(&count) || count > kMaxPoolCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot pool count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        PoolEntry entry;
        if (!detail::decode_pool(reader, &entry.descriptor)) {
            return Status(ErrorCode::CorruptRecord, "snapshot pool record is not decodable");
        }
        if (!reader.get_id(&entry.capacity_generation)) {
            return Status(ErrorCode::CorruptRecord, "snapshot pool capacity generation");
        }
        if (entry.descriptor.validate().ok() == false) {
            return Status(ErrorCode::CorruptRecord, "snapshot contains an invalid pool");
        }
        pool_names_[entry.descriptor.name] = entry.descriptor.id.raw();
        pools_[entry.descriptor.id.raw()] = std::move(entry);
    }

    if (!reader.get_u32(&count) || count > kMaxQueueCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot queue count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        QueueDescriptor queue;
        if (!detail::decode_queue(reader, &queue)) {
            return Status(ErrorCode::CorruptRecord, "snapshot queue record is not decodable");
        }
        queue_names_[queue.name] = queue.id.raw();
        queues_[queue.id.raw()] = std::move(queue);
    }

    if (!reader.get_u32(&count) || count > kMaxAllocationCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot allocation count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        AllocationRecord record;
        if (!detail::decode_allocation(reader, &record)) {
            return Status(ErrorCode::CorruptRecord, "snapshot allocation record is not decodable");
        }
        if (record.units > kMaxUnitsPerPool) {
            return Status(ErrorCode::CorruptRecord, "snapshot allocation units are out of range");
        }
        allocations_[record.id.raw()] = std::move(record);
    }

    if (!reader.get_u32(&count) || count > kMaxSnapshotCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot pressure count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        PressureSnapshot snapshot;
        if (!detail::decode_pressure_snapshot(reader, &snapshot)) {
            return Status(ErrorCode::CorruptRecord, "snapshot pressure record is not decodable");
        }
        PressureEntry& entry = pressure_[snapshot.pool.raw()];
        entry.latest = snapshot;
        entry.has_latest = true;
        entry.latest_revalidated = false;
        entry.history.push_back(snapshot);
    }

    if (!reader.get_u32(&count) || count > static_cast<u32>(kMaxAttemptMemory)) {
        return Status(ErrorCode::CorruptRecord, "snapshot attempt count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        AttemptRecord attempt;
        if (!detail::decode_attempt(reader, &attempt)) {
            return Status(ErrorCode::CorruptRecord, "snapshot attempt record is not decodable");
        }
        attempt_order_.push_back(attempt.id.raw());
        attempts_[attempt.id.raw()] = std::move(attempt);
    }

    if (!reader.get_u32(&count) || count > kMaxPoolCount) {
        return Status(ErrorCode::CorruptRecord, "snapshot capacity count is out of range");
    }
    for (u32 i = 0; i < count; ++i) {
        CapacityEntry capacity;
        if (!reader.get_id(&capacity.resource) || !reader.get_id(&capacity.backend) ||
            !reader.get_id(&capacity.generation) || !reader.get_u64(&capacity.raw_units) ||
            !reader.get_u64(&capacity.observed_at)) {
            return Status(ErrorCode::CorruptRecord, "snapshot capacity record is not decodable");
        }
        capacities_[capacity.resource.raw()] = capacity;
    }

    if (!reader.empty()) {
        return Status(ErrorCode::CorruptRecord, "snapshot payload has trailing bytes");
    }
    return Status::success();
}

Status BufferFabric::Impl::apply_journal_payload(const u8* payload, usize size, u64 ordinal) {
    BF_UNUSED(ordinal);
    ByteReader reader(payload, size);
    u8 raw_op = 0;
    if (!reader.get_u8(&raw_op)) {
        return Status(ErrorCode::CorruptRecord, "journal record has no opcode");
    }
    const auto op = static_cast<JournalOp>(raw_op);
    switch (op) {
        case JournalOp::EpochAdvance: {
            u64 value = 0;
            if (!reader.get_u64(&value)) return Status(ErrorCode::CorruptRecord, "epoch record");
            const EpochId recorded = EpochId::from_raw(value);
            if (recorded > epoch_) epoch_ = recorded;
            u64 hi = 0;
            u64 lo = 0;
            if (!reader.get_u64(&hi) || !reader.get_u64(&lo)) {
                return Status(ErrorCode::CorruptRecord, "epoch record boot");
            }
            if (hi != 0 || lo != 0) {
                boot_.hi = hi;
                boot_.lo = lo;
            }
            return Status::success();
        }
        case JournalOp::PolicyUpsert: {
            PoolPolicy policy;
            if (!detail::decode_policy(reader, &policy)) {
                return Status(ErrorCode::CorruptRecord, "policy record is not decodable");
            }
            if (!policy.validate().ok()) {
                return Status(ErrorCode::CorruptRecord, "journal contains an invalid policy");
            }
            policy_names_[policy.name] = policy.id.raw();
            policies_[policy.id.raw()] = std::move(policy);
            return Status::success();
        }
        case JournalOp::PoolUpsert: {
            PoolEntry entry;
            if (!detail::decode_pool(reader, &entry.descriptor) ||
                !reader.get_id(&entry.capacity_generation)) {
                return Status(ErrorCode::CorruptRecord, "pool record is not decodable");
            }
            if (!entry.descriptor.validate().ok()) {
                return Status(ErrorCode::CorruptRecord, "journal contains an invalid pool");
            }
            auto existing = pools_.find(entry.descriptor.id.raw());
            if (existing != pools_.end()) {
                const u64 preserved_members = existing->second.members.size();
                BF_UNUSED(preserved_members);
                entry.members = existing->second.members;
                entry.runtime = existing->second.runtime;
            }
            pool_names_[entry.descriptor.name] = entry.descriptor.id.raw();
            pools_[entry.descriptor.id.raw()] = std::move(entry);
            return Status::success();
        }
        case JournalOp::PoolRemoved: {
            u64 raw_id = 0;
            if (!reader.get_u64(&raw_id)) return Status(ErrorCode::CorruptRecord, "pool removal");
            const auto it = pools_.find(raw_id);
            if (it != pools_.end()) {
                pool_names_.erase(it->second.descriptor.name);
                pools_.erase(it);
            }
            return Status::success();
        }
        case JournalOp::QueueUpsert: {
            QueueDescriptor queue;
            if (!detail::decode_queue(reader, &queue)) {
                return Status(ErrorCode::CorruptRecord, "queue record is not decodable");
            }
            queue_names_[queue.name] = queue.id.raw();
            queues_[queue.id.raw()] = std::move(queue);
            return Status::success();
        }
        case JournalOp::QueueRetired: {
            u64 raw_id = 0;
            if (!reader.get_u64(&raw_id)) return Status(ErrorCode::CorruptRecord, "queue retirement");
            const auto it = queues_.find(raw_id);
            if (it != queues_.end()) it->second.retired = true;
            return Status::success();
        }
        case JournalOp::CapacitySet: {
            CapacityEntry capacity;
            if (!reader.get_id(&capacity.resource) || !reader.get_id(&capacity.backend) ||
                !reader.get_id(&capacity.generation) || !reader.get_u64(&capacity.raw_units) ||
                !reader.get_u64(&capacity.observed_at)) {
                return Status(ErrorCode::CorruptRecord, "capacity record is not decodable");
            }
            capacities_[capacity.resource.raw()] = capacity;
            return Status::success();
        }
        case JournalOp::AllocationReserved:
        case JournalOp::AllocationCommitted:
        case JournalOp::AllocationReleased:
        case JournalOp::AllocationReclaimed:
        case JournalOp::AllocationFenced:
        case JournalOp::AllocationRevalidated:
        case JournalOp::AllocationExpired: {
            AllocationRecord record;
            if (!detail::decode_allocation(reader, &record)) {
                return Status(ErrorCode::CorruptRecord, "allocation record is not decodable");
            }
            if (record.units > kMaxUnitsPerPool) {
                return Status(ErrorCode::CorruptRecord, "allocation units are out of range");
            }
            allocations_[record.id.raw()] = std::move(record);
            return Status::success();
        }
        case JournalOp::PressureObserved: {
            PressureSnapshot snapshot;
            if (!detail::decode_pressure_snapshot(reader, &snapshot)) {
                return Status(ErrorCode::CorruptRecord, "pressure record is not decodable");
            }
            PressureEntry& entry = pressure_[snapshot.pool.raw()];
            entry.latest = snapshot;
            entry.has_latest = true;
            entry.latest_revalidated = false;
            entry.history.push_back(snapshot);
            while (entry.history.size() > config_.pressure_history_capacity) {
                entry.history.pop_front();
            }
            return Status::success();
        }
        case JournalOp::AttemptRecorded: {
            AttemptRecord attempt;
            if (!detail::decode_attempt(reader, &attempt)) {
                return Status(ErrorCode::CorruptRecord, "attempt record is not decodable");
            }
            if (attempts_.find(attempt.id.raw()) == attempts_.end()) {
                attempt_order_.push_back(attempt.id.raw());
            }
            attempts_[attempt.id.raw()] = std::move(attempt);
            return Status::success();
        }
        case JournalOp::Checkpoint:
        case JournalOp::BackendRegistered:
            return Status::success();
    }
    return Status(ErrorCode::CorruptRecord, "journal record carries an unknown opcode");
}

Status BufferFabric::Impl::write_full_snapshot() {
    if (store_.config().mode == DurabilityMode::None) {
        return Status::success();
    }
    u32 pressure_with_latest = 0;
    for (const auto& [id, entry] : pressure_) {
        BF_UNUSED(id);
        if (entry.has_latest) ++pressure_with_latest;
    }

    ByteWriter writer(4096);
    writer.put_u8(0xFFu);
    writer.put_u64(epoch_.raw());
    writer.put_u64(boot_.hi);
    writer.put_u64(boot_.lo);
    writer.put_u64(next_pool_id_);
    writer.put_u64(next_queue_id_);
    writer.put_u64(next_allocation_id_);
    writer.put_u64(next_policy_id_);
    writer.put_u64(next_snapshot_id_);
    writer.put_u64(next_decision_sequence_);
    writer.put_u64(next_attempt_sequence_);
    writer.put_u32(static_cast<u32>(policies_.size()));
    for (const auto& [id, policy] : policies_) {
        BF_UNUSED(id);
        detail::encode_policy(writer, policy);
    }
    writer.put_u32(static_cast<u32>(pools_.size()));
    for (const auto& [id, entry] : pools_) {
        BF_UNUSED(id);
        detail::encode_pool(writer, entry.descriptor);
        writer.put_id(entry.capacity_generation);
    }
    writer.put_u32(static_cast<u32>(queues_.size()));
    for (const auto& [id, queue] : queues_) {
        BF_UNUSED(id);
        detail::encode_queue(writer, queue);
    }
    writer.put_u32(static_cast<u32>(allocations_.size()));
    for (const auto& [id, record] : allocations_) {
        BF_UNUSED(id);
        detail::encode_allocation(writer, record);
    }
    writer.put_u32(pressure_with_latest);
    for (const auto& [id, entry] : pressure_) {
        BF_UNUSED(id);
        if (!entry.has_latest) continue;
        detail::encode_pressure_snapshot(writer, entry.latest);
    }
    writer.put_u32(static_cast<u32>(attempts_.size()));
    for (const auto& [id, attempt] : attempts_) {
        BF_UNUSED(id);
        detail::encode_attempt(writer, attempt);
    }
    writer.put_u32(static_cast<u32>(capacities_.size()));
    for (const auto& [id, capacity] : capacities_) {
        BF_UNUSED(id);
        writer.put_id(capacity.resource);
        writer.put_id(capacity.backend);
        writer.put_id(capacity.generation);
        writer.put_u64(capacity.raw_units);
        writer.put_u64(capacity.observed_at);
    }
    if (writer.failed()) {
        return Status(ErrorCode::OversizedMessage, "snapshot image could not be encoded");
    }

    SnapshotHeader header;
    header.format_version = BUFFER_FABRIC_PERSISTENCE_FORMAT_VERSION;
    header.epoch = epoch_;
    header.boot = boot_;
    header.record_count = metrics_.journal_records_written;
    BF_TRY(store_.write_snapshot(header, writer.data().data(), writer.size()));
    metrics_.snapshots_written += 1;
    return Status::success();
}

Status BufferFabric::Impl::load_from_store() {
    DurableStore::SnapshotVisitor snapshot_visitor =
        [this](const SnapshotHeader& header, const u8* payload, usize size) -> Status {
        return apply_snapshot_payload(header, payload, size);
    };
    DurableStore::RecordVisitor record_visitor = [this](const u8* payload, usize size,
                                                        u64 ordinal) -> Status {
        return apply_journal_payload(payload, size, ordinal);
    };
    BF_TRY(store_.recover(snapshot_visitor, record_visitor, &recovery_));

    recovery_.pools_restored = pools_.size();
    recovery_.policies_restored = policies_.size();
    recovery_.queues_restored = queues_.size();

    // Reservations are leases on capacity, not durable commitments. They are
    // reported as unfinished attempts and their units are returned to free
    // capacity: a restart never silently restores a live lease.
    const Tick stamp = now();
    for (auto& [id, record] : allocations_) {
        BF_UNUSED(id);
        if (record.state != AllocationState::Reserved) continue;
        UnfinishedAttempt attempt;
        attempt.attempt = record.attempt;
        attempt.kind = AttemptKind::Reserve;
        attempt.allocation = record.id;
        attempt.units = record.units;
        attempt.journaled_at = record.created_at;
        recovery_.unfinished_attempts.push_back(attempt);
        record.state = AllocationState::Expired;
        record.last_touched = stamp;
        record.revision += 1;
        recovery_.allocations_restored_as_uncommitted += 1;
    }
    for (const auto& [id, record] : allocations_) {
        BF_UNUSED(id);
        if (record.state == AllocationState::Committed) {
            recovery_.allocations_restored += 1;
        }
        if (record.state == AllocationState::Fenced ||
            record.state == AllocationState::Reclaimed) {
            recovery_.fences_restored += 1;
        }
    }
    for (const auto& [id, entry] : pressure_) {
        BF_UNUSED(id);
        if (entry.has_latest) recovery_.pressure_snapshots_invalidated += 1;
    }

    rebuild_aggregates();
    return Status::success();
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

BufferFabric::BufferFabric() : impl_(std::make_unique<Impl>()) {}

BufferFabric::~BufferFabric() {
    if (impl_) {
        impl_->store_.close();
        impl_->open_ = false;
    }
}

bool BufferFabric::is_open() const noexcept {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->open_;
}

EpochId BufferFabric::epoch() const noexcept {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->epoch_;
}

BootIncarnation BufferFabric::boot_incarnation() const noexcept {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->boot_;
}

RecoveryReport BufferFabric::recovery_report() const {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->recovery_;
}

void BufferFabric::begin_shutdown() noexcept {
    BufferFabric::Impl::Lock guard(*impl_);
    impl_->shutting_down_ = true;
}

bool BufferFabric::shutting_down() const noexcept {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->shutting_down_;
}

void BufferFabric::set_event_sink(IEventSink* sink) {
    std::lock_guard<std::mutex> guard(impl_->sink_mutex_);
    impl_->sink_ = sink;
}

void BufferFabric::set_clock(IClock* clock) {
    BufferFabric::Impl::Lock guard(*impl_);
    impl_->clock_ = clock;
}

Status BufferFabric::open(const FabricConfig& config) {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (impl_->open_) {
            return Status(ErrorCode::LifecycleViolation, "fabric is already open");
        }
        if (config.decision_history_capacity == 0) {
            return Status(ErrorCode::InvalidArgument, "decision_history_capacity must be non-zero");
        }
        if (config.attempt_memory_capacity == 0 || config.attempt_memory_capacity > kMaxAttemptMemory) {
            return Status(ErrorCode::InvalidArgument, "attempt_memory_capacity out of range");
        }
        if (config.pressure_history_capacity == 0 || config.pressure_history_capacity > kMaxSnapshotCount) {
            return Status(ErrorCode::InvalidArgument, "pressure_history_capacity out of range");
        }
        if (config.max_inspection_rows == 0 || config.max_inspection_rows > kMaxAllocationCount) {
            return Status(ErrorCode::InvalidArgument, "max_inspection_rows out of range");
        }
        impl_->config_ = config;

        EpochId durable_epoch{};
        if (config.persistence.mode != DurabilityMode::None) {
            status = impl_->store_.configure(config.persistence);
            if (!status.ok()) {
                return status;
            }
            status = impl_->load_from_store();
            if (!status.ok()) {
                impl_->store_.close();
                return status;
            }
            durable_epoch = impl_->epoch_;
            impl_->metrics_.recoveries += 1;
            impl_->metrics_.journal_records_replayed += impl_->recovery_.journal_records_applied;
            impl_->metrics_.journal_tail_truncations +=
                impl_->recovery_.journal_bytes_discarded == 0 ? 0 : 1;
        }

        u64 next_epoch = 0;
        if (add_overflow(durable_epoch.raw(), 1, &next_epoch) || next_epoch == 0) {
            impl_->store_.close();
            return Status(ErrorCode::EpochRegression, "the fabric epoch is exhausted");
        }
        impl_->epoch_ = EpochId::from_raw(next_epoch);
        impl_->recovery_.durable_epoch = durable_epoch;
        impl_->recovery_.new_epoch = impl_->epoch_;
        impl_->recovery_.previous_boot = impl_->boot_;
        impl_->recovery_.boot = impl_->mint_boot();
        impl_->boot_ = impl_->recovery_.boot;
        impl_->metrics_.epoch_advances += 1;
        impl_->open_ = true;
        impl_->shutting_down_ = false;

        if (config.persistence.mode != DurabilityMode::None) {
            ByteWriter writer(32);
            writer.put_u64(impl_->epoch_.raw());
            writer.put_u64(impl_->boot_.hi);
            writer.put_u64(impl_->boot_.lo);
            status = impl_->journal(JournalOp::EpochAdvance, writer.data().data(), writer.size());
            if (!status.ok()) {
                impl_->open_ = false;
                impl_->store_.close();
                return status;
            }
        }

        impl_->push_event(events, EventKind::Recovered, ErrorCode::Ok,
                          std::string(to_string(impl_->recovery_.outcome)));
        impl_->push_event(events, EventKind::EpochAdvanced, ErrorCode::Ok, "fabric opened");
    }
    impl_->publish(events);
    return Status::success();
}

VoidResult BufferFabric::close() {
    std::vector<FabricEvent> events;
    Status status;
    {
        BufferFabric::Impl::Lock guard(*impl_);
        if (!impl_->open_) return Status::success();
        impl_->shutting_down_ = true;
        // A clean close does not need to write a snapshot: every accepted
        // mutation was already journaled and flushed at its own durability
        // boundary. Snapshotting happens on explicit checkpoint() or
        // automatically when the journal reaches its compaction threshold.
        impl_->store_.close();
        impl_->shutting_down_ = false;
        impl_->open_ = false;
        impl_->push_event(events, EventKind::ShutdownComplete, ErrorCode::Ok, "fabric closed");
    }
    impl_->publish(events);
    return status;
}

VoidResult BufferFabric::attach_backend(IBackend* backend) {
    if (backend == nullptr) {
        return Status(ErrorCode::InvalidArgument, "backend pointer is null");
    }
    // Every backend accessor is a virtual call into embedder code, so all of
    // them happen before the fabric lock is taken.
    const BackendId backend_id = backend->id();
    const BackendKind kind = backend->kind();
    const Generation generation = backend->generation();
    const bool ready = backend->ready();
    if (!backend_id.valid()) {
        return Status(ErrorCode::InvalidArgument, "backend id must be non-zero");
    }
    BufferFabric::Impl::Lock guard(*impl_);
    if (!impl_->open_) {
        return Status(ErrorCode::LifecycleViolation, "fabric is not open");
    }
    BackendEntry entry;
    entry.backend = backend;
    entry.kind = kind;
    entry.generation = generation;
    entry.ready = ready;
    impl_->backends_[backend_id.raw()] = entry;
    return VoidResult{};
}

u64 BufferFabric::backend_count() const {
    BufferFabric::Impl::Lock guard(*impl_);
    return impl_->backends_.size();
}

}  // namespace buffer_fabric
// Continued in fabric_topology.cpp and fabric_alloc.cpp.
