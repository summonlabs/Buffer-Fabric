#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

enum class EventKind : u8 {
    PoolRegistered = 0,
    PoolRemoved = 1,
    PoolResized = 2,
    PolicyPublished = 3,
    QueueRegistered = 4,
    QueueRetired = 5,
    AllocationReserved = 6,
    AllocationCommitted = 7,
    AllocationReleased = 8,
    AllocationReclaimed = 9,
    AllocationFenced = 10,
    AllocationRevalidated = 11,
    AllocationExpired = 12,
    PressureUpdated = 13,
    PressureRejected = 14,
    DecisionRefused = 15,
    EpochAdvanced = 16,
    Recovered = 17,
    AccountingViolation = 18,
    ShutdownStarted = 19,
    ShutdownComplete = 20,
    AttemptConflict = 21,
    BackendEffectSkipped = 22,
};

[[nodiscard]] BF_API std::string_view to_string(EventKind kind) noexcept;

struct BF_API FabricEvent {
    EventKind kind{EventKind::DecisionRefused};
    Tick at{0};
    EpochId epoch{};
    u64 sequence{0};
    PoolId pool{};
    QueueId queue{};
    AllocationId allocation{};
    u64 units{0};
    ErrorCode outcome{ErrorCode::Ok};
    std::string detail{};
};

/// Event sink. Implementations must not call back into the fabric: the fabric
/// invokes sinks strictly after releasing its own state lock, and a sink that
/// re-entered the fabric from the emitting thread could still observe a lock
/// it does not hold, but it would invert ordering with respect to the emitting
/// call. Sinks are therefore documented as leaf callbacks.
class BF_API IEventSink {
public:
    IEventSink() = default;
    virtual ~IEventSink();
    IEventSink(const IEventSink&) = delete;
    IEventSink& operator=(const IEventSink&) = delete;
    virtual void on_event(const FabricEvent& event) = 0;
};

/// Records events in a bounded in-memory ring. Safe for concurrent use.
class BF_API RingEventSink final : public IEventSink {
public:
    explicit RingEventSink(usize capacity = 1024);

    void on_event(const FabricEvent& event) override;
    [[nodiscard]] u64 emitted() const noexcept { return emitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 retained() const noexcept;
    /// Copy the retained events in emission order. Bounded by capacity.
    [[nodiscard]] std::vector<FabricEvent> snapshot() const;
    void clear();

private:
    mutable std::mutex mutex_{};
    std::vector<FabricEvent> events_{};
    usize next_{0};
    usize capacity_{0};
    std::atomic<u64> emitted_{0};
    std::atomic<u64> dropped_{0};
};

/// Lifetime counters. All fields are monotonic except the live gauges, and all
/// are updated without holding the fabric state lock where possible.
struct BF_API FabricMetrics {
    u64 decisions{0};
    u64 grants{0};
    u64 partial_grants{0};
    u64 refusals{0};
    u64 reductions{0};
    u64 fences{0};
    u64 reclaims{0};
    u64 revalidations{0};
    u64 idempotent_replays{0};
    u64 attempt_conflicts{0};
    u64 allocations_created{0};
    u64 allocations_committed{0};
    u64 allocations_released{0};
    u64 allocations_reclaimed{0};
    u64 allocations_fenced{0};
    u64 allocations_expired{0};
    u64 pressure_snapshots_accepted{0};
    u64 pressure_snapshots_rejected{0};
    u64 journal_records_written{0};
    u64 journal_bytes_written{0};
    u64 journal_records_replayed{0};
    u64 journal_tail_truncations{0};
    u64 snapshots_written{0};
    u64 recoveries{0};
    u64 epoch_advances{0};
    u64 backend_effects_attempted{0};
    u64 backend_effects_unsupported{0};
    u64 live_allocations{0};
    u64 live_committed_units{0};
    u64 peak_committed_units{0};
};

}  // namespace buffer_fabric
