#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/topology.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

// ---------------------------------------------------------------------------
// Accounting
//
// Per pool the fabric maintains one exact integer identity:
//
//   raw + borrowed_in + overcommit_used
//       == protected + allocated + free + lent_out
//
// where
//   raw             authoritative raw capacity of the pool's resource share
//   protected       protected headroom, never allocatable by general claimants
//   allocated       reserved + committed units held by allocations owned by
//                   this pool, including any borrowed portion
//   free            unallocated, unreserved units under the pool's authority
//   borrowed_in     portion of allocated drawn from ancestors' free capacity
//   lent_out        units of this pool's free capacity committed to descendants
//   overcommit_used excess of own-capacity usage over usable capacity, granted
//                   only under an explicit bounded overcommit policy
//
// with the derived helpers
//   usable          raw - protected
//   own_usage       allocated - borrowed_in + lent_out
//   overcommit_used max(0, own_usage - usable)
//   free            usable + overcommit_used - own_usage
//
// The identity holds exactly, in unsigned 64-bit arithmetic, for every
// reachable state. Two further invariants follow and are checked separately:
//
//   free == 0  or  overcommit_used == 0      (no free capacity while overcommitted)
//   allocated == reserved + committed == pinned + reclaimable
//
// Globally, across all pools:
//
//   sum(borrowed_in) == sum(lent_out)        (every lending has a borrower)
//   sum(raw) + sum(overcommit_used) == sum(protected) + sum(allocated) + sum(free)
// ---------------------------------------------------------------------------
struct BF_API PoolAccounting {
    PoolId pool{};
    Generation pool_generation{};
    u64 raw{0};
    u64 protected_units{0};
    u64 usable{0};
    u64 allocated{0};
    u64 reserved{0};
    u64 committed{0};
    u64 pinned{0};
    u64 reclaimable{0};
    u64 borrowed_in{0};
    u64 lent_out{0};
    u64 overcommit_used{0};
    u64 overcommit_limit{0};
    u64 free{0};

    [[nodiscard]] u64 own_usage() const noexcept;
    [[nodiscard]] u64 closure_rhs() const noexcept;
    [[nodiscard]] u64 closure_lhs() const noexcept;
};

enum class AccountingFault : u8 {
    None = 0,
    PoolClosureMismatch = 1,
    FreeWithOvercommit = 2,
    AllocationSplitMismatch = 3,
    PinnedReclaimableMismatch = 4,
    OvercommitWithoutPolicy = 5,
    OvercommitBeyondLimit = 6,
    ProtectedExceedsRaw = 7,
    UsableMismatch = 8,
    GlobalBorrowMismatch = 9,
    GlobalClosureMismatch = 10,
    FreeAboveUsable = 11,
    OwnUsageAboveAllowance = 12,
    SaturatedField = 13,
};

[[nodiscard]] BF_API std::string_view to_string(AccountingFault fault) noexcept;

struct BF_API AccountingViolation {
    AccountingFault fault{AccountingFault::None};
    PoolId pool{};
    std::string detail{};
};

struct BF_API AccountingReport {
    bool closed{true};
    u64 pools_checked{0};
    u64 raw_total{0};
    u64 protected_total{0};
    u64 allocated_total{0};
    u64 free_total{0};
    u64 borrowed_total{0};
    u64 lent_total{0};
    u64 overcommit_total{0};
    std::vector<AccountingViolation> violations{};

    [[nodiscard]] bool ok() const noexcept { return closed; }
    [[nodiscard]] Status status() const;
};

[[nodiscard]] BF_API AccountingReport check_pool_accounting(const PoolAccounting& accounting,
                                                            OvercommitMode mode,
                                                            u64 overcommit_limit);

[[nodiscard]] BF_API AccountingReport check_global_accounting(const std::vector<PoolAccounting>& pools);

[[nodiscard]] BF_API std::string format_pool_accounting(const PoolAccounting& accounting);

}  // namespace buffer_fabric
