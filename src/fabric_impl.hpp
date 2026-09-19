#pragma once

// Internal state of BufferFabric. Not installed and not part of the public API.

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "buffer_fabric/fabric.hpp"
#include "fs.hpp"
#include "wire.hpp"

namespace buffer_fabric {

/// Per-pool authoritative aggregates. Every field is maintained incrementally
/// under the fabric mutex and is recomputable from the allocation set alone;
/// validate_accounting() performs exactly that recomputation and compares.
struct PoolEntry {
    PoolDescriptor descriptor{};
    Generation capacity_generation{};
    PoolRuntime runtime{};

    u64 reserved_units{0};
    u64 committed_units{0};
    u64 pinned_units{0};
    u64 reclaimable_units{0};
    /// allocated - borrowed_in + lent_out
    u64 own_usage{0};
    u64 borrowed_in{0};
    u64 lent_out{0};

    std::unordered_map<u64, u64> queue_committed{};
    std::unordered_map<u64, u64> queue_reserved{};
    std::unordered_set<u64> members{};

    [[nodiscard]] u64 allocated() const noexcept { return reserved_units + committed_units; }
    [[nodiscard]] u64 usable() const noexcept {
        return descriptor.raw_units >= descriptor.protected_units
                   ? descriptor.raw_units - descriptor.protected_units
                   : 0;
    }
    [[nodiscard]] u64 overcommit_used() const noexcept {
        const u64 usable_units = usable();
        return own_usage > usable_units ? own_usage - usable_units : 0;
    }
    [[nodiscard]] u64 free_units() const noexcept {
        return usable() + overcommit_used() - own_usage;
    }
};

struct CapacityEntry {
    ResourceId resource{};
    BackendId backend{};
    Generation generation{};
    u64 raw_units{0};
    Tick observed_at{0};
};

struct PressureEntry {
    std::deque<PressureSnapshot> history{};
    bool has_latest{false};
    PressureSnapshot latest{};
    bool latest_revalidated{false};
};

struct BackendEntry {
    IBackend* backend{nullptr};
    BackendKind kind{BackendKind::Null};
    Generation generation{};
    bool ready{false};
};

struct BufferFabric::Impl {
    // --- lock discipline ---------------------------------------------------
    // mutex_ guards every authoritative field below. It is a non-recursive
    // std::mutex and is never held while calling out to a backend, an event
    // sink, a clock, or any other user-supplied component:
    //
    //   * the clock is sampled by Lock before the mutex is taken;
    //   * backends are queried before the mutex is taken;
    //   * events are collected into a local vector and published after the
    //     mutex is released (see publish()).
    //
    // There is therefore no path from a caller-supplied component back into
    // the fabric with the mutex already held, and no read-to-write upgrade
    // because only a plain non-recursive mutex is used.
    mutable std::mutex mutex_{};
    std::mutex sink_mutex_{};

    /// Monotonic tick sampled once per public operation, before the mutex is
    /// taken. Every timestamp inside a locked section is read from here.
    mutable Tick tick_{0};

    /// RAII access to authoritative state.
    ///
    /// Member initialisation is ordered, so the clock is sampled before the
    /// mutex is acquired. Any user-supplied IClock is therefore invoked with
    /// no fabric lock held and cannot deadlock by re-entering the fabric.
    class Lock {
    public:
        explicit Lock(Impl& impl)
            : impl_(impl), sampled_(impl.clock().now_ticks()), guard_(impl.mutex_) {
            impl_.tick_ = sampled_;
        }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        Lock(Lock&&) = delete;
        Lock& operator=(Lock&&) = delete;

    private:
        Impl& impl_;
        Tick sampled_;
        std::lock_guard<std::mutex> guard_;
    };

    FabricConfig config_{};
    mutable SteadyClock default_clock_{};
    IClock* clock_{nullptr};
    IEventSink* sink_{nullptr};
    RingEventSink local_sink_{256};

    bool open_{false};
    bool shutting_down_{false};
    EpochId epoch_{};
    BootIncarnation boot_{};

    u64 next_pool_id_{1};
    u64 next_queue_id_{1};
    u64 next_allocation_id_{1};
    u64 next_policy_id_{1};
    u64 next_snapshot_id_{1};
    u64 next_attempt_sequence_{1};
    u64 next_decision_sequence_{1};

    std::unordered_map<u64, BackendEntry> backends_{};
    std::unordered_map<u64, CapacityEntry> capacities_{};
    std::unordered_map<u64, PoolPolicy> policies_{};
    std::unordered_map<std::string, u64> policy_names_{};
    std::unordered_map<u64, PoolEntry> pools_{};
    std::unordered_map<std::string, u64> pool_names_{};
    std::unordered_map<u64, QueueDescriptor> queues_{};
    std::unordered_map<std::string, u64> queue_names_{};
    std::unordered_map<u64, AllocationRecord> allocations_{};
    std::unordered_map<u64, PressureEntry> pressure_{};
    std::unordered_map<u64, AttemptRecord> attempts_{};
    std::deque<u64> attempt_order_{};
    std::deque<DecisionRecord> decisions_{};

    RecoveryReport recovery_{};
    DurableStore store_{};
    FabricMetrics metrics_{};
    u64 allocations_total_{0};

    // --- helpers (all called with mutex_ held) -----------------------------
    /// The tick sampled when the current Lock was taken.
    [[nodiscard]] Tick now() const noexcept { return tick_; }
    [[nodiscard]] IClock& clock() const;
    /// Mint a fresh boot incarnation. Never zero and never equal to a previous
    /// incarnation of this process.
    [[nodiscard]] BootIncarnation mint_boot() noexcept;

    [[nodiscard]] PoolEntry* pool(u64 raw);
    [[nodiscard]] const PoolEntry* pool(u64 raw) const;
    [[nodiscard]] Status require_pool(PoolId id, PoolEntry** out);
    [[nodiscard]] Status require_queue(QueueId id, QueueDescriptor** out);

    [[nodiscard]] PoolAccounting accounting_of(const PoolEntry& entry) const;
    [[nodiscard]] Status check_closure(const PoolEntry& entry) const;

    /// Recompute every aggregate from the allocation set. Destroys nothing.
    void rebuild_aggregates();
    /// Build a scratch copy of the aggregates and compare with the live ones.
    [[nodiscard]] AccountingReport deep_verify() const;

    [[nodiscard]] PressureEvidence resolve_evidence(const PoolEntry& entry) const;
    void record_decision(const Decision& decision);

    /// Attempt de-duplication. Returns:
    ///   Ok          -> proceed (attempt was not seen before, call record_attempt)
    ///   IdempotentReplay handled by the caller through *previous
    [[nodiscard]] Status lookup_attempt(AttemptId id, Digest digest, AttemptRecord* previous,
                                        bool* replay) const;
    void record_attempt(const AttemptRecord& record);
    void evict_attempts();

    /// Capacity arithmetic for one pool.
    struct GrantComputation {
        u64 own_free{0};
        u64 own_overcommit_headroom{0};
        u64 borrowable{0};
        u64 borrow_limit_remaining{0};
        PoolId lender{};
        u64 lender_free{0};
        u64 max_grant{0};
        BindingConstraint binding{BindingConstraint::None};
    };
    [[nodiscard]] GrantComputation compute_grant(const PoolEntry& entry, const PoolPolicy& policy,
                                                 u64 requested) const;

    /// Apply a positive commitment of \p units to \p entry, drawing
    /// \p borrow_units from \p lender.
    [[nodiscard]] Status apply_commit_units(PoolEntry& entry, u64 units, u64 borrow_units,
                                            PoolId lender);
    [[nodiscard]] Status apply_release_units(PoolEntry& entry, const AllocationRecord& record);

    /// Recompute one pool's queue maps entry for \p queue.
    void adjust_queue(PoolEntry& entry, QueueId queue, i64 delta, bool reserved);

    void push_event(std::vector<FabricEvent>& events, EventKind kind, ErrorCode outcome,
                    std::string detail, PoolId pool = {}, QueueId queue = {},
                    AllocationId allocation = {}, u64 units = 0, u64 sequence = 0) const;
    void publish(std::vector<FabricEvent>& events);

    // --- durable mutation --------------------------------------------------
    [[nodiscard]] Status journal(JournalOp op, const void* payload, usize size);
    [[nodiscard]] Status journal_policy(const PoolPolicy& policy);
    [[nodiscard]] Status journal_pool(const PoolEntry& entry);
    [[nodiscard]] Status journal_queue(const QueueDescriptor& queue);
    [[nodiscard]] Status journal_allocation(JournalOp op, const AllocationRecord& record);
    [[nodiscard]] Status journal_capacity(const CapacityEntry& capacity);
    [[nodiscard]] Status journal_attempt(const AttemptRecord& record);
    [[nodiscard]] Status journal_pressure(const PressureSnapshot& snapshot);

    [[nodiscard]] Status load_from_store();
    [[nodiscard]] Status apply_snapshot_payload(const SnapshotHeader& header, const u8* payload,
                                                usize size);
    [[nodiscard]] Status apply_journal_payload(const u8* payload, usize size, u64 ordinal);
    [[nodiscard]] Status write_full_snapshot();

    [[nodiscard]] Status ensure_alloc_capacity() const;

    // --- core operations (mutex_ held) ------------------------------------
    [[nodiscard]] Result<Decision> allocate_locked(const AllocateRequest& request,
                                                   std::vector<FabricEvent>& events);
    [[nodiscard]] Result<Decision> commit_locked(const CommitRequest& request,
                                                 std::vector<FabricEvent>& events);
    [[nodiscard]] Result<Decision> release_locked(const ReleaseRequest& request,
                                                  std::vector<FabricEvent>& events);
    [[nodiscard]] Result<Decision> revalidate_locked(const RevalidateRequest& request,
                                                     std::vector<FabricEvent>& events);
    [[nodiscard]] Result<Decision> fence_stale_locked(PoolId pool,
                                                      std::vector<FabricEvent>& events);
    [[nodiscard]] Result<Decision> expire_reservations_locked(PoolId pool,
                                                              std::vector<FabricEvent>& events);
    [[nodiscard]] Result<ReclamationPlan> plan_reclaim_locked(const ReclaimRequest& request,
                                                              std::vector<FabricEvent>& events);
    [[nodiscard]] Result<ReclaimResult> apply_reclaim_locked(const ReclaimRequest& request,
                                                             std::vector<FabricEvent>& events);
    [[nodiscard]] Status fence_allocation_locked(AllocationRecord& record,
                                                 AllocationState new_state, ErrorCode reason,
                                                 std::vector<FabricEvent>& events);

    /// Fence (or reduce) allocations so that \p entry fits its capacity again.
    /// Returns the number of units returned to free capacity.
    struct ShrinkResult {
        u64 fenced_allocations{0};
        u64 fenced_units{0};
        u64 reduced_allocations{0};
        u64 reduced_units{0};
        u64 borrow_returned_units{0};
    };
    [[nodiscard]] Status enforce_capacity(PoolEntry& entry, bool fence_everything,
                                          ShrinkResult* result,
                                          std::vector<FabricEvent>& events);
    [[nodiscard]] std::vector<u64> sorted_members(const PoolEntry& entry) const;
};

}  // namespace buffer_fabric
