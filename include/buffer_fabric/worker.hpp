#pragma once

#include <string>

#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// A small accounting view returned by a coordinator response.
struct BF_API AccountingReportLike {
    bool closed{false};
    u64 raw_total{0};
    u64 protected_total{0};
    u64 allocated_total{0};
    u64 free_total{0};
    u64 borrowed_total{0};
    u64 lent_total{0};
    u64 overcommit_total{0};
    u64 pools_observed{0};
};

struct BF_API WorkerConfig {
    std::string host{"127.0.0.1"};
    u16 port{0};
    PoolId pool{};
    QueueId queue{};
    u64 units{64};
    u64 operations{64};
    /// Epoch the worker claims at handshake. Zero means "I hold no epoch".
    EpochId claim_epoch{};
    /// Epoch asserted on every subsequent request. Zero means "not asserted".
    EpochId expected_epoch{};
    /// Base offset for this worker's attempt identifiers. Attempt identities
    /// are fabric-wide, so concurrent workers must not share a range.
    u64 attempt_base{0};
};

struct BF_API WorkerReport {
    bool connected{false};
    bool handshake_ok{false};
    bool stale_epoch_rejected{false};
    bool protocol_error{false};
    u64 operations_attempted{0};
    u64 grants{0};
    u64 partial_grants{0};
    u64 refusals{0};
    u64 release_ok{0};
    u64 error_responses{0};
    u64 granted_units{0};
    u64 released_units{0};
    u64 bytes_sent{0};
    u64 bytes_received{0};
    EpochId coordinator_epoch{};
    BootIncarnation coordinator_boot{};
    AccountingReportLike accounting{};
};

/// Connect to a coordinator, perform a handshake, then run
/// allocate/release cycles. Returns a report describing exactly what happened;
/// an epoch rejection is reported, not treated as a transport failure.
[[nodiscard]] BF_API Result<WorkerReport> run_worker(const WorkerConfig& config);

}  // namespace buffer_fabric
