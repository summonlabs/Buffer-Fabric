#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/authority.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// One reclaimable allocation selected as a victim, with the exact evidence
/// that selected it.
struct BF_API ReclaimVictim {
    AllocationId allocation{};
    QueueId queue{};
    u64 units{0};
    u64 borrowed_units{0};
    u64 reclaim_priority{0};
    Tick last_touched{0};
    ReclaimClass reclaim_class{ReclaimClass::Reclaimable};
};

struct BF_API ReclaimRequest {
    AttemptId attempt{};
    PoolId pool{};
    u64 target_units{0};
    /// When true the plan is produced but nothing is revoked.
    bool dry_run{true};
    Generation expected_pool_generation{};
    EpochId expected_epoch{};

    [[nodiscard]] Digest request_digest() const;
};

/// A deterministic, explainable reclamation plan.
///
/// Victim selection is a total order: (reclaim_priority ascending, last_touched
/// ascending, allocation id ascending). Given identical state, the plan is
/// byte-identical. Protected commitments are never selected unless the pool
/// policy explicitly sets allow_protected_reclaim, and pool protected headroom
/// is never a candidate under any policy.
struct BF_API ReclamationPlan {
    PoolId pool{};
    Generation pool_generation{};
    EpochId epoch{};
    u64 target_units{0};
    u64 planned_units{0};
    u64 shortfall_units{0};
    u64 reclaimable_available_units{0};
    u64 pinned_available_units{0};
    bool protected_reclaim_used{false};
    bool truncated{false};
    std::vector<ReclaimVictim> victims{};
    AuthorityVector authority{};
    Tick built_at{0};
    Digest digest{};

    [[nodiscard]] bool complete() const noexcept { return shortfall_units == 0; }
};

struct BF_API ReclaimResult {
    ReclamationPlan plan{};
    u64 reclaimed_units{0};
    u64 revoked_allocations{0};
    u64 borrower_units_returned{0};
    u64 lender_units_returned{0};
    PoolAccounting accounting{};
    Tick applied_at{0};
};

}  // namespace buffer_fabric
