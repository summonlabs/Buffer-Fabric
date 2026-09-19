#pragma once

#include <string>
#include <string_view>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Thresholds
//
// Utilization bands expressed in basis points (1 bp = 0.01%) of the pool's
// authoritative usable capacity. Comparisons are exact: the classifier compares
// used*10000 against band*usable using widenable integer multiplication, so
// there is no rounding and no overflow, even at kMaxUnitsPerPool.
//
// Band semantics (used/usable as a ratio r):
//   r <  elevated  -> CLEAR
//   r >= elevated  -> ELEVATED
//   r >= high      -> HIGH
//   r >= critical  -> CRITICAL
//
// The bands must be strictly increasing and critical must not exceed 10000
// (100.00%). A pool whose usable capacity is zero and whose committed usage is
// zero is CLEAR; a pool with zero usable capacity and non-zero committed usage
// is CRITICAL by definition.
// ---------------------------------------------------------------------------
struct BF_API Thresholds {
    static constexpr u32 kScale = 10000;

    u32 elevated_bp{7000};
    u32 high_bp{8500};
    u32 critical_bp{9500};

    [[nodiscard]] Status validate() const;
    friend bool operator==(const Thresholds&, const Thresholds&) = default;
};

/// Which band a utilization observation landed in.
enum class PressureState : u8 {
    /// No usable evidence exists. Never treated as permissive authority.
    Unknown = 0,
    Clear = 1,
    Elevated = 2,
    High = 3,
    Critical = 4,
    /// Evidence exists but its epoch, generation or age makes it unusable.
    Stale = 5,
};

[[nodiscard]] BF_API std::string_view to_string(PressureState state) noexcept;
[[nodiscard]] BF_API bool pressure_is_known(PressureState state) noexcept;
[[nodiscard]] BF_API bool pressure_at_least(PressureState state, PressureState floor) noexcept;

struct PressureObservation {
    PressureState state{PressureState::Unknown};
    u32 utilization_bp{0};
    u32 binding_bp{0};
    bool overcommitted{false};
};

/// Exact band classification. \p used is committed capacity measured on the
/// pool's own basis (own-raw usage plus lendings). Returns CLEAR/CRITICAL
/// directly when \p usable is zero.
[[nodiscard]] BF_API PressureObservation classify_pressure(const Thresholds& thresholds, u64 used,
                                                           u64 usable, bool overcommitted) noexcept;

/// True when the utilization is at or above \p band_bp.
[[nodiscard]] BF_API bool utilization_at_least(u64 used, u64 usable, u32 band_bp) noexcept;

// ---------------------------------------------------------------------------
// Capacity, borrowing and reclamation policy
// ---------------------------------------------------------------------------

/// How the pool treats requests that would push committed usage past
/// usable capacity. Bounded overcommit is explicit and always limited.
enum class OvercommitMode : u8 {
    Forbid = 0,
    Bounded = 1,
};

/// Whether a pool may draw on its ancestors' free capacity.
enum class BorrowMode : u8 {
    Forbid = 0,
    FromAncestorFree = 1,
};

struct BF_API OvercommitPolicy {
    OvercommitMode mode{OvercommitMode::Forbid};
    u64 limit_units{0};
};

struct BF_API BorrowPolicy {
    BorrowMode mode{BorrowMode::Forbid};
    u64 limit_units{0};
};

struct BF_API PressurePolicy {
    Thresholds thresholds{};
    /// Lifetime of a pressure snapshot in ticks. Snapshots older than this are
    /// STALE, not CLEAR.
    Tick evidence_ttl_ticks{2'000'000'000ull};
    /// When true, an increase in committed usage is refused unless fresh,
    /// epoch-matched, generation-matched evidence is present.
    bool refuse_increase_on_unknown{true};
    /// Advisory corrective intent emitted when the pool reaches HIGH.
    bool reduce_on_high{true};
    /// Advisory corrective intent emitted when the pool reaches CRITICAL.
    bool reduce_on_critical{true};
    /// When true, a HIGH/CRITICAL pool refuses new grants outright instead of
    /// granting down to the free remainder.
    bool refuse_new_grants_above_high{false};
};

struct BF_API ReclaimPolicy {
    bool enabled{true};
    /// An allocation is not eligible for reclamation until it has been
    /// untouched for at least this many ticks.
    Tick min_hold_ticks{0};
    /// Upper bound on units revoked by a single reclaim operation.
    u64 max_units_per_operation{1ull << 40};
    /// Free capacity that must remain after reclamation.
    u64 reserve_units{0};
    /// Protected commitments are only revocable when this is explicitly set.
    bool allow_protected_reclaim{false};
};

// ---------------------------------------------------------------------------
// Pool policy
//
// The complete governing policy for one pool. A policy is an immutable durable
// object identified by PolicyId and revisioned by Generation. Allocations bind
// the exact policy generation that granted them; a policy change invalidates
// dependent allocations unless the change is classified as non-binding.
// ---------------------------------------------------------------------------
struct BF_API PoolPolicy {
    PolicyId id{};
    Generation generation{};
    std::string name{};
    ProvenanceId provenance{};

    // --- capacity governance ---
    /// Committed usage is capped at usable + overcommit allowance.
    OvercommitPolicy overcommit{};
    /// Ancestor borrowing.
    BorrowPolicy borrow{};
    /// Hard cap on the total units any single allocation may hold. 0 = no cap
    /// beyond capacity rules.
    u64 max_single_allocation_units{0};
    /// Hard cap applied to one request before any other rule. 0 = no cap.
    u64 max_single_request_units{0};
    /// When non-zero, a request that would push the requesting queue's total
    /// above this fraction (basis points) of usable capacity is reduced to the
    /// soft mark unless pressure is at most ELEVATED.
    u32 soft_limit_bp{0};
    /// When true a request larger than the available remainder is partially
    /// granted; when false it is refused outright.
    bool allow_partial_grant{true};

    // --- pressure governance ---
    PressurePolicy pressure{};
    /// Pressure state at or above which new grants are refused entirely.
    PressureState refuse_grants_at{PressureState::Critical};
    /// Pressure state at or above which a zero-grant evaluation is reported as
    /// a REDUCE decision with a corrective target.
    PressureState reduce_at{PressureState::High};

    // --- reclamation ---
    ReclaimPolicy reclaim{};
    /// A pool may never be shrunk below this usable capacity.
    u64 usable_floor_units{0};

    // --- generation handling ---
    /// When true, a pool/queue/capacity generation advance fences every
    /// allocation bound to the previous generation.
    bool fence_on_generation_change{true};
    /// Lifetime of a reservation before it is considered abandoned. 0 disables
    /// expiry.
    Tick reservation_ttl_ticks{30'000'000'000ull};
    /// Maximum number of pending reservations per pool.
    u64 max_pending_reservations{4096};

    [[nodiscard]] Status validate() const;
    [[nodiscard]] Digest digest() const;
};

}  // namespace buffer_fabric
