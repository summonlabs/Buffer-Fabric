// bfbench -- synthetic benchmark for Buffer Fabric.
//
// SYNTHETIC. Every number produced here comes from an in-process synthetic
// topology driven by a synthetic backend and a manual clock. Nothing in this
// program observes or programs physical hardware, and no result here
// represents a physical network, NIC, switch or device measurement.
//
// What is measured is completed work: an operation is counted only once the
// fabric has accepted it and the accounting identity has been verified. No
// enqueue or submission latency is reported.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "buffer_fabric/backend.hpp"
#include "buffer_fabric/fabric.hpp"
#include "buffer_fabric/json.hpp"
#include "buffer_fabric/version.hpp"
#include "tool_args.hpp"

using namespace buffer_fabric;

namespace {

struct Random {
    u64 state{0x9E3779B97F4A7C15ull};

    explicit Random(u64 seed) : state(seed == 0 ? 1 : seed) {}

    u64 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    u64 below(u64 bound) { return bound == 0 ? 0 : next() % bound; }
};

struct ScenarioResult {
    std::string name{};
    u64 operations{0};
    double seconds{0.0};
    u64 units{0};
    bool closed{true};
};

struct Topology {
    BufferFabric fabric{};
    SyntheticBackend backend{BackendId::from_raw(1), Generation::from_raw(1), "bench"};
    ManualClock clock{1'000};
    std::vector<PoolId> pools{};
    std::vector<QueueId> queues{};
    std::vector<std::vector<QueueId>> queues_by_pool{};
    ResourceId resource{ResourceId::from_raw(1)};
};

Status build_topology(Topology* topo, u64 pool_count, u64 queues_per_pool) {
    topo->fabric.set_clock(&topo->clock);
    FabricConfig config;
    config.decision_history_capacity = 4096;
    config.attempt_memory_capacity = 1u << 16;
    BF_TRY(topo->fabric.open(config));
    BF_TRY(topo->fabric.attach_backend(&topo->backend));

    const u64 pool_units = 1u << 22;
    const u64 capacity = pool_units * pool_count + (1u << 20);
    topo->backend.set_resource(topo->resource, capacity, Generation::from_raw(1));
    auto capacity_generation =
        topo->fabric.set_capacity(topo->resource, topo->backend.id(), capacity, Generation{});
    if (!capacity_generation.ok()) return capacity_generation.status();

    PoolPolicy policy;
    policy.id = PolicyId::from_raw(1);
    policy.name = "bench";
    policy.pressure.refuse_increase_on_unknown = false;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    policy.reclaim.max_units_per_operation = kMaxUnitsPerPool;
    auto published = topo->fabric.publish_policy(policy);
    if (!published.ok()) return published.status();

    for (u64 index = 0; index < pool_count; ++index) {
        PoolRegistration registration;
        registration.id = PoolId::from_raw(index + 1);
        registration.name = "bench-pool-" + std::to_string(index + 1);
        registration.resource = topo->resource;
        registration.backend = topo->backend.id();
        registration.raw_units = pool_units;
        registration.protected_units = pool_units / 16;
        registration.policy = PolicyId::from_raw(1);
        auto created = topo->fabric.register_pool(registration);
        if (!created.ok()) return created.status();
        topo->pools.push_back(created.value());
        topo->queues_by_pool.emplace_back();
        for (u64 queue_index = 0; queue_index < queues_per_pool; ++queue_index) {
            QueueRegistration queue_registration;
            queue_registration.id =
                QueueId::from_raw(index * queues_per_pool + queue_index + 1);
            queue_registration.name = "bench-queue-" + std::to_string(index) + "-" +
                                      std::to_string(queue_index);
            queue_registration.pool = created.value();
            queue_registration.demand_units = 0;
            auto queue_created = topo->fabric.register_queue(queue_registration);
            if (!queue_created.ok()) return queue_created.status();
            topo->queues.push_back(queue_created.value());
            topo->queues_by_pool.back().push_back(queue_created.value());
        }
    }
    return Status::success();
}

double elapsed_seconds(std::chrono::steady_clock::time_point start) {
    const auto delta = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double>(delta).count();
}

void report(const ScenarioResult& result, bool json, bool last) {
    if (json) {
        std::printf("    {\"scenario\":\"%s\",\"completed_operations\":%llu,\"seconds\":%.6f,"
                    "\"operations_per_second\":%.1f,\"units\":%llu,\"accounting_closed\":%s}%s\n",
                    result.name.c_str(), static_cast<unsigned long long>(result.operations),
                    result.seconds,
                    result.seconds > 0 ? static_cast<double>(result.operations) / result.seconds : 0.0,
                    static_cast<unsigned long long>(result.units),
                    result.closed ? "true" : "false", last ? "" : ",");
        return;
    }
    std::printf("%-28s %12llu ops %10.4f s %14.0f ops/s units=%llu closure=%s\n",
                result.name.c_str(), static_cast<unsigned long long>(result.operations),
                result.seconds,
                result.seconds > 0 ? static_cast<double>(result.operations) / result.seconds : 0.0,
                static_cast<unsigned long long>(result.units),
                result.closed ? "closed" : "VIOLATED");
}

}  // namespace

int main(int argc, char** argv) {
    const bftool::Arguments arguments = bftool::parse(argc, argv);
    if (arguments.has("--help")) {
        bftool::print_usage("bfbench",
                            "[--ops N] [--pools N] [--queues N] [--seed N] [--json]");
        return 0;
    }
    const u64 operations = arguments.number("--ops", 200'000);
    const u64 pool_count = arguments.number("--pools", 4);
    const u64 queues_per_pool = arguments.number("--queues", 8);
    const u64 seed = arguments.number("--seed", 0x5EED);
    const bool json = arguments.has("--json");

    if (pool_count == 0 || pool_count > 256 || queues_per_pool == 0 || queues_per_pool > 256) {
        std::fprintf(stderr, "pools and queues must be between 1 and 256\n");
        return 1;
    }

    Topology topo;
    const Status built = build_topology(&topo, pool_count, queues_per_pool);
    if (!built.ok()) {
        std::fprintf(stderr, "topology build failed: %s\n", built.to_string().c_str());
        return 1;
    }

    Random random(seed);
    std::vector<ScenarioResult> results;

    if (json) {
        std::printf("{\n  \"label\": \"SYNTHETIC\",\n");
        std::printf("  \"note\": \"in-process synthetic topology; no physical device or network was "
                    "observed\",\n");
        std::printf("  \"pools\": %llu,\n  \"queues_per_pool\": %llu,\n  \"seed\": %llu,\n",
                    static_cast<unsigned long long>(pool_count),
                    static_cast<unsigned long long>(queues_per_pool),
                    static_cast<unsigned long long>(seed));
        std::printf("  \"scenarios\": [\n");
    } else {
        std::printf("Buffer Fabric %s synthetic benchmark\n", BUFFER_FABRIC_VERSION_STRING);
        std::printf("label=SYNTHETIC pools=%llu queues_per_pool=%llu seed=%llu\n",
                    static_cast<unsigned long long>(pool_count),
                    static_cast<unsigned long long>(queues_per_pool),
                    static_cast<unsigned long long>(seed));
        std::printf("all figures are completed work in this process; no physical device is "
                    "exercised\n\n");
    }

    // 1. Steady-state allocate/release cycles.
    {
        ScenarioResult result;
        result.name = "allocate_release";
        u64 attempt = 1;
        const auto start = std::chrono::steady_clock::now();
        for (u64 index = 0; index < operations; ++index) {
            const u64 pool_index = random.below(pool_count);
            AllocateRequest request;
            request.attempt = AttemptId::from_raw(attempt++);
            request.pool = topo.pools[pool_index];
            request.queue = topo.queues_by_pool[pool_index][random.below(queues_per_pool)];
            request.requested_units = 1 + random.below(4096);
            auto decision = topo.fabric.allocate(request);
            if (!decision.ok() || decision.value().granted_units == 0) continue;
            ReleaseRequest release;
            release.attempt = AttemptId::from_raw(attempt++);
            release.allocation = decision.value().allocation;
            release.release_all = true;
            auto released = topo.fabric.release(release);
            if (!released.ok()) continue;
            result.operations += 2;
            result.units += decision.value().granted_units;
        }
        result.seconds = elapsed_seconds(start);
        result.closed = topo.fabric.validate_accounting().closed;
        results.push_back(result);
        report(result, json, false);
    }

    // 2. Fill each pool to its limit, then reclaim everything.
    {
        ScenarioResult result;
        result.name = "fill_then_reclaim";
        u64 attempt = 1'000'000;
        std::vector<AllocationId> live;
        const auto start = std::chrono::steady_clock::now();
        bool filled = true;
        while (filled) {
            filled = false;
            for (u64 pool_index = 0; pool_index < pool_count; ++pool_index) {
                AllocateRequest request;
                request.attempt = AttemptId::from_raw(attempt++);
                request.pool = topo.pools[pool_index];
                request.queue = topo.queues_by_pool[pool_index][0];
                request.requested_units = 1024;
                auto decision = topo.fabric.allocate(request);
                if (decision.ok() && decision.value().granted_units == 1024) {
                    live.push_back(decision.value().allocation);
                    result.operations += 1;
                    result.units += 1024;
                    filled = true;
                }
            }
        }
        for (u64 pool_index = 0; pool_index < pool_count; ++pool_index) {
            ReclaimRequest reclaim;
            reclaim.attempt = AttemptId::from_raw(attempt++);
            reclaim.pool = topo.pools[pool_index];
            reclaim.target_units = 1ull << 40;
            auto applied = topo.fabric.apply_reclaim(reclaim);
            if (applied.ok()) result.operations += applied.value().revoked_allocations;
        }
        result.seconds = elapsed_seconds(start);
        result.closed = topo.fabric.validate_accounting().closed;
        results.push_back(result);
        report(result, json, false);
    }

    // 3. Fragmentation pattern: allocate many small blocks and release every
    //    other one, then measure how much further allocation succeeds.
    {
        ScenarioResult result;
        result.name = "fragmentation";
        u64 attempt = 10'000'000;
        std::vector<AllocationId> blocks;
        const auto start = std::chrono::steady_clock::now();
        const u64 blocks_per_pool = 512;
        for (u64 pool_index = 0; pool_index < pool_count; ++pool_index) {
            const u64 quantum =
                (1u << 22) - ((1u << 22) / 16) == 0 ? 1 : (((1u << 22) - ((1u << 22) / 16)) / blocks_per_pool);
            for (u64 index = 0; index < blocks_per_pool; ++index) {
                AllocateRequest request;
                request.attempt = AttemptId::from_raw(attempt++);
                request.pool = topo.pools[pool_index];
                request.queue = topo.queues_by_pool[pool_index][index % queues_per_pool];
                request.requested_units = quantum == 0 ? 1 : quantum;
                auto decision = topo.fabric.allocate(request);
                if (decision.ok() && decision.value().granted_units != 0) {
                    blocks.push_back(decision.value().allocation);
                    result.operations += 1;
                    result.units += decision.value().granted_units;
                }
            }
        }
        for (std::size_t index = 0; index < blocks.size(); index += 2) {
            ReleaseRequest release;
            release.attempt = AttemptId::from_raw(attempt++);
            release.allocation = blocks[index];
            release.release_all = true;
            if (topo.fabric.release(release).ok()) result.operations += 1;
        }
        result.seconds = elapsed_seconds(start);
        result.closed = topo.fabric.validate_accounting().closed;
        results.push_back(result);
        report(result, json, false);
    }

    // 4. Pure evaluation throughput: no mutation at all.
    {
        ScenarioResult result;
        result.name = "evaluate";
        const auto start = std::chrono::steady_clock::now();
        for (u64 index = 0; index < operations; ++index) {
            const u64 pool_index = random.below(pool_count);
            EvaluateRequest request;
            request.pool = topo.pools[pool_index];
            request.queue = topo.queues_by_pool[pool_index][random.below(queues_per_pool)];
            request.requested_units = 1 + random.below(4096);
            auto evaluated = topo.fabric.evaluate(request);
            if (evaluated.ok()) result.operations += 1;
        }
        result.seconds = elapsed_seconds(start);
        result.closed = topo.fabric.validate_accounting().closed;
        results.push_back(result);
        report(result, json, false);
    }

    // 5. Pressure transitions: drive the fabric across every band and measure
    //    the completed allocation decisions that result.
    {
        ScenarioResult result;
        result.name = "pressure_transitions";
        u64 attempt = 20'000'000;
        const auto start = std::chrono::steady_clock::now();
        const PoolId target = topo.pools[0];
        auto explanation = topo.fabric.explain(target);
        if (explanation.ok()) {
            const u64 usable = explanation.value().usable_units;
            const u64 steps = 40;
            for (u64 step = 0; step < steps; ++step) {
                const u64 want = (usable / steps) * (step + 1);
                const u64 used = explanation.value().allocated_units -
                                 explanation.value().borrowed_in_units +
                                 explanation.value().lent_out_units;
                if (want > used) {
                    AllocateRequest request;
                    request.attempt = AttemptId::from_raw(attempt++);
                    request.pool = target;
                    request.queue = topo.queues_by_pool[0][0];
                    request.requested_units = want - used;
                    auto decision = topo.fabric.allocate(request);
                    if (decision.ok()) result.operations += 1;
                }
                PressureSnapshot snapshot;
                snapshot.pool = target;
                snapshot.pool_generation = explanation.value().pool_generation;
                snapshot.epoch = topo.fabric.epoch();
                snapshot.boot = topo.fabric.boot_incarnation();
                snapshot.observed_at = topo.clock.now_ticks();
                snapshot.ttl_ticks = 1ull << 62;
                snapshot.committed_units = 0;
                snapshot.assert_committed = false;
                if (topo.fabric.observe_pressure(snapshot).ok()) result.operations += 1;
                topo.clock.advance(1000);
                explanation = topo.fabric.explain(target);
                if (!explanation.ok()) break;
            }
        }
        result.seconds = elapsed_seconds(start);
        result.closed = topo.fabric.validate_accounting().closed;
        results.push_back(result);
        report(result, json, true);
    }

    if (json) {
        std::printf("  ],\n");
        const AccountingReport report_all = topo.fabric.validate_accounting();
        std::printf("  \"accounting_closed\": %s,\n", report_all.closed ? "true" : "false");
        const FabricMetrics metrics = topo.fabric.metrics();
        std::printf("  \"decisions\": %llu,\n", static_cast<unsigned long long>(metrics.decisions));
        std::printf("  \"grants\": %llu,\n", static_cast<unsigned long long>(metrics.grants));
        std::printf("  \"refusals\": %llu\n", static_cast<unsigned long long>(metrics.refusals));
        std::printf("}\n");
    } else {
        const AccountingReport report_all = topo.fabric.validate_accounting();
        std::printf("\naccounting closure across every scenario: %s\n",
                    report_all.closed ? "closed" : "VIOLATED");
        const FabricMetrics metrics = topo.fabric.metrics();
        std::printf("decisions=%llu grants=%llu refusals=%llu fences=%llu reclaims=%llu\n",
                    static_cast<unsigned long long>(metrics.decisions),
                    static_cast<unsigned long long>(metrics.grants),
                    static_cast<unsigned long long>(metrics.refusals),
                    static_cast<unsigned long long>(metrics.fences),
                    static_cast<unsigned long long>(metrics.reclaims));
    }
    const VoidResult closed = topo.fabric.close();
    if (!closed.ok()) {
        std::fprintf(stderr, "close failed: %s\n", closed.status().to_string().c_str());
        return 1;
    }
    for (const auto& result : results) {
        if (!result.closed) return 1;
    }
    return 0;
}
