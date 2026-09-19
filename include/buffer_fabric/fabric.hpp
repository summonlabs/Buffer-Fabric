#pragma once

#include <memory>
#include <string>
#include <vector>

#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/backend.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/decision.hpp"
#include "buffer_fabric/explain.hpp"
#include "buffer_fabric/observation.hpp"
#include "buffer_fabric/persistence.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/pressure.hpp"
#include "buffer_fabric/reclaim.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/topology.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Inputs required to bring a pool into existence.
struct BF_API PoolRegistration {
    /// Invalid id requests automatic allocation from the fabric's id space.
    PoolId id{};
    std::string name{};
    ResourceId resource{};
    BackendId backend{};
    u64 raw_units{0};
    u64 protected_units{0};
    PoolId parent{};
    PolicyId policy{};
    TenantId tenant{};
    ProvenanceId provenance{};
};

struct BF_API QueueRegistration {
    QueueId id{};
    std::string name{};
    PoolId pool{};
    TenantId tenant{};
    ClassId traffic_class{};
    u64 demand_units{0};
    u64 weight{1};
    u64 max_committed_units{0};
};

/// Result of a pool resize, including the exact consequences for allocations
/// that were bound to the previous capacity generation.
struct BF_API ResizeOutcome {
    Generation previous_generation{};
    Generation new_generation{};
    u64 previous_raw_units{0};
    u64 new_raw_units{0};
    u64 previous_protected_units{0};
    u64 new_protected_units{0};
    u64 fenced_allocations{0};
    u64 fenced_units{0};
    u64 reduced_allocations{0};
    u64 reduced_units{0};
    u64 borrow_returned_units{0};
    bool refused{false};
    std::string reason{};
    PoolAccounting accounting{};
};

/// Policy over one pool whose generation must be revalidated.
struct BF_API PoolGenerationStatus {
    PoolId pool{};
    Generation pool_generation{};
    Generation policy_generation{};
    Generation capacity_generation{};
    u64 allocations_bound{0};
    u64 allocations_stale{0};
    bool policy_bound{false};
};

struct BF_API FabricConfig {
    /// Durability settings. DurabilityMode::None keeps everything in memory and
    /// makes no durability claim.
    PersistenceConfig persistence{};

    /// Number of decisions retained for explanation.
    u64 decision_history_capacity{4096};
    /// Number of attempts retained for de-duplication.
    u64 attempt_memory_capacity{kMaxAttemptMemory};
    /// Verify the accounting identity after every accepted mutation. Disabling
    /// this removes the runtime guarantee and is not recommended.
    bool verify_accounting_on_mutation{true};
    /// Reject requests whose declared generations do not match current state.
    bool strict_generation_binding{true};
    /// Refuse mutations once shutdown has begun.
    bool refuse_work_after_shutdown{true};
    /// Maximum number of pressure snapshots retained per pool.
    u64 pressure_history_capacity{64};
    /// Maximum number of allocations returned by one inspection call.
    u64 max_inspection_rows{65536};
    /// When true, an allocation whose queue has retired is fenced automatically.
    bool auto_fence_on_queue_retirement{true};
    /// When true, publishing capacity authority asks the attached backend to
    /// program the corresponding physical reservation. A backend that owns no
    /// programmable memory answers Unsupported, which is recorded and
    /// tolerated; any other backend failure refuses the publication. When
    /// false the runtime never touches the backend's programming hooks.
    bool program_backend_effects{false};
};

/// Bounded counters describing the convergence of the fabric's own journal.
struct BF_API FabricStatistics {
    u64 pools{0};
    u64 policies{0};
    u64 queues{0};
    u64 allocations_live{0};
    u64 allocations_total{0};
    u64 decisions{0};
    u64 attempts_retained{0};
    u64 pressure_snapshots{0};
    u64 durable_records{0};
    u64 durable_bytes{0};
};

// ---------------------------------------------------------------------------
// BufferFabric
//
// The authoritative runtime. All public methods are safe to call concurrently
// from multiple threads. Internally a single non-recursive mutex guards
// authoritative state; no callback, event sink or user-supplied component is
// ever invoked while that mutex is held, so there is no lock-reentrancy path
// from a callback back into the fabric and no read-to-write lock upgrade.
// ---------------------------------------------------------------------------
class BF_API BufferFabric {
public:
    /// Opaque implementation type. It is declared here so that the
    /// implementation translation units can define its members out of line;
    /// it is never defined in a public header and is not part of the ABI.
    struct Impl;

    BufferFabric();
    ~BufferFabric();
    BufferFabric(const BufferFabric&) = delete;
    BufferFabric& operator=(const BufferFabric&) = delete;
    BufferFabric(BufferFabric&&) = delete;
    BufferFabric& operator=(BufferFabric&&) = delete;

    // --- lifecycle -------------------------------------------------------
    /// Open the fabric. When persistence is enabled the durable image is
    /// recovered, the fabric epoch is advanced, and a fresh boot incarnation is
    /// minted. Any authority bound to the previous epoch or boot incarnation is
    /// reported as stale and is not restored as live.
    [[nodiscard]] Status open(const FabricConfig& config);
    [[nodiscard]] VoidResult close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] EpochId epoch() const noexcept;
    [[nodiscard]] BootIncarnation boot_incarnation() const noexcept;
    [[nodiscard]] RecoveryReport recovery_report() const;

    /// Stop accepting work, refuse new mutations, and leave committed state
    /// intact. Idempotent.
    void begin_shutdown() noexcept;
    [[nodiscard]] bool shutting_down() const noexcept;

    // --- backends --------------------------------------------------------
    [[nodiscard]] VoidResult attach_backend(IBackend* backend);
    [[nodiscard]] u64 backend_count() const;

    /// Publish capacity authority for a resource. The backend is consulted for
    /// the authoritative raw capacity; a backend that is not ready, or that
    /// does not know the resource, yields UNKNOWN and the call is refused.
    [[nodiscard]] Result<Generation> set_capacity(ResourceId resource, BackendId backend,
                                                  u64 expected_raw_units, Generation expected);

    // --- policy ----------------------------------------------------------
    /// Publish a policy. The generation is assigned by the fabric; a
    /// re-published identical policy returns the existing generation without
    /// advancing it.
    [[nodiscard]] Result<Generation> publish_policy(const PoolPolicy& policy);
    [[nodiscard]] Result<PoolPolicy> policy(PolicyId id) const;

    // --- topology --------------------------------------------------------
    [[nodiscard]] Result<PoolId> register_pool(const PoolRegistration& registration);
    [[nodiscard]] Status remove_pool(PoolId pool, Generation expected_pool_generation);
    [[nodiscard]] Result<ResizeOutcome> resize_pool(PoolId pool, u64 new_raw_units,
                                                    u64 new_protected_units,
                                                    Generation expected_pool_generation);
    [[nodiscard]] Result<QueueId> register_queue(const QueueRegistration& registration);
    [[nodiscard]] Status retarget_queue(QueueId queue, PoolId new_pool,
                                        Generation expected_queue_generation);
    [[nodiscard]] Result<Generation> update_queue_demand(QueueId queue, u64 demand_units,
                                                         Generation expected_queue_generation);
    [[nodiscard]] Status retire_queue(QueueId queue, Generation expected_queue_generation);
    [[nodiscard]] Result<PoolGenerationStatus> pool_generation_status(PoolId pool) const;

    // --- pressure --------------------------------------------------------
    /// Ingest a pressure observation. The snapshot is accepted for storage only
    /// when its pool, generation and epoch are structurally valid; freshness is
    /// evaluated at decision time, never at ingestion time.
    [[nodiscard]] Status observe_pressure(const PressureSnapshot& snapshot);
    [[nodiscard]] Result<PressureEvidence> pressure(PoolId pool) const;

    // --- allocation ------------------------------------------------------
    [[nodiscard]] Result<Decision> allocate(const AllocateRequest& request);
    [[nodiscard]] Result<Decision> commit(const CommitRequest& request);
    [[nodiscard]] Result<Decision> release(const ReleaseRequest& request);
    [[nodiscard]] Result<Decision> revalidate(const RevalidateRequest& request);
    [[nodiscard]] Result<EvaluationResult> evaluate(const EvaluateRequest& request) const;

    /// Fence every allocation in the pool that is bound to a generation older
    /// than current state. Fenced units are returned to free capacity and the
    /// records are retained for lineage.
    [[nodiscard]] Result<Decision> fence_stale(PoolId pool);

    /// Expire reservations whose reservation TTL elapsed.
    [[nodiscard]] Result<Decision> expire_reservations(PoolId pool);

    // --- reclamation -----------------------------------------------------
    [[nodiscard]] Result<ReclamationPlan> plan_reclaim(const ReclaimRequest& request) const;
    [[nodiscard]] Result<ReclaimResult> apply_reclaim(const ReclaimRequest& request);

    // --- explanation and inspection --------------------------------------
    [[nodiscard]] Result<Explanation> explain(PoolId pool) const;
    [[nodiscard]] Result<FabricSummary> summary() const;
    [[nodiscard]] Result<std::vector<PoolAccounting>> ledger() const;
    [[nodiscard]] Result<std::vector<PoolView>> pool_views() const;
    [[nodiscard]] Result<PoolView> pool_view(PoolId pool) const;
    [[nodiscard]] Result<AllocationRecord> allocation(AllocationId id) const;
    [[nodiscard]] Result<std::vector<AllocationRecord>> allocations(PoolId pool, u64 limit) const;
    [[nodiscard]] Result<std::vector<DecisionRecord>> decision_history(u64 limit) const;
    [[nodiscard]] Result<std::vector<AttemptRecord>> attempt_history(u64 limit) const;
    [[nodiscard]] FabricStatistics statistics() const;
    [[nodiscard]] FabricMetrics metrics() const;
    [[nodiscard]] AccountingReport validate_accounting() const;
    [[nodiscard]] Result<std::string> accounting_text() const;

    // --- durability ------------------------------------------------------
    /// Force a durable checkpoint: write a snapshot and truncate the journal.
    [[nodiscard]] Status checkpoint();

    // --- instrumentation -------------------------------------------------
    void set_event_sink(IEventSink* sink);
    void set_clock(IClock* clock);

private:
    std::unique_ptr<Impl> impl_;
};

/// Convenience: the library's declared capabilities, so an embedder can assert
/// what is and is not compiled in.
struct BF_API Capabilities {
    bool durability{true};
    bool multiprocess_transport{true};
    bool address_sanitizer{false};
    bool physical_device_backend{false};
    bool distributed_consensus{false};
    std::string build_type{};
    std::string compiler{};

    [[nodiscard]] std::string to_json() const;
};

[[nodiscard]] BF_API Capabilities capabilities();

}  // namespace buffer_fabric
