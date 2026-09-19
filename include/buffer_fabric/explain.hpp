#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/authority.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/decision.hpp"
#include "buffer_fabric/json.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/pressure.hpp"
#include "buffer_fabric/reclaim.hpp"
#include "buffer_fabric/topology.hpp"

namespace buffer_fabric {

/// Everything the runtime can state about one pool's capacity governance.
///
/// The explanation is a pure function of authoritative state: identical state
/// produces a byte-identical explanation, including the digest. It never
/// contains an unbounded collection; the authority vector is fixed capacity and
/// the queue list is capped at the topology bound.
struct BF_API Explanation {
    PoolId pool{};
    Generation pool_generation{};
    EpochId epoch{};
    BootIncarnation boot{};
    u64 sequence{0};
    Tick generated_at{0};

    u64 raw_units{0};
    u64 protected_headroom_units{0};
    u64 usable_units{0};
    u64 allocated_units{0};
    u64 reserved_units{0};
    u64 committed_units{0};
    u64 pinned_units{0};
    u64 reclaimable_units{0};
    u64 free_units{0};
    u64 borrowed_in_units{0};
    u64 lent_out_units{0};
    u64 overcommit_used_units{0};
    u64 overcommit_limit_units{0};

    PressureState pressure{PressureState::Unknown};
    u32 utilization_bp{0};
    u32 binding_bp{0};
    Thresholds thresholds{};
    PressureEvidence evidence{};

    BindingConstraint binding{BindingConstraint::None};
    CorrectiveIntent intent{};
    AuthorityVector authority{};
    AccountingReport accounting{};
    std::vector<QueueUsage> queues{};

    Digest digest{};

    [[nodiscard]] std::string to_text() const;
    [[nodiscard]] std::string to_json() const;
    void write_json(JsonWriter& writer) const;
};

/// Whole-fabric summary across every registered pool.
struct BF_API FabricSummary {
    EpochId epoch{};
    BootIncarnation boot{};
    u64 pool_count{0};
    u64 queue_count{0};
    u64 allocation_count{0};
    u64 raw_total{0};
    u64 protected_total{0};
    u64 allocated_total{0};
    u64 free_total{0};
    u64 overcommit_total{0};
    u64 borrowed_total{0};
    u64 lent_total{0};
    u64 live_allocations{0};
    AccountingReport accounting{};

    [[nodiscard]] std::string to_json() const;
};

}  // namespace buffer_fabric
