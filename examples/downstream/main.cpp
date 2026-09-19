// Independent downstream consumer of Buffer Fabric.
//
// This program links only against the installed package. It exercises the
// public surface end to end: attach a backend, publish capacity authority,
// publish policy, register a pool and a queue, observe pressure, allocate,
// release, inspect the explanation, and assert that the accounting identity
// closes.

#include <cstdio>
#include <string>

#include <buffer_fabric/backend.hpp>
#include <buffer_fabric/fabric.hpp>

using namespace buffer_fabric;

namespace {

int fail(const char* what, const Status& status) {
    std::fprintf(stderr, "downstream: %s failed: %s\n", what, status.to_string().c_str());
    return 1;
}

}  // namespace

int main() {
    const ResourceId resource = ResourceId::from_raw(1);
    const PoolId pool_id = PoolId::from_raw(1);
    const QueueId queue_id = QueueId::from_raw(1);
    const PolicyId policy_id = PolicyId::from_raw(1);
    constexpr u64 kCapacity = 4'000'000;
    constexpr u64 kRaw = 1'000'000;
    constexpr u64 kProtected = 100'000;

    SyntheticBackend backend(BackendId::from_raw(1), Generation::from_raw(1), "downstream");
    BufferFabric fabric;

    Status status = fabric.open(FabricConfig{});
    if (!status.ok()) return fail("open", status);
    VoidResult attached = fabric.attach_backend(&backend);
    if (!attached.ok()) return fail("attach_backend", attached.status());

    backend.set_resource(resource, kCapacity, Generation::from_raw(1));
    auto capacity = fabric.set_capacity(resource, backend.id(), kCapacity, Generation{});
    if (!capacity.ok()) return fail("set_capacity", capacity.status());

    PoolPolicy policy;
    policy.id = policy_id;
    policy.name = "downstream";
    policy.pressure.evidence_ttl_ticks = 1'000'000'000ull;
    auto policy_generation = fabric.publish_policy(policy);
    if (!policy_generation.ok()) return fail("publish_policy", policy_generation.status());

    PoolRegistration pool_registration;
    pool_registration.id = pool_id;
    pool_registration.name = "downstream-pool";
    pool_registration.resource = resource;
    pool_registration.backend = backend.id();
    pool_registration.raw_units = kRaw;
    pool_registration.protected_units = kProtected;
    pool_registration.policy = policy_id;
    auto created_pool = fabric.register_pool(pool_registration);
    if (!created_pool.ok()) return fail("register_pool", created_pool.status());

    QueueRegistration queue_registration;
    queue_registration.id = queue_id;
    queue_registration.name = "downstream-queue";
    queue_registration.pool = pool_id;
    queue_registration.demand_units = 400'000;
    auto created_queue = fabric.register_queue(queue_registration);
    if (!created_queue.ok()) return fail("register_queue", created_queue.status());

    // An increase requires fresh evidence. With no evidence the fabric reports
    // that revalidation is required rather than inventing authority.
    AllocateRequest cold;
    cold.attempt = AttemptId::from_raw(1);
    cold.pool = pool_id;
    cold.queue = queue_id;
    cold.requested_units = 1000;
    auto cold_result = fabric.allocate(cold);
    if (!cold_result.ok()) return fail("allocate(no evidence)", cold_result.status());
    if (cold_result.value().granted_units != 0 ||
        cold_result.value().kind != DecisionKind::Revalidate) {
        std::fprintf(stderr, "downstream: expected a revalidation demand without evidence\n");
        return 1;
    }

    auto explanation = fabric.explain(pool_id);
    if (!explanation.ok()) return fail("explain", explanation.status());

    PressureSnapshot snapshot;
    snapshot.pool = pool_id;
    snapshot.pool_generation = explanation.value().pool_generation;
    snapshot.epoch = fabric.epoch();
    snapshot.boot = fabric.boot_incarnation();
    snapshot.observed_at = 1;
    snapshot.ttl_ticks = 1'000'000'000ull;
    snapshot.demand_units = 400'000;
    snapshot.committed_units = 0;
    snapshot.assert_committed = false;
    status = fabric.observe_pressure(snapshot);
    if (!status.ok()) return fail("observe_pressure", status);

    AllocateRequest request;
    request.attempt = AttemptId::from_raw(2);
    request.pool = pool_id;
    request.queue = queue_id;
    request.requested_units = 250'000;
    auto decision = fabric.allocate(request);
    if (!decision.ok()) return fail("allocate", decision.status());
    if (decision.value().granted_units != 250'000) {
        std::fprintf(stderr, "downstream: expected a full grant, got %llu\n",
                     static_cast<unsigned long long>(decision.value().granted_units));
        return 1;
    }

    auto after = fabric.explain(pool_id);
    if (!after.ok()) return fail("explain(after)", after.status());
    if (!after.value().accounting.closed) {
        std::fprintf(stderr, "downstream: accounting closure violated\n");
        return 1;
    }
    const u64 usable = kRaw - kProtected;
    if (after.value().allocated_units != 250'000 ||
        after.value().free_units != usable - 250'000) {
        std::fprintf(stderr, "downstream: unexpected accounting\n");
        return 1;
    }

    ReleaseRequest release;
    release.attempt = AttemptId::from_raw(3);
    release.allocation = decision.value().allocation;
    release.release_all = true;
    auto released = fabric.release(release);
    if (!released.ok()) return fail("release", released.status());

    const AccountingReport report = fabric.validate_accounting();
    if (!report.closed) {
        std::fprintf(stderr, "downstream: deep accounting verification failed\n");
        return 1;
    }
    auto final_state = fabric.explain(pool_id);
    if (!final_state.ok()) return fail("explain(final)", final_state.status());
    if (final_state.value().allocated_units != 0) {
        std::fprintf(stderr, "downstream: pool did not return to its baseline\n");
        return 1;
    }

    std::printf("downstream ok: capacity=%llu usable=%llu closure=closed\n",
                static_cast<unsigned long long>(kRaw),
                static_cast<unsigned long long>(usable));
    std::printf("capabilities: %s\n", capabilities().to_json().c_str());
    VoidResult closed = fabric.close();
    if (!closed.ok()) return fail("close", closed.status());
    return 0;
}
