#pragma once

// Shared fixtures for the Buffer Fabric test suite.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include "buffer_fabric/backend.hpp"
#include "buffer_fabric/fabric.hpp"
#include "buffer_fabric/observation.hpp"
#include "test_framework.hpp"

namespace bftest {

using namespace buffer_fabric;

inline constexpr u64 kResourceCapacity = 4'000'000;
inline constexpr u64 kPoolRaw = 1'000'000;
inline constexpr u64 kPoolProtected = 100'000;
inline constexpr u64 kQueueDemand = 900'000;
inline constexpr Tick kEvidenceTtl = 1'000'000;

/// A fabric with one synthetic resource, one policy, one pool and one queue,
/// driven by a manual clock so that every freshness decision is deterministic.
struct Harness {
    SyntheticBackend backend{BackendId::from_raw(1), Generation::from_raw(1), "harness"};
    ManualClock clock{10'000};
    BufferFabric fabric{};
    RingEventSink events{512};

    ResourceId resource{ResourceId::from_raw(1)};
    PoolId pool{PoolId::from_raw(1)};
    QueueId queue{QueueId::from_raw(1)};
    PolicyId policy_id{PolicyId::from_raw(1)};

    u64 attempt_counter{1};
    PoolPolicy policy{};

    /// The policy is fully formed before open() so that callers can adjust it
    /// and publish it as revision 1 of the harness policy.
    Harness() {
        policy.id = policy_id;
        policy.name = "harness";
        policy.pressure.evidence_ttl_ticks = kEvidenceTtl;
    }

    /// Open the fabric and ensure the harness topology exists. Reopening a
    /// durable directory is expected: entities that are already present are
    /// left exactly as the durable image restored them.
    Status open(FabricConfig config = FabricConfig{}) {
        fabric.set_clock(&clock);
        fabric.set_event_sink(&events);
        BF_TRY(fabric.open(config));
        BF_TRY(fabric.attach_backend(&backend));
        backend.set_resource(resource, kResourceCapacity, Generation::from_raw(1));
        auto capacity = fabric.set_capacity(resource, backend.id(), kResourceCapacity, Generation{});
        if (!capacity.ok()) return capacity.status();

        auto published = fabric.publish_policy(policy);
        if (!published.ok() && published.code() != ErrorCode::DuplicateName) {
            return published.status();
        }

        auto existing_pool = fabric.pool_view(pool);
        if (!existing_pool.ok()) {
            if (existing_pool.code() != ErrorCode::UnknownPool) return existing_pool.status();
            PoolRegistration registration;
            registration.id = pool;
            registration.name = "harness-pool";
            registration.resource = resource;
            registration.backend = backend.id();
            registration.raw_units = kPoolRaw;
            registration.protected_units = kPoolProtected;
            registration.policy = policy_id;
            auto created = fabric.register_pool(registration);
            if (!created.ok()) return created.status();
        }

        QueueRegistration queue_registration;
        queue_registration.id = queue;
        queue_registration.name = "harness-queue";
        queue_registration.pool = pool;
        queue_registration.demand_units = kQueueDemand;
        auto created_queue = fabric.register_queue(queue_registration);
        if (!created_queue.ok() && created_queue.code() != ErrorCode::DuplicateQueue &&
            created_queue.code() != ErrorCode::DuplicateName) {
            return created_queue.status();
        }
        return Status::success();
    }

    /// Publish fresh evidence describing the pool's current authoritative
    /// state. Call this before an allocation whenever the policy refuses
    /// increases on unknown evidence (the default).
    Status observe_current(PoolId target = PoolId{}) {
        const PoolId which = target.valid() ? target : pool;
        auto explanation = fabric.explain(which);
        if (!explanation.ok()) return explanation.status();
        const Explanation& view = explanation.value();
        PressureSnapshot snapshot;
        snapshot.pool = which;
        snapshot.pool_generation = view.pool_generation;
        snapshot.epoch = view.epoch;
        snapshot.boot = view.boot;
        snapshot.observed_at = clock.now_ticks();
        snapshot.ttl_ticks = kEvidenceTtl;
        snapshot.demand_units = kQueueDemand;
        snapshot.committed_units =
            view.allocated_units - view.borrowed_in_units + view.lent_out_units;
        snapshot.assert_committed = true;
        return fabric.observe_pressure(snapshot);
    }

    PressureState pressure_state(PoolId target = PoolId{}) {
        const Status status = observe_current(target);
        BF_UNUSED(status);
        auto evidence = fabric.pressure(target.valid() ? target : pool);
        if (!evidence.ok()) return PressureState::Unknown;
        return evidence.value().state;
    }

    AttemptId next_attempt() { return AttemptId::from_raw(attempt_counter++); }

    AllocateRequest allocate_request(u64 units, PoolId target = PoolId{},
                                     QueueId target_queue = QueueId{}) {
        AllocateRequest request;
        request.attempt = next_attempt();
        request.pool = target.valid() ? target : pool;
        request.queue = target_queue.valid() ? target_queue : queue;
        request.requested_units = units;
        request.expected_epoch = fabric.epoch();
        return request;
    }

    Result<Decision> allocate(u64 units, PoolId target = PoolId{},
                              QueueId target_queue = QueueId{}) {
        return fabric.allocate(allocate_request(units, target, target_queue));
    }

    Result<Decision> release(AllocationId id) {
        ReleaseRequest request;
        request.attempt = next_attempt();
        request.allocation = id;
        request.release_all = true;
        request.expected_epoch = fabric.epoch();
        return fabric.release(request);
    }

    /// own_usage = allocated - borrowed_in + lent_out, read back from the
    /// fabric's explanation rather than recomputed by the test.
    u64 own_usage(PoolId target = PoolId{}) const {
        auto explanation = fabric.explain(target.valid() ? target : pool);
        if (!explanation.ok()) return 0;
        const Explanation& view = explanation.value();
        return view.allocated_units - view.borrowed_in_units + view.lent_out_units;
    }

    u64 free_units(PoolId target = PoolId{}) const {
        auto explanation = fabric.explain(target.valid() ? target : pool);
        if (!explanation.ok()) return 0;
        return explanation.value().free_units;
    }

    /// Register an additional pool on the same resource with a fresh id.
    Result<PoolId> add_pool(u64 raw, u64 protected_units, PoolId parent = PoolId{},
                            PolicyId policy_override = PolicyId{}) {
        static std::atomic<u64> next_id{10};
        PoolRegistration registration;
        registration.id = PoolId::from_raw(next_id.fetch_add(1));
        registration.name = "pool-" + std::to_string(registration.id.raw());
        registration.resource = resource;
        registration.backend = backend.id();
        registration.raw_units = raw;
        registration.protected_units = protected_units;
        registration.parent = parent;
        registration.policy = policy_override.valid() ? policy_override : policy_id;
        auto created = fabric.register_pool(registration);
        if (!created.ok()) return created.status();
        return created.value();
    }

    Result<QueueId> add_queue(PoolId target, u64 demand, u64 max_committed = 0) {
        static std::atomic<u64> next_id{100};
        QueueRegistration registration;
        registration.id = QueueId::from_raw(next_id.fetch_add(1));
        registration.name = "queue-" + std::to_string(registration.id.raw());
        registration.pool = target;
        registration.demand_units = demand;
        registration.max_committed_units = max_committed;
        auto created = fabric.register_queue(registration);
        if (!created.ok()) return created.status();
        return created.value();
    }
};

/// Temporary directory helper that removes its tree on destruction.
class TempDirectory {
public:
    explicit TempDirectory(const std::string& tag);
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string file(const std::string& leaf) const;

private:
    std::string path_{};
};

}  // namespace bftest
