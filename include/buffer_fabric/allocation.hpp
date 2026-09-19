#pragma once

#include <array>
#include <string>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Maximum depth of a derived allocation's lineage chain.
inline constexpr u32 kMaxLineageDepth = 32;

/// Bits carried forward from ancestors when an allocation is derived.
struct BF_API Lineage {
    /// The first allocation in this chain.
    AllocationId origin{};
    /// The immediate ancestor, invalid when this allocation is a root.
    AllocationId parent{};
    /// Number of derivation steps from the origin. 0 for a root.
    u32 depth{0};
    /// Digest over (origin, parent, depth, units, pool, queue) for each step.
    Digest chain_digest{};
};

enum class AllocationState : u8 {
    /// Units are deducted from free capacity but the allocation is not yet
    /// authoritative and is not counted as committed usage.
    Reserved = 0,
    /// Authoritative allocation. Counted in committed units.
    Committed = 1,
    /// Invalidated because a bound generation advanced. The units were returned
    /// to free capacity; the record is retained for lineage and audit. A fenced
    /// allocation can be revalidated only if capacity is available again.
    Fenced = 2,
    /// Revoked by reclamation. Units returned to free capacity.
    Reclaimed = 3,
    /// Released by its owner. Units returned to free capacity.
    Released = 4,
    /// Expired without ever being committed. Units returned to free capacity.
    Expired = 5,
};

[[nodiscard]] BF_API std::string_view to_string(AllocationState state) noexcept;
[[nodiscard]] BF_API bool allocation_holds_units(AllocationState state) noexcept;
[[nodiscard]] BF_API bool allocation_is_terminal(AllocationState state) noexcept;

/// Whether an allocation may be reclaimed. This is a separate axis from pool
/// protected headroom: a pinned allocation is a commitment the claimant asked
/// the fabric to hold.
enum class ReclaimClass : u8 {
    /// Never revoked by reclamation unless policy explicitly permits it.
    Pinned = 0,
    /// Eligible for reclamation under policy.
    Reclaimable = 1,
};

[[nodiscard]] BF_API std::string_view to_string(ReclaimClass klass) noexcept;

/// The exact generations an allocation was granted under. Any advance in the
/// bound generations invalidates the allocation unless the fabric classifies
/// the change as non-binding for that allocation.
struct BF_API BoundGenerations {
    Generation pool_generation{};
    Generation queue_generation{};
    Generation policy_generation{};
    Generation capacity_generation{};
    Generation backend_generation{};
    EpochId epoch{};
    BootIncarnation boot{};

    friend bool operator==(const BoundGenerations&, const BoundGenerations&) = default;
    [[nodiscard]] Digest digest() const;
};

struct BF_API AllocationRecord {
    AllocationId id{};
    PoolId pool{};
    QueueId queue{};
    TenantId tenant{};
    ClassId traffic_class{};
    u64 units{0};
    AllocationState state{AllocationState::Reserved};
    ReclaimClass reclaim_class{ReclaimClass::Reclaimable};
    /// Lower value is reclaimed first.
    u64 reclaim_priority{0};
    Lineage lineage{};
    BoundGenerations bound{};
    AttemptId attempt{};
    ProvenanceId provenance{};
    /// Portion of \ref units drawn from ancestors rather than the pool's own
    /// raw capacity.
    u64 borrow_units{0};
    /// Ancestor that lent \ref borrow_units. Invalid when borrow_units is 0.
    PoolId lender{};
    Tick created_at{0};
    Tick last_touched{0};
    /// 0 means no expiry.
    Tick expires_at{0};
    /// Per-allocation mutation sequence, incremented on every accepted
    /// mutation. Used to make duplicate attempts observable.
    u64 revision{0};

    [[nodiscard]] bool holds_units() const noexcept { return allocation_holds_units(state); }
    [[nodiscard]] u64 units_from_own_capacity() const noexcept {
        return units >= borrow_units ? units - borrow_units : 0;
    }
};

// ---------------------------------------------------------------------------
// Attempts
//
// Every mutating call carries an AttemptId plus a digest of its canonical
// request. Replaying the same attempt with the same digest is idempotent and
// returns the recorded outcome; reusing an attempt id with a different digest
// is a conflict and is refused.
// ---------------------------------------------------------------------------
enum class AttemptKind : u8 {
    Allocate = 0,
    Commit = 1,
    Release = 2,
    Reclaim = 3,
    Fence = 4,
    Revalidate = 5,
    Reserve = 6,
};

[[nodiscard]] BF_API std::string_view to_string(AttemptKind kind) noexcept;

struct BF_API AttemptRecord {
    AttemptId id{};
    AttemptKind kind{AttemptKind::Allocate};
    Digest request_digest{};
    ErrorCode outcome{ErrorCode::Ok};
    AllocationId allocation{};
    u64 granted_units{0};
    u64 sequence{0};
    Tick recorded_at{0};
    /// True when the attempt's durable intent was journaled but the operation
    /// never reached its commit boundary (for example after a crash).
    bool unfinished{false};
};

}  // namespace buffer_fabric
