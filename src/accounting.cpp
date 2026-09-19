#include "buffer_fabric/accounting.hpp"

#include <cstdio>

namespace buffer_fabric {
namespace {

void add_violation(AccountingReport* report, AccountingFault fault, PoolId pool,
                   std::string detail) {
    report->closed = false;
    AccountingViolation violation;
    violation.fault = fault;
    violation.pool = pool;
    violation.detail = std::move(detail);
    if (report->violations.size() < 64) {
        report->violations.push_back(std::move(violation));
    }
}

[[nodiscard]] std::string u64_text(const char* label, u64 value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s=%llu", label, static_cast<unsigned long long>(value));
    return std::string(buffer);
}

}  // namespace

std::string_view to_string(AccountingFault fault) noexcept {
    switch (fault) {
        case AccountingFault::None: return "NONE";
        case AccountingFault::PoolClosureMismatch: return "POOL_CLOSURE_MISMATCH";
        case AccountingFault::FreeWithOvercommit: return "FREE_WITH_OVERCOMMIT";
        case AccountingFault::AllocationSplitMismatch: return "ALLOCATION_SPLIT_MISMATCH";
        case AccountingFault::PinnedReclaimableMismatch: return "PINNED_RECLAIMABLE_MISMATCH";
        case AccountingFault::OvercommitWithoutPolicy: return "OVERCOMMIT_WITHOUT_POLICY";
        case AccountingFault::OvercommitBeyondLimit: return "OVERCOMMIT_BEYOND_LIMIT";
        case AccountingFault::ProtectedExceedsRaw: return "PROTECTED_EXCEEDS_RAW";
        case AccountingFault::UsableMismatch: return "USABLE_MISMATCH";
        case AccountingFault::GlobalBorrowMismatch: return "GLOBAL_BORROW_MISMATCH";
        case AccountingFault::GlobalClosureMismatch: return "GLOBAL_CLOSURE_MISMATCH";
        case AccountingFault::FreeAboveUsable: return "FREE_ABOVE_USABLE";
        case AccountingFault::OwnUsageAboveAllowance: return "OWN_USAGE_ABOVE_ALLOWANCE";
        case AccountingFault::SaturatedField: return "SATURATED_FIELD";
    }
    return "UNKNOWN";
}

u64 PoolAccounting::own_usage() const noexcept {
    const u64 base = allocated >= borrowed_in ? allocated - borrowed_in : 0;
    u64 sum = 0;
    if (add_overflow(base, lent_out, &sum)) return (std::numeric_limits<u64>::max)();
    return sum;
}

u64 PoolAccounting::closure_lhs() const noexcept {
    u64 sum = 0;
    if (add_overflow(raw, borrowed_in, &sum)) return (std::numeric_limits<u64>::max)();
    if (add_overflow(sum, overcommit_used, &sum)) return (std::numeric_limits<u64>::max)();
    return sum;
}

u64 PoolAccounting::closure_rhs() const noexcept {
    u64 sum = 0;
    if (add_overflow(protected_units, allocated, &sum)) return (std::numeric_limits<u64>::max)();
    if (add_overflow(sum, free, &sum)) return (std::numeric_limits<u64>::max)();
    if (add_overflow(sum, lent_out, &sum)) return (std::numeric_limits<u64>::max)();
    return sum;
}

Status AccountingReport::status() const {
    if (closed) return Status::success();
    std::string detail = "accounting closure failed";
    if (!violations.empty()) {
        detail.append(": ");
        detail.append(std::string(to_string(violations.front().fault)));
        if (!violations.front().detail.empty()) {
            detail.append(" (");
            detail.append(violations.front().detail);
            detail.append(")");
        }
    }
    return Status(ErrorCode::AccountingViolation, detail);
}

AccountingReport check_pool_accounting(const PoolAccounting& a, OvercommitMode mode,
                                       u64 overcommit_limit) {
    AccountingReport report;
    report.pools_checked = 1;
    report.raw_total = a.raw;
    report.protected_total = a.protected_units;
    report.allocated_total = a.allocated;
    report.free_total = a.free;
    report.borrowed_total = a.borrowed_in;
    report.lent_total = a.lent_out;
    report.overcommit_total = a.overcommit_used;

    if (a.protected_units > a.raw) {
        add_violation(&report, AccountingFault::ProtectedExceedsRaw, a.pool,
                      u64_text("protected", a.protected_units) + " " + u64_text("raw", a.raw));
    }
    const u64 expected_usable = a.raw >= a.protected_units ? a.raw - a.protected_units : 0;
    if (a.usable != expected_usable) {
        add_violation(&report, AccountingFault::UsableMismatch, a.pool,
                      u64_text("declared", a.usable) + " " + u64_text("expected", expected_usable));
    }

    u64 split = 0;
    if (add_overflow(a.reserved, a.committed, &split)) {
        add_violation(&report, AccountingFault::SaturatedField, a.pool, "reserved + committed");
    } else if (split != a.allocated) {
        add_violation(&report, AccountingFault::AllocationSplitMismatch, a.pool,
                      u64_text("reserved+committed", split) + " " + u64_text("allocated", a.allocated));
    }

    u64 classes = 0;
    if (add_overflow(a.pinned, a.reclaimable, &classes)) {
        add_violation(&report, AccountingFault::SaturatedField, a.pool, "pinned + reclaimable");
    } else if (classes != a.allocated) {
        add_violation(&report, AccountingFault::PinnedReclaimableMismatch, a.pool,
                      u64_text("pinned+reclaimable", classes) + " " +
                          u64_text("allocated", a.allocated));
    }

    u64 lhs = 0;
    u64 rhs = 0;
    bool saturated = false;
    if (add_overflow(a.raw, a.borrowed_in, &lhs) || add_overflow(lhs, a.overcommit_used, &lhs)) {
        saturated = true;
    }
    if (add_overflow(a.protected_units, a.allocated, &rhs) || add_overflow(rhs, a.free, &rhs) ||
        add_overflow(rhs, a.lent_out, &rhs)) {
        saturated = true;
    }
    if (saturated) {
        add_violation(&report, AccountingFault::SaturatedField, a.pool, "closure side saturated");
    } else if (lhs != rhs) {
        add_violation(&report, AccountingFault::PoolClosureMismatch, a.pool,
                      u64_text("lhs", lhs) + " " + u64_text("rhs", rhs));
    }

    if (a.free != 0 && a.overcommit_used != 0) {
        add_violation(&report, AccountingFault::FreeWithOvercommit, a.pool,
                      u64_text("free", a.free) + " " + u64_text("overcommit", a.overcommit_used));
    }
    if (a.overcommit_used != 0 && mode != OvercommitMode::Bounded) {
        add_violation(&report, AccountingFault::OvercommitWithoutPolicy, a.pool,
                      u64_text("overcommit", a.overcommit_used));
    }
    if (a.overcommit_used > overcommit_limit) {
        add_violation(&report, AccountingFault::OvercommitBeyondLimit, a.pool,
                      u64_text("overcommit", a.overcommit_used) + " " +
                          u64_text("limit", overcommit_limit));
    }
    u64 allowance = 0;
    if (add_overflow(a.usable, a.overcommit_used, &allowance)) {
        allowance = (std::numeric_limits<u64>::max)();
    }
    if (a.free > allowance) {
        add_violation(&report, AccountingFault::FreeAboveUsable, a.pool,
                      u64_text("free", a.free) + " " + u64_text("allowance", allowance));
    }
    const u64 own = a.own_usage();
    u64 max_own = 0;
    if (add_overflow(a.usable, overcommit_limit, &max_own)) {
        max_own = (std::numeric_limits<u64>::max)();
    }
    if (own > max_own) {
        add_violation(&report, AccountingFault::OwnUsageAboveAllowance, a.pool,
                      u64_text("own_usage", own) + " " + u64_text("allowance", max_own));
    }
    return report;
}

AccountingReport check_global_accounting(const std::vector<PoolAccounting>& pools) {
    AccountingReport report;
    for (const auto& pool : pools) {
        AccountingReport single = check_pool_accounting(pool, OvercommitMode::Bounded,
                                                        pool.overcommit_limit);
        report.closed = report.closed && single.closed;
        for (auto& violation : single.violations) {
            if (report.violations.size() < 64) report.violations.push_back(std::move(violation));
        }
        report.pools_checked += 1;
        u64 next = 0;
        bool ok = true;
        ok = ok && !add_overflow(report.raw_total, pool.raw, &next);
        report.raw_total = next;
        ok = ok && !add_overflow(report.protected_total, pool.protected_units, &next);
        report.protected_total = next;
        ok = ok && !add_overflow(report.allocated_total, pool.allocated, &next);
        report.allocated_total = next;
        ok = ok && !add_overflow(report.free_total, pool.free, &next);
        report.free_total = next;
        ok = ok && !add_overflow(report.borrowed_total, pool.borrowed_in, &next);
        report.borrowed_total = next;
        ok = ok && !add_overflow(report.lent_total, pool.lent_out, &next);
        report.lent_total = next;
        ok = ok && !add_overflow(report.overcommit_total, pool.overcommit_used, &next);
        report.overcommit_total = next;
        if (!ok) {
            add_violation(&report, AccountingFault::SaturatedField, pool.pool, "global totals");
        }
    }

    if (report.borrowed_total != report.lent_total) {
        add_violation(&report, AccountingFault::GlobalBorrowMismatch, PoolId{},
                      u64_text("borrowed", report.borrowed_total) + " " +
                          u64_text("lent", report.lent_total));
    }

    u64 lhs = 0;
    u64 rhs = 0;
    const bool lhs_ok = !add_overflow(report.raw_total, report.overcommit_total, &lhs);
    const bool rhs_ok = !add_overflow(report.protected_total, report.allocated_total, &rhs) &&
                        !add_overflow(rhs, report.free_total, &rhs);
    if (!lhs_ok || !rhs_ok) {
        add_violation(&report, AccountingFault::SaturatedField, PoolId{}, "global closure");
    } else if (lhs != rhs) {
        add_violation(&report, AccountingFault::GlobalClosureMismatch, PoolId{},
                      u64_text("lhs", lhs) + " " + u64_text("rhs", rhs));
    }
    return report;
}

std::string format_pool_accounting(const PoolAccounting& a) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "pool=%llu gen=%llu raw=%llu protected=%llu usable=%llu allocated=%llu "
                  "reserved=%llu committed=%llu pinned=%llu reclaimable=%llu free=%llu "
                  "borrowed_in=%llu lent_out=%llu overcommit=%llu limit=%llu",
                  static_cast<unsigned long long>(a.pool.raw()),
                  static_cast<unsigned long long>(a.pool_generation.raw()),
                  static_cast<unsigned long long>(a.raw),
                  static_cast<unsigned long long>(a.protected_units),
                  static_cast<unsigned long long>(a.usable),
                  static_cast<unsigned long long>(a.allocated),
                  static_cast<unsigned long long>(a.reserved),
                  static_cast<unsigned long long>(a.committed),
                  static_cast<unsigned long long>(a.pinned),
                  static_cast<unsigned long long>(a.reclaimable),
                  static_cast<unsigned long long>(a.free),
                  static_cast<unsigned long long>(a.borrowed_in),
                  static_cast<unsigned long long>(a.lent_out),
                  static_cast<unsigned long long>(a.overcommit_used),
                  static_cast<unsigned long long>(a.overcommit_limit));
    return std::string(buffer);
}

}  // namespace buffer_fabric
