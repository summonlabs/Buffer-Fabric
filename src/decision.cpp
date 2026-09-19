#include "buffer_fabric/decision.hpp"

#include "buffer_fabric/hash.hpp"
#include "buffer_fabric/reclaim.hpp"
#include "wire.hpp"

namespace buffer_fabric {

std::string_view to_string(DecisionKind kind) noexcept {
    switch (kind) {
        case DecisionKind::Grant: return "GRANT";
        case DecisionKind::GrantPartial: return "GRANT_PARTIAL";
        case DecisionKind::Refuse: return "REFUSE";
        case DecisionKind::Reduce: return "REDUCE";
        case DecisionKind::Fence: return "FENCE";
        case DecisionKind::Revalidate: return "REVALIDATE";
        case DecisionKind::Reclaim: return "RECLAIM";
        case DecisionKind::NoOp: return "NO_OP";
        case DecisionKind::IdempotentReplay: return "IDEMPOTENT_REPLAY";
    }
    return "UNKNOWN";
}

namespace {

[[nodiscard]] Digest finish(ByteWriter& writer) {
    return digest_bytes(writer.data().data(), writer.size());
}

}  // namespace

Digest AllocateRequest::request_digest() const {
    ByteWriter writer(160);
    writer.put_u8(static_cast<u8>(AttemptKind::Allocate));
    writer.put_id(attempt);
    writer.put_id(pool);
    writer.put_id(queue);
    writer.put_u64(requested_units);
    writer.put_u8(static_cast<u8>(reclaim_class));
    writer.put_u64(reclaim_priority);
    writer.put_u64(ttl_ticks);
    writer.put_bool(commit_immediately);
    writer.put_id(parent_allocation);
    writer.put_id(provenance);
    writer.put_id(expected_pool_generation);
    writer.put_id(expected_policy_generation);
    writer.put_id(expected_queue_generation);
    writer.put_id(expected_capacity_generation);
    writer.put_id(expected_epoch);
    return finish(writer);
}

Digest CommitRequest::request_digest() const {
    ByteWriter writer(80);
    writer.put_u8(static_cast<u8>(AttemptKind::Commit));
    writer.put_id(attempt);
    writer.put_id(allocation);
    writer.put_u64(commit_units);
    writer.put_id(expected_pool_generation);
    writer.put_id(expected_epoch);
    return finish(writer);
}

Digest ReleaseRequest::request_digest() const {
    ByteWriter writer(80);
    writer.put_u8(static_cast<u8>(AttemptKind::Release));
    writer.put_id(attempt);
    writer.put_id(allocation);
    writer.put_u64(units);
    writer.put_bool(release_all);
    writer.put_id(expected_pool_generation);
    writer.put_id(expected_epoch);
    return finish(writer);
}

Digest RevalidateRequest::request_digest() const {
    ByteWriter writer(64);
    writer.put_u8(static_cast<u8>(AttemptKind::Revalidate));
    writer.put_id(attempt);
    writer.put_id(allocation);
    writer.put_id(expected_pool_generation);
    writer.put_id(expected_epoch);
    return finish(writer);
}

Digest ReclaimRequest::request_digest() const {
    ByteWriter writer(80);
    writer.put_u8(static_cast<u8>(AttemptKind::Reclaim));
    writer.put_id(attempt);
    writer.put_id(pool);
    writer.put_u64(target_units);
    writer.put_bool(dry_run);
    writer.put_id(expected_pool_generation);
    writer.put_id(expected_epoch);
    return finish(writer);
}

}  // namespace buffer_fabric
