#include <string>
#include <vector>

#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;

namespace {

/// Deterministic xorshift generator: the property suite must be reproducible
/// from the seed alone.
struct Random {
    u64 state{0x2545F4914F6CDD1Dull};
    explicit Random(u64 seed) : state(seed == 0 ? 1 : seed) {}
    u64 next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    u64 below(u64 bound) { return bound == 0 ? 0 : next() % bound; }
};

struct World {
    Harness harness{};
    std::vector<PoolId> pools{};
    std::vector<QueueId> queues{};
    std::vector<AllocationId> live{};
    u64 attempt{1};
    u64 operations{0};
};

/// Drive one random operation. Returns false when the generator ran out of
/// applicable operations (which is not a failure).
bool step(World& world, Random& random) {
    Harness& harness = world.harness;
    if (world.pools.empty()) return false;
    const PoolId pool = world.pools[random.below(world.pools.size())];
    const QueueId queue = world.queues[random.below(world.queues.size())];
    const u64 choice = random.below(12);

    if (choice == 0 || choice == 1) {
        const Status observed = harness.observe_current(pool);
        BF_UNUSED(observed);
    } else if (choice == 2 || choice == 3) {
        AllocateRequest request;
        request.attempt = AttemptId::from_raw(world.attempt++);
        request.pool = pool;
        request.queue = queue;
        request.requested_units = 1 + random.below(50'000);
        request.reclaim_priority = random.below(4);
        request.reclaim_class = random.below(8) == 0 ? ReclaimClass::Pinned
                                                     : ReclaimClass::Reclaimable;
        request.commit_immediately = random.below(4) != 0;
        request.expected_epoch = harness.fabric.epoch();
        auto decision = harness.fabric.allocate(request);
        if (decision.ok() && decision.value().granted_units != 0) {
            world.live.push_back(decision.value().allocation);
        }
    } else if (choice == 4 || choice == 5) {
        if (world.live.empty()) return true;
        const std::size_t index = static_cast<std::size_t>(random.below(world.live.size()));
        const AllocationId id = world.live[index];
        ReleaseRequest request;
        request.attempt = AttemptId::from_raw(world.attempt++);
        request.allocation = id;
        const bool partial = random.below(3) == 0;
        request.release_all = !partial;
        if (partial) {
            auto record = harness.fabric.allocation(id);
            if (record.ok() && record.value().units > 1) {
                request.units = 1 + random.below(record.value().units - 1);
            } else {
                request.release_all = true;
            }
        }
        auto decision = harness.fabric.release(request);
        if (decision.ok() && request.release_all) {
            world.live.erase(world.live.begin() + static_cast<std::ptrdiff_t>(index));
        }
    } else if (choice == 6) {
        if (world.live.empty()) return true;
        const AllocationId id = world.live[random.below(world.live.size())];
        CommitRequest request;
        request.attempt = AttemptId::from_raw(world.attempt++);
        request.allocation = id;
        BF_UNUSED(harness.fabric.commit(request));
    } else if (choice == 7) {
        ReclaimRequest request;
        request.attempt = AttemptId::from_raw(world.attempt++);
        request.pool = pool;
        request.target_units = 1 + random.below(20'000);
        auto applied = harness.fabric.apply_reclaim(request);
        if (applied.ok()) {
            world.live.clear();
            auto records = harness.fabric.allocations(pool, 4096);
            if (records.ok()) {
                for (const auto& record : records.value()) {
                    if (record.holds_units()) world.live.push_back(record.id);
                }
            }
        }
    } else if (choice == 8) {
        BF_UNUSED(harness.fabric.fence_stale(pool));
        world.live.clear();
        auto records = harness.fabric.allocations(pool, 4096);
        if (records.ok()) {
            for (const auto& record : records.value()) {
                if (record.holds_units()) world.live.push_back(record.id);
            }
        }
    } else if (choice == 9) {
        BF_UNUSED(harness.fabric.expire_reservations(pool));
    } else if (choice == 10) {
        const u64 raw = 200'000 + random.below(800'000);
        const u64 protected_units = random.below(raw / 8 + 1);
        BF_UNUSED(harness.fabric.resize_pool(pool, raw, protected_units, Generation{}));
        world.live.clear();
        auto records = harness.fabric.allocations(pool, 4096);
        if (records.ok()) {
            for (const auto& record : records.value()) {
                if (record.holds_units()) world.live.push_back(record.id);
            }
        }
    } else {
        BF_UNUSED(harness.fabric.update_queue_demand(queue, random.below(500'000), Generation{}));
    }
    world.operations += 1;
    return true;
}

void run_seeded(u64 seed, u64 steps) {
    World world;
    BF_REQUIRE(world.harness.open().ok());
    world.pools.push_back(world.harness.pool);
    world.queues.push_back(world.harness.queue);

    // One child pool and one extra queue widen the state space.
    auto child = world.harness.add_pool(300'000, 10'000, world.harness.pool);
    if (child.ok()) {
        world.pools.push_back(child.value());
        auto child_queue = world.harness.add_queue(child.value(), 200'000);
        if (child_queue.ok()) world.queues.push_back(child_queue.value());
    }

    Random random(seed);
    for (u64 index = 0; index < steps; ++index) {
        if (!step(world, random)) break;
    }

    const AccountingReport report = world.harness.fabric.validate_accounting();
    if (!report.closed) {
        std::string detail;
        for (const auto& violation : report.violations) {
            detail.append(std::string(to_string(violation.fault)));
            detail.push_back(' ');
            detail.append(violation.detail);
            detail.push_back(';');
        }
        ::bftest::fail(__FILE__, __LINE__, "accounting closure", detail);
    }

    // The explanation must agree with the ledger for every pool.
    for (const PoolId pool : world.pools) {
        auto explanation = world.harness.fabric.explain(pool);
        BF_REQUIRE(explanation.ok());
        auto view = world.harness.fabric.pool_view(pool);
        BF_REQUIRE(view.ok());
        BF_CHECK_EQ(explanation.value().allocated_units, view.value().allocated_units);
        BF_CHECK_EQ(explanation.value().free_units, view.value().free_units);
        BF_CHECK(explanation.value().accounting.closed);
    }
}

}  // namespace

BF_TEST(property, seeded_random_operations_preserve_closure) {
    for (u64 seed = 1; seed <= 24; ++seed) {
        run_seeded(seed * 7919ull, 250);
    }
}

BF_TEST(property, long_run_preserves_closure) {
    run_seeded(0xDEADBEEFull, 3000);
}

BF_TEST(property, replaying_an_attempt_never_changes_state) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());

    const AllocateRequest request = harness.allocate_request(1234);
    auto first = harness.fabric.allocate(request);
    BF_REQUIRE(first.ok());
    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    const Digest before = explanation.value().digest;

    for (int index = 0; index < 8; ++index) {
        auto replay = harness.fabric.allocate(request);
        BF_REQUIRE(replay.ok());
        BF_CHECK(replay.value().kind == DecisionKind::IdempotentReplay);
    }
    auto after = harness.fabric.explain(harness.pool);
    BF_REQUIRE(after.ok());
    BF_CHECK_EQ(before, after.value().digest);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(property, allocation_then_release_returns_to_baseline) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    auto baseline = harness.fabric.explain(harness.pool);
    BF_REQUIRE(baseline.ok());

    for (int round = 0; round < 64; ++round) {
        BF_REQUIRE(harness.observe_current().ok());
        auto decision = harness.allocate(1000 + static_cast<u64>(round));
        BF_REQUIRE(decision.ok());
        BF_REQUIRE(decision.value().granted_units > 0);
        BF_REQUIRE(harness.release(decision.value().allocation).ok());
    }
    auto final_state = harness.fabric.explain(harness.pool);
    BF_REQUIRE(final_state.ok());
    BF_CHECK_EQ(final_state.value().allocated_units, baseline.value().allocated_units);
    BF_CHECK_EQ(final_state.value().free_units, baseline.value().free_units);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(property, every_pressure_band_is_reachable_and_explained) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.pressure.refuse_increase_on_unknown = false;
    policy.refuse_grants_at = PressureState::Unknown;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;

    bool saw_elevated = false;
    bool saw_high = false;
    bool saw_critical = false;
    for (u64 step = 1; step <= 20; ++step) {
        auto explanation = harness.fabric.explain(harness.pool);
        BF_REQUIRE(explanation.ok());
        const u64 used = explanation.value().allocated_units -
                         explanation.value().borrowed_in_units + explanation.value().lent_out_units;
        const u64 want = (usable / 20) * step;
        if (want > used) {
            AllocateRequest request = harness.allocate_request(want - used);
            auto decision = harness.fabric.allocate(request);
            BF_REQUIRE(decision.ok());
        }
        BF_REQUIRE(harness.observe_current().ok());
        auto evidence = harness.fabric.pressure(harness.pool);
        BF_REQUIRE(evidence.ok());
        switch (evidence.value().state) {
            case PressureState::Elevated: saw_elevated = true; break;
            case PressureState::High: saw_high = true; break;
            case PressureState::Critical: saw_critical = true; break;
            default: break;
        }
    }
    BF_CHECK(saw_elevated);
    BF_CHECK(saw_high);
    BF_CHECK(saw_critical);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}
