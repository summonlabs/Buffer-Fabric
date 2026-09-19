#pragma once

#include <string>

#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/authority.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/pressure.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// The outcome class of an evaluation. This is an intent classification, not a
/// transport for congestion control: the fabric never paces, schedules or
/// shapes traffic; it states what capacity authority exists.
enum class DecisionKind : u8 {
    /// Requested units were granted in full.
    Grant = 0,
    /// A positive amount smaller than requested was granted.
    GrantPartial = 1,
    /// Nothing was granted.
    Refuse = 2,
    /// The claimant must reduce its committed usage to the reported target.
    Reduce = 3,
    /// A bound generation advanced; dependent allocations were invalidated.
    Fence = 4,
    /// Authority could not be established; the claimant must revalidate.
    Revalidate = 5,
    /// Reclamation intent was produced or applied.
    Reclaim = 6,
    /// The operation was a structural no-op (identical state already present).
    NoOp = 7,
    /// The attempt was already applied; the recorded outcome was replayed.
    IdempotentReplay = 8,
};

[[nodiscard]] BF_API std::string_view to_string(DecisionKind kind) noexcept;

/// What the fabric asks the claimant (or an operator) to do next. The fabric
/// publishes intent; acting on it is outside this runtime's boundary.
struct BF_API CorrectiveIntent {
    bool reduce_required{false};
    u64 reduce_to_units{0};
    bool fence_required{false};
    bool revalidate_required{false};
    u64 reclaim_target_units{0};
    Tick revalidate_by{0};

    [[nodiscard]] bool any() const noexcept {
        return reduce_required || fence_required || revalidate_required || reclaim_target_units != 0;
    }
};

/// A fully explained authoritative outcome.
struct BF_API Decision {
    DecisionKind kind{DecisionKind::Refuse};
    ErrorCode status{ErrorCode::Ok};
    std::string reason{};
    PoolId pool{};
    QueueId queue{};
    u64 requested_units{0};
    u64 granted_units{0};
    AllocationId allocation{};
    BindingConstraint binding{BindingConstraint::None};
    PressureState pressure{PressureState::Unknown};
    u32 utilization_bp{0};
    CorrectiveIntent intent{};
    AuthorityVector authority{};
    PoolAccounting accounting{};
    Tick decided_at{0};
    /// Fabric-side monotonic decision sequence.
    u64 sequence{0};
    /// Digest over the canonical encoding of this decision.
    Digest digest{};

    [[nodiscard]] bool succeeded() const noexcept {
        return status == ErrorCode::Ok &&
               (kind == DecisionKind::Grant || kind == DecisionKind::GrantPartial ||
                kind == DecisionKind::NoOp || kind == DecisionKind::IdempotentReplay);
    }
    [[nodiscard]] bool granted_anything() const noexcept { return granted_units != 0; }
};

// ---------------------------------------------------------------------------
// Requests
//
// expected_* fields are assertions by the caller. A zero generation means "no
// assertion"; the fabric then binds whatever is current. A non-zero assertion
// that does not match current state is refused as StaleGeneration. Missing
// authority is never upgraded to positive authority.
// ---------------------------------------------------------------------------
struct BF_API AllocateRequest {
    AttemptId attempt{};
    PoolId pool{};
    QueueId queue{};
    u64 requested_units{0};
    ReclaimClass reclaim_class{ReclaimClass::Reclaimable};
    u64 reclaim_priority{0};
    /// Lifetime of the allocation in ticks. 0 means no expiry.
    Tick ttl_ticks{0};
    /// When false the request produces a RESERVED allocation that must be
    /// committed explicitly.
    bool commit_immediately{true};
    /// Optional derivation lineage.
    AllocationId parent_allocation{};
    ProvenanceId provenance{};

    Generation expected_pool_generation{};
    Generation expected_policy_generation{};
    Generation expected_queue_generation{};
    Generation expected_capacity_generation{};
    EpochId expected_epoch{};

    [[nodiscard]] Digest request_digest() const;
};

struct BF_API CommitRequest {
    AttemptId attempt{};
    AllocationId allocation{};
    u64 commit_units{0};
    Generation expected_pool_generation{};
    EpochId expected_epoch{};

    [[nodiscard]] Digest request_digest() const;
};

struct BF_API ReleaseRequest {
    AttemptId attempt{};
    AllocationId allocation{};
    /// Units to release. Ignored when release_all is true.
    u64 units{0};
    bool release_all{true};
    Generation expected_pool_generation{};
    EpochId expected_epoch{};

    [[nodiscard]] Digest request_digest() const;
};

struct BF_API RevalidateRequest {
    AttemptId attempt{};
    AllocationId allocation{};
    Generation expected_pool_generation{};
    EpochId expected_epoch{};

    [[nodiscard]] Digest request_digest() const;
};

/// Evaluation of a hypothetical request. Never mutates authoritative state.
struct BF_API EvaluateRequest {
    PoolId pool{};
    QueueId queue{};
    u64 requested_units{0};
    ReclaimClass reclaim_class{ReclaimClass::Reclaimable};
    EpochId expected_epoch{};
};

struct BF_API EvaluationResult {
    Decision decision{};
    PoolAccounting accounting{};
    PressureEvidence evidence{};
};

/// Bounded, in-memory decision history retained for explanation and audit.
struct BF_API DecisionRecord {
    u64 sequence{0};
    Tick at{0};
    DecisionKind kind{DecisionKind::Refuse};
    ErrorCode status{ErrorCode::Ok};
    PoolId pool{};
    QueueId queue{};
    AllocationId allocation{};
    u64 requested_units{0};
    u64 granted_units{0};
    BindingConstraint binding{BindingConstraint::None};
    PressureState pressure{PressureState::Unknown};
    Digest authority_digest{};
    std::string reason{};
};

}  // namespace buffer_fabric
