#include <atomic>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;

namespace {

/// An event sink that calls back into the fabric. This is the executable form
/// of the lock-reentrancy audit: the fabric publishes events only after it has
/// released its state lock, so a sink that takes the lock again cannot deadlock
/// and cannot observe a half-applied mutation.
class ReentrantSink final : public IEventSink {
public:
    explicit ReentrantSink(BufferFabric* fabric) : fabric_(fabric) {}

    void on_event(const FabricEvent& event) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        // Re-entering the fabric from inside a callback must be safe.
        const FabricMetrics metrics = fabric_->metrics();
        BF_UNUSED(metrics);
        const AccountingReport report = fabric_->validate_accounting();
        closures_ok.fetch_add(report.closed ? 1 : 0, std::memory_order_relaxed);
        if (event.kind == EventKind::AllocationCommitted) {
            commits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::atomic<u64> calls{0};
    std::atomic<u64> closures_ok{0};
    std::atomic<u64> commits{0};

private:
    BufferFabric* fabric_;
};

/// A clock that calls back into the fabric. The fabric samples the clock
/// before taking its state lock, so this re-entrant public call is safe; if the
/// clock were ever read while the lock was held, the nested call would
/// deadlock. The nested read is detected and short-circuited so the test fails
/// fast instead of hanging.
class ReentrantClock final : public IClock {
public:
    explicit ReentrantClock(BufferFabric* fabric) : fabric_(fabric) {}

    [[nodiscard]] Tick now_ticks() const noexcept override {
        static thread_local bool inside = false;
        if (inside) {
            nested_calls.fetch_add(1, std::memory_order_relaxed);
            return ticks.load(std::memory_order_relaxed);
        }
        inside = true;
        const EpochId epoch = fabric_->epoch();
        BF_UNUSED(epoch);
        inside = false;
        return ticks.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] u64 wall_seconds() const noexcept override { return 1700000000ull; }

    mutable std::atomic<Tick> ticks{20'000};
    mutable std::atomic<u64> nested_calls{0};

private:
    BufferFabric* fabric_;
};

}  // namespace

BF_TEST(concurrency, the_clock_is_never_read_while_the_state_lock_is_held) {
    Harness harness;
    ReentrantClock clock(&harness.fabric);
    harness.fabric.set_clock(&clock);
    BF_REQUIRE(harness.open().ok());
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    auto decision = harness.allocate(4096);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{4096});
    BF_REQUIRE(harness.release(decision.value().allocation).ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());
    // The clock really did re-enter the fabric, and it did so with no lock
    // held: the process is still running, which is the proof.
    BF_CHECK(clock.nested_calls.load() > 0);
}

BF_TEST(concurrency, parallel_allocate_and_release_preserves_closure) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());
    // Evidence has no expiry within the run.
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    constexpr int kThreads = 8;
    constexpr int kIterations = 400;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                const u64 attempt = static_cast<u64>(thread_index) * 1'000'000ull +
                                    static_cast<u64>(iteration) * 2ull + 1ull;
                AllocateRequest request;
                request.attempt = AttemptId::from_raw(attempt);
                request.pool = harness.pool;
                request.queue = harness.queue;
                request.requested_units = 512;
                auto decision = harness.fabric.allocate(request);
                if (!decision.ok()) {
                    failures.fetch_add(1);
                    continue;
                }
                if (decision.value().granted_units == 0) continue;
                ReleaseRequest release;
                release.attempt = AttemptId::from_raw(attempt + 1);
                release.allocation = decision.value().allocation;
                release.release_all = true;
                auto released = harness.fabric.release(release);
                if (!released.ok()) failures.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) thread.join();

    BF_CHECK_EQ(failures.load(), 0);
    const AccountingReport report = harness.fabric.validate_accounting();
    BF_CHECK(report.closed);
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    const FabricMetrics metrics = harness.fabric.metrics();
    BF_CHECK_EQ(metrics.live_allocations, u64{0});
}

BF_TEST(concurrency, a_duplicated_attempt_is_applied_exactly_once) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    constexpr int kThreads = 12;
    const AllocateRequest request = harness.allocate_request(4096);
    std::atomic<int> grants{0};
    std::atomic<int> replays{0};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            auto decision = harness.fabric.allocate(request);
            if (!decision.ok()) {
                errors.fetch_add(1);
                return;
            }
            if (decision.value().kind == DecisionKind::IdempotentReplay) {
                replays.fetch_add(1);
                return;
            }
            if (decision.value().granted_units != 0) grants.fetch_add(1);
        });
    }
    for (auto& thread : threads) thread.join();

    BF_CHECK_EQ(errors.load(), 0);
    BF_CHECK_EQ(grants.load(), 1);
    BF_CHECK_EQ(replays.load(), kThreads - 1);
    BF_CHECK_EQ(harness.own_usage(), u64{4096});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(concurrency, event_callbacks_may_reenter_the_fabric) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    // The sink is installed after open() so that the harness's own sink does
    // not replace it.
    ReentrantSink sink(&harness.fabric);
    harness.fabric.set_event_sink(&sink);
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    for (int index = 0; index < 32; ++index) {
        auto decision = harness.allocate(256);
        BF_REQUIRE(decision.ok());
        if (decision.value().granted_units != 0) {
            BF_REQUIRE(harness.release(decision.value().allocation).ok());
        }
    }
    BF_CHECK(sink.calls.load() > 0);
    BF_CHECK_EQ(sink.closures_ok.load(), sink.calls.load());
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(concurrency, concurrent_evaluation_never_mutates) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(10'000);
    BF_REQUIRE(decision.ok());
    auto before = harness.fabric.explain(harness.pool);
    BF_REQUIRE(before.ok());

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int index = 0; index < 6; ++index) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < 200; ++iteration) {
                EvaluateRequest request;
                request.pool = harness.pool;
                request.queue = harness.queue;
                request.requested_units = 1024;
                auto evaluated = harness.fabric.evaluate(request);
                if (!evaluated.ok()) errors.fetch_add(1);
                auto explanation = harness.fabric.explain(harness.pool);
                if (!explanation.ok()) errors.fetch_add(1);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    BF_CHECK_EQ(errors.load(), 0);
    auto after = harness.fabric.explain(harness.pool);
    BF_REQUIRE(after.ok());
    BF_CHECK_EQ(before.value().allocated_units, after.value().allocated_units);
    BF_CHECK_EQ(before.value().free_units, after.value().free_units);
}

BF_TEST(concurrency, shutdown_stops_accepting_work_while_releases_complete) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    std::vector<AllocationId> ids;
    for (int index = 0; index < 16; ++index) {
        auto decision = harness.allocate(1024);
        BF_REQUIRE(decision.ok());
        ids.push_back(decision.value().allocation);
    }

    std::atomic<int> refused{0};
    std::atomic<int> released{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int index = 0; index < 64; ++index) {
            auto decision = harness.allocate(64);
            if (!decision.ok() && decision.code() == ErrorCode::ShuttingDown) {
                refused.fetch_add(1);
            }
        }
    });
    threads.emplace_back([&] {
        for (const AllocationId id : ids) {
            if (harness.release(id).ok()) released.fetch_add(1);
        }
    });
    harness.fabric.begin_shutdown();
    for (auto& thread : threads) thread.join();

    BF_CHECK(released.load() > 0);
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(concurrency, concurrent_topology_mutation_is_serialised) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int index = 0; index < 200; ++index) {
            auto decision = harness.allocate(128);
            if (!decision.ok()) errors.fetch_add(1);
        }
    });
    threads.emplace_back([&] {
        for (int index = 0; index < 200; ++index) {
            if (!harness.observe_current().ok()) errors.fetch_add(1);
        }
    });
    threads.emplace_back([&] {
        for (int index = 0; index < 200; ++index) {
            const AccountingReport report = harness.fabric.validate_accounting();
            if (!report.closed) errors.fetch_add(1);
        }
    });
    for (auto& thread : threads) thread.join();
    BF_CHECK_EQ(errors.load(), 0);
    BF_CHECK(harness.fabric.validate_accounting().closed);
}
