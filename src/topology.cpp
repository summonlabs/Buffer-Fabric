#include "buffer_fabric/topology.hpp"

#include <string>

#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/pressure.hpp"
#include "wire.hpp"

namespace buffer_fabric {

std::string_view to_string(AllocationState state) noexcept {
    switch (state) {
        case AllocationState::Reserved: return "RESERVED";
        case AllocationState::Committed: return "COMMITTED";
        case AllocationState::Fenced: return "FENCED";
        case AllocationState::Reclaimed: return "RECLAIMED";
        case AllocationState::Released: return "RELEASED";
        case AllocationState::Expired: return "EXPIRED";
    }
    return "UNKNOWN";
}

bool allocation_holds_units(AllocationState state) noexcept {
    return state == AllocationState::Reserved || state == AllocationState::Committed;
}

bool allocation_is_terminal(AllocationState state) noexcept {
    return state == AllocationState::Released || state == AllocationState::Reclaimed ||
           state == AllocationState::Expired;
}

std::string_view to_string(ReclaimClass klass) noexcept {
    switch (klass) {
        case ReclaimClass::Pinned: return "PINNED";
        case ReclaimClass::Reclaimable: return "RECLAIMABLE";
    }
    return "UNKNOWN";
}

std::string_view to_string(AttemptKind kind) noexcept {
    switch (kind) {
        case AttemptKind::Allocate: return "ALLOCATE";
        case AttemptKind::Commit: return "COMMIT";
        case AttemptKind::Release: return "RELEASE";
        case AttemptKind::Reclaim: return "RECLAIM";
        case AttemptKind::Fence: return "FENCE";
        case AttemptKind::Revalidate: return "REVALIDATE";
        case AttemptKind::Reserve: return "RESERVE";
    }
    return "UNKNOWN";
}

Status PoolDescriptor::validate() const {
    if (id.raw() == 0) {
        return Status(ErrorCode::InvalidArgument, "pool id must be non-zero");
    }
    if (!is_valid_name(name)) {
        return Status(ErrorCode::NameTooLong,
                      "pool name must be 1..64 characters of [A-Za-z0-9_.-]");
    }
    if (raw_units == 0) {
        return Status(ErrorCode::InvalidArgument, "pool raw capacity must be non-zero");
    }
    if (raw_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "pool raw capacity exceeds the supported bound");
    }
    if (protected_units > raw_units) {
        return Status(ErrorCode::ProtectedHeadroomViolation,
                      "protected headroom exceeds raw capacity");
    }
    if (protected_units == raw_units) {
        return Status(ErrorCode::ProtectedHeadroomViolation,
                      "a pool whose entire capacity is protected can never allocate");
    }
    if (!backend.valid()) {
        return Status(ErrorCode::UnknownBackend, "pool must reference a backend");
    }
    if (!policy.valid()) {
        return Status(ErrorCode::UnknownPolicy, "pool must reference a policy");
    }
    if (parent == id) {
        return Status(ErrorCode::PoolCycle, "pool cannot be its own parent");
    }
    return Status::success();
}

Digest BoundGenerations::digest() const {
    ByteWriter writer(64);
    detail::encode_bound(writer, *this);
    return digest_bytes(writer.data().data(), writer.size());
}

namespace detail {

void encode_string(ByteWriter& writer, const std::string& text, usize max_bytes) {
    writer.put_str(text, max_bytes, "string");
}

bool decode_string(ByteReader& reader, std::string* out, usize max_bytes) {
    return reader.get_str(out, max_bytes);
}

void encode_thresholds(ByteWriter& writer, const Thresholds& thresholds) {
    writer.put_u32(thresholds.elevated_bp);
    writer.put_u32(thresholds.high_bp);
    writer.put_u32(thresholds.critical_bp);
}

bool decode_thresholds(ByteReader& reader, Thresholds* out) {
    if (!reader.get_u32(&out->elevated_bp)) return false;
    if (!reader.get_u32(&out->high_bp)) return false;
    if (!reader.get_u32(&out->critical_bp)) return false;
    return true;
}

void encode_overcommit(ByteWriter& writer, const OvercommitPolicy& policy) {
    writer.put_u8(static_cast<u8>(policy.mode));
    writer.put_u64(policy.limit_units);
}

bool decode_overcommit(ByteReader& reader, OvercommitPolicy* out) {
    u8 mode = 0;
    if (!reader.get_u8(&mode)) return false;
    if (mode > static_cast<u8>(OvercommitMode::Bounded)) return false;
    out->mode = static_cast<OvercommitMode>(mode);
    return reader.get_u64(&out->limit_units);
}

void encode_borrow(ByteWriter& writer, const BorrowPolicy& policy) {
    writer.put_u8(static_cast<u8>(policy.mode));
    writer.put_u64(policy.limit_units);
}

bool decode_borrow(ByteReader& reader, BorrowPolicy* out) {
    u8 mode = 0;
    if (!reader.get_u8(&mode)) return false;
    if (mode > static_cast<u8>(BorrowMode::FromAncestorFree)) return false;
    out->mode = static_cast<BorrowMode>(mode);
    return reader.get_u64(&out->limit_units);
}

void encode_policy(ByteWriter& writer, const PoolPolicy& policy) {
    writer.put_id(policy.id);
    writer.put_id(policy.generation);
    encode_string(writer, policy.name, kWireMaxNameBytes);
    writer.put_id(policy.provenance);
    encode_overcommit(writer, policy.overcommit);
    encode_borrow(writer, policy.borrow);
    writer.put_u64(policy.max_single_allocation_units);
    writer.put_u64(policy.max_single_request_units);
    writer.put_u32(policy.soft_limit_bp);
    writer.put_bool(policy.allow_partial_grant);
    encode_thresholds(writer, policy.pressure.thresholds);
    writer.put_u64(policy.pressure.evidence_ttl_ticks);
    writer.put_bool(policy.pressure.refuse_increase_on_unknown);
    writer.put_bool(policy.pressure.reduce_on_high);
    writer.put_bool(policy.pressure.reduce_on_critical);
    writer.put_bool(policy.pressure.refuse_new_grants_above_high);
    writer.put_u8(static_cast<u8>(policy.refuse_grants_at));
    writer.put_u8(static_cast<u8>(policy.reduce_at));
    writer.put_bool(policy.reclaim.enabled);
    writer.put_u64(policy.reclaim.min_hold_ticks);
    writer.put_u64(policy.reclaim.max_units_per_operation);
    writer.put_u64(policy.reclaim.reserve_units);
    writer.put_bool(policy.reclaim.allow_protected_reclaim);
    writer.put_u64(policy.usable_floor_units);
    writer.put_bool(policy.fence_on_generation_change);
    writer.put_u64(policy.reservation_ttl_ticks);
    writer.put_u64(policy.max_pending_reservations);
}

bool decode_policy(ByteReader& reader, PoolPolicy* out) {
    if (!reader.get_id(&out->id)) return false;
    if (!reader.get_id(&out->generation)) return false;
    if (!decode_string(reader, &out->name, kWireMaxNameBytes)) return false;
    if (!reader.get_id(&out->provenance)) return false;
    if (!decode_overcommit(reader, &out->overcommit)) return false;
    if (!decode_borrow(reader, &out->borrow)) return false;
    if (!reader.get_u64(&out->max_single_allocation_units)) return false;
    if (!reader.get_u64(&out->max_single_request_units)) return false;
    if (!reader.get_u32(&out->soft_limit_bp)) return false;
    if (!reader.get_bool(&out->allow_partial_grant)) return false;
    if (!decode_thresholds(reader, &out->pressure.thresholds)) return false;
    if (!reader.get_u64(&out->pressure.evidence_ttl_ticks)) return false;
    if (!reader.get_bool(&out->pressure.refuse_increase_on_unknown)) return false;
    if (!reader.get_bool(&out->pressure.reduce_on_high)) return false;
    if (!reader.get_bool(&out->pressure.reduce_on_critical)) return false;
    if (!reader.get_bool(&out->pressure.refuse_new_grants_above_high)) return false;
    u8 refuse_at = 0;
    u8 reduce_at = 0;
    if (!reader.get_u8(&refuse_at)) return false;
    if (!reader.get_u8(&reduce_at)) return false;
    if (refuse_at > static_cast<u8>(PressureState::Stale)) return false;
    if (reduce_at > static_cast<u8>(PressureState::Stale)) return false;
    out->refuse_grants_at = static_cast<PressureState>(refuse_at);
    out->reduce_at = static_cast<PressureState>(reduce_at);
    if (!reader.get_bool(&out->reclaim.enabled)) return false;
    if (!reader.get_u64(&out->reclaim.min_hold_ticks)) return false;
    if (!reader.get_u64(&out->reclaim.max_units_per_operation)) return false;
    if (!reader.get_u64(&out->reclaim.reserve_units)) return false;
    if (!reader.get_bool(&out->reclaim.allow_protected_reclaim)) return false;
    if (!reader.get_u64(&out->usable_floor_units)) return false;
    if (!reader.get_bool(&out->fence_on_generation_change)) return false;
    if (!reader.get_u64(&out->reservation_ttl_ticks)) return false;
    if (!reader.get_u64(&out->max_pending_reservations)) return false;
    return true;
}

void encode_pool(ByteWriter& writer, const PoolDescriptor& pool) {
    writer.put_id(pool.id);
    writer.put_id(pool.generation);
    encode_string(writer, pool.name, kWireMaxNameBytes);
    writer.put_id(pool.resource);
    writer.put_id(pool.backend);
    writer.put_u64(pool.raw_units);
    writer.put_u64(pool.protected_units);
    writer.put_id(pool.parent);
    writer.put_id(pool.policy);
    writer.put_id(pool.tenant);
    writer.put_id(pool.provenance);
    writer.put_u64(pool.created_at);
    writer.put_bool(pool.retired);
}

bool decode_pool(ByteReader& reader, PoolDescriptor* out) {
    if (!reader.get_id(&out->id)) return false;
    if (!reader.get_id(&out->generation)) return false;
    if (!decode_string(reader, &out->name, kWireMaxNameBytes)) return false;
    if (!reader.get_id(&out->resource)) return false;
    if (!reader.get_id(&out->backend)) return false;
    if (!reader.get_u64(&out->raw_units)) return false;
    if (!reader.get_u64(&out->protected_units)) return false;
    if (!reader.get_id(&out->parent)) return false;
    if (!reader.get_id(&out->policy)) return false;
    if (!reader.get_id(&out->tenant)) return false;
    if (!reader.get_id(&out->provenance)) return false;
    if (!reader.get_u64(&out->created_at)) return false;
    if (!reader.get_bool(&out->retired)) return false;
    return true;
}

void encode_queue(ByteWriter& writer, const QueueDescriptor& queue) {
    writer.put_id(queue.id);
    writer.put_id(queue.generation);
    encode_string(writer, queue.name, kWireMaxNameBytes);
    writer.put_id(queue.pool);
    writer.put_id(queue.tenant);
    writer.put_id(queue.traffic_class);
    writer.put_u64(queue.demand_units);
    writer.put_u64(queue.weight);
    writer.put_u64(queue.max_committed_units);
    writer.put_u64(queue.registered_at);
    writer.put_u64(queue.demand_updated_at);
    writer.put_bool(queue.retired);
}

bool decode_queue(ByteReader& reader, QueueDescriptor* out) {
    if (!reader.get_id(&out->id)) return false;
    if (!reader.get_id(&out->generation)) return false;
    if (!decode_string(reader, &out->name, kWireMaxNameBytes)) return false;
    if (!reader.get_id(&out->pool)) return false;
    if (!reader.get_id(&out->tenant)) return false;
    if (!reader.get_id(&out->traffic_class)) return false;
    if (!reader.get_u64(&out->demand_units)) return false;
    if (!reader.get_u64(&out->weight)) return false;
    if (!reader.get_u64(&out->max_committed_units)) return false;
    if (!reader.get_u64(&out->registered_at)) return false;
    if (!reader.get_u64(&out->demand_updated_at)) return false;
    if (!reader.get_bool(&out->retired)) return false;
    return true;
}

void encode_lineage(ByteWriter& writer, const Lineage& lineage) {
    writer.put_id(lineage.origin);
    writer.put_id(lineage.parent);
    writer.put_u32(lineage.depth);
    writer.put_digest(lineage.chain_digest);
}

bool decode_lineage(ByteReader& reader, Lineage* out) {
    if (!reader.get_id(&out->origin)) return false;
    if (!reader.get_id(&out->parent)) return false;
    if (!reader.get_u32(&out->depth)) return false;
    if (!reader.get_digest(&out->chain_digest)) return false;
    return true;
}

void encode_bound(ByteWriter& writer, const BoundGenerations& bound) {
    writer.put_id(bound.pool_generation);
    writer.put_id(bound.queue_generation);
    writer.put_id(bound.policy_generation);
    writer.put_id(bound.capacity_generation);
    writer.put_id(bound.backend_generation);
    writer.put_id(bound.epoch);
    writer.put_u64(bound.boot.hi);
    writer.put_u64(bound.boot.lo);
}

bool decode_bound(ByteReader& reader, BoundGenerations* out) {
    if (!reader.get_id(&out->pool_generation)) return false;
    if (!reader.get_id(&out->queue_generation)) return false;
    if (!reader.get_id(&out->policy_generation)) return false;
    if (!reader.get_id(&out->capacity_generation)) return false;
    if (!reader.get_id(&out->backend_generation)) return false;
    if (!reader.get_id(&out->epoch)) return false;
    if (!reader.get_u64(&out->boot.hi)) return false;
    if (!reader.get_u64(&out->boot.lo)) return false;
    return true;
}

void encode_allocation(ByteWriter& writer, const AllocationRecord& allocation) {
    writer.put_id(allocation.id);
    writer.put_id(allocation.pool);
    writer.put_id(allocation.queue);
    writer.put_id(allocation.tenant);
    writer.put_id(allocation.traffic_class);
    writer.put_u64(allocation.units);
    writer.put_u8(static_cast<u8>(allocation.state));
    writer.put_u8(static_cast<u8>(allocation.reclaim_class));
    writer.put_u64(allocation.reclaim_priority);
    encode_lineage(writer, allocation.lineage);
    encode_bound(writer, allocation.bound);
    writer.put_id(allocation.attempt);
    writer.put_id(allocation.provenance);
    writer.put_u64(allocation.borrow_units);
    writer.put_id(allocation.lender);
    writer.put_u64(allocation.created_at);
    writer.put_u64(allocation.last_touched);
    writer.put_u64(allocation.expires_at);
    writer.put_u64(allocation.revision);
}

bool decode_allocation(ByteReader& reader, AllocationRecord* out) {
    if (!reader.get_id(&out->id)) return false;
    if (!reader.get_id(&out->pool)) return false;
    if (!reader.get_id(&out->queue)) return false;
    if (!reader.get_id(&out->tenant)) return false;
    if (!reader.get_id(&out->traffic_class)) return false;
    if (!reader.get_u64(&out->units)) return false;
    u8 state = 0;
    if (!reader.get_u8(&state)) return false;
    if (state > static_cast<u8>(AllocationState::Expired)) return false;
    out->state = static_cast<AllocationState>(state);
    u8 klass = 0;
    if (!reader.get_u8(&klass)) return false;
    if (klass > static_cast<u8>(ReclaimClass::Reclaimable)) return false;
    out->reclaim_class = static_cast<ReclaimClass>(klass);
    if (!reader.get_u64(&out->reclaim_priority)) return false;
    if (!decode_lineage(reader, &out->lineage)) return false;
    if (!decode_bound(reader, &out->bound)) return false;
    if (!reader.get_id(&out->attempt)) return false;
    if (!reader.get_id(&out->provenance)) return false;
    if (!reader.get_u64(&out->borrow_units)) return false;
    if (!reader.get_id(&out->lender)) return false;
    if (!reader.get_u64(&out->created_at)) return false;
    if (!reader.get_u64(&out->last_touched)) return false;
    if (!reader.get_u64(&out->expires_at)) return false;
    if (!reader.get_u64(&out->revision)) return false;
    return true;
}

void encode_pressure_snapshot(ByteWriter& writer, const PressureSnapshot& snapshot) {
    writer.put_id(snapshot.id);
    writer.put_id(snapshot.pool);
    writer.put_id(snapshot.pool_generation);
    writer.put_id(snapshot.epoch);
    writer.put_u64(snapshot.boot.hi);
    writer.put_u64(snapshot.boot.lo);
    writer.put_u64(snapshot.observed_at);
    writer.put_u64(snapshot.ttl_ticks);
    writer.put_u64(snapshot.demand_units);
    writer.put_u64(snapshot.committed_units);
    writer.put_u64(snapshot.reclaimable_units);
    writer.put_id(snapshot.provenance);
    writer.put_bool(snapshot.assert_committed);
    writer.put_digest(snapshot.evidence_digest);
}

bool decode_pressure_snapshot(ByteReader& reader, PressureSnapshot* out) {
    if (!reader.get_id(&out->id)) return false;
    if (!reader.get_id(&out->pool)) return false;
    if (!reader.get_id(&out->pool_generation)) return false;
    if (!reader.get_id(&out->epoch)) return false;
    if (!reader.get_u64(&out->boot.hi)) return false;
    if (!reader.get_u64(&out->boot.lo)) return false;
    if (!reader.get_u64(&out->observed_at)) return false;
    if (!reader.get_u64(&out->ttl_ticks)) return false;
    if (!reader.get_u64(&out->demand_units)) return false;
    if (!reader.get_u64(&out->committed_units)) return false;
    if (!reader.get_u64(&out->reclaimable_units)) return false;
    if (!reader.get_id(&out->provenance)) return false;
    if (!reader.get_bool(&out->assert_committed)) return false;
    if (!reader.get_digest(&out->evidence_digest)) return false;
    return true;
}

void encode_attempt(ByteWriter& writer, const AttemptRecord& attempt) {
    writer.put_id(attempt.id);
    writer.put_u8(static_cast<u8>(attempt.kind));
    writer.put_digest(attempt.request_digest);
    writer.put_u16(static_cast<u16>(attempt.outcome));
    writer.put_id(attempt.allocation);
    writer.put_u64(attempt.granted_units);
    writer.put_u64(attempt.sequence);
    writer.put_u64(attempt.recorded_at);
    writer.put_bool(attempt.unfinished);
}

bool decode_attempt(ByteReader& reader, AttemptRecord* out) {
    if (!reader.get_id(&out->id)) return false;
    u8 kind = 0;
    if (!reader.get_u8(&kind)) return false;
    if (kind > static_cast<u8>(AttemptKind::Reserve)) return false;
    out->kind = static_cast<AttemptKind>(kind);
    if (!reader.get_digest(&out->request_digest)) return false;
    u16 outcome = 0;
    if (!reader.get_u16(&outcome)) return false;
    if (outcome > static_cast<u16>(ErrorCode::ProtocolVersionMismatch)) return false;
    out->outcome = static_cast<ErrorCode>(outcome);
    if (!reader.get_id(&out->allocation)) return false;
    if (!reader.get_u64(&out->granted_units)) return false;
    if (!reader.get_u64(&out->sequence)) return false;
    if (!reader.get_u64(&out->recorded_at)) return false;
    if (!reader.get_bool(&out->unfinished)) return false;
    return true;
}

}  // namespace detail

}  // namespace buffer_fabric
