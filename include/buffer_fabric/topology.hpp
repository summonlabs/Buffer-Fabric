#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Pool topology
//
// A pool is a named partition of one authoritative resource's raw buffer
// capacity. Pools form a forest; a pool may borrow from the free capacity of
// its ancestors when its policy allows it. Pool identity is (PoolId,
// Generation): every mutation that changes the descriptor's meaning advances
// the generation and invalidates allocations bound to the previous one.
// ---------------------------------------------------------------------------
struct BF_API PoolDescriptor {
    PoolId id{};
    Generation generation{};
    std::string name{};
    /// Authoritative resource this pool partitions.
    ResourceId resource{};
    /// Backend that vouches for the resource's raw capacity.
    BackendId backend{};
    /// Authoritative raw capacity, in buffer units.
    u64 raw_units{0};
    /// Protected headroom: reserved, never allocatable by general claimants.
    u64 protected_units{0};
    /// Optional parent pool for borrowing. Invalid id means a root pool.
    PoolId parent{};
    /// Policy governing this pool.
    PolicyId policy{};
    TenantId tenant{};
    ProvenanceId provenance{};
    Tick created_at{0};
    /// Retired pools accept no new allocations; existing ones are fenced.
    bool retired{false};

    [[nodiscard]] Status validate() const;
    [[nodiscard]] u64 usable_units() const noexcept {
        return raw_units >= protected_units ? raw_units - protected_units : 0;
    }
};

/// Mutable accounting state maintained alongside a descriptor. Every field is
/// derived by the fabric; none of it is accepted from callers.
struct BF_API PoolRuntime {
    u64 borrowed_in{0};
    u64 lent_out{0};
    u64 overcommit_used{0};
    u64 free_units{0};
    u64 reserved_units{0};
    u64 committed_units{0};
    u64 pinned_units{0};
    u64 reclaimable_units{0};
    u64 grant_count{0};
    u64 refuse_count{0};
    u64 reclaim_count{0};
    u64 fenced_count{0};
    u64 peak_committed_units{0};
    Tick last_decision_at{0};
};

// ---------------------------------------------------------------------------
// Queue binding
//
// Queue lifecycle, scheduling and packet ownership live outside this runtime.
// The fabric ingests the queue's *authoritative demand and generation* from its
// owner, binds allocations to that exact generation, and reports the
// consequences when the generation changes or the queue retires. The fabric
// does not create, destroy, schedule or pace queues.
// ---------------------------------------------------------------------------
struct BF_API QueueDescriptor {
    QueueId id{};
    Generation generation{};
    std::string name{};
    PoolId pool{};
    TenantId tenant{};
    ClassId traffic_class{};
    /// Authoritative outstanding demand reported by the queue's owner.
    u64 demand_units{0};
    /// Relative share weight. 0 means "no reserved share".
    u64 weight{1};
    /// Hard cap on total committed units for this queue. 0 = unlimited.
    u64 max_committed_units{0};
    Tick registered_at{0};
    Tick demand_updated_at{0};
    /// Retired queues keep their identity and generation for lineage but accept
    /// no new allocations.
    bool retired{false};
};

/// Resolved view of one queue's committed usage inside one pool.
struct BF_API QueueUsage {
    QueueId queue{};
    PoolId pool{};
    Generation queue_generation{};
    u64 committed_units{0};
    u64 reserved_units{0};
    u64 allocation_count{0};
};

/// Snapshot of one pool's topology and accounting, used for explanation and
/// for cross-process inspection.
struct BF_API PoolView {
    PoolDescriptor descriptor{};
    PoolPolicy policy{};
    PoolRuntime runtime{};
    std::vector<QueueUsage> queues{};
    u64 raw_units{0};
    u64 protected_units{0};
    u64 usable_units{0};
    u64 allocated_units{0};
    u64 free_units{0};
};

}  // namespace buffer_fabric
