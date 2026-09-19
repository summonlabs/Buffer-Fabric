#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;

namespace {

Status open_harness(Harness* harness, FabricConfig config = FabricConfig{}) {
    return harness->open(config);
}

}  // namespace

BF_TEST(fabric, lifecycle_is_explicit) {
    Harness harness;
    BF_CHECK(!harness.fabric.is_open());
    BF_CHECK_CODE(harness.fabric.allocate(harness.allocate_request(1)), ErrorCode::LifecycleViolation);
    BF_REQUIRE(open_harness(&harness).ok());
    BF_CHECK(harness.fabric.is_open());
    BF_CHECK_CODE(harness.fabric.open(FabricConfig{}), ErrorCode::LifecycleViolation);
    BF_CHECK(harness.fabric.close().ok());
    BF_CHECK(!harness.fabric.is_open());
    BF_CHECK(harness.fabric.close().ok());
}

BF_TEST(fabric, pool_requires_authoritative_capacity) {
    Harness harness;
    harness.fabric.set_clock(&harness.clock);
    BF_REQUIRE(harness.fabric.open(FabricConfig{}).ok());
    BF_REQUIRE(harness.fabric.attach_backend(&harness.backend).ok());
    harness.backend.set_resource(harness.resource, bftest::kResourceCapacity,
                                 Generation::from_raw(1));
    PoolPolicy policy;
    policy.id = harness.policy_id;
    policy.name = "p";
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());

    // No capacity authority yet: the pool cannot be founded.
    PoolRegistration registration;
    registration.name = "orphan";
    registration.resource = harness.resource;
    registration.backend = harness.backend.id();
    registration.raw_units = 1000;
    registration.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(registration), ErrorCode::UnknownResource);

    BF_REQUIRE(harness.fabric
                   .set_capacity(harness.resource, harness.backend.id(), bftest::kResourceCapacity,
                                 Generation{})
                   .ok());
    BF_CHECK(harness.fabric.register_pool(registration).ok());
}

BF_TEST(fabric, capacity_authority_cannot_shrink_below_partitioned_pools) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    harness.backend.set_resource(harness.resource, bftest::kPoolRaw / 2, Generation::from_raw(2));
    BF_CHECK_CODE(harness.fabric.set_capacity(harness.resource, harness.backend.id(),
                                              bftest::kPoolRaw / 2, Generation::from_raw(2)),
                  ErrorCode::CapacityExceeded);
    harness.backend.set_resource(harness.resource, bftest::kResourceCapacity,
                                 Generation::from_raw(3));
    BF_CHECK(harness.fabric
                 .set_capacity(harness.resource, harness.backend.id(), bftest::kResourceCapacity,
                               Generation::from_raw(3))
                 .ok());
}

BF_TEST(fabric, resource_capacity_is_not_over_partitioned) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    PoolRegistration registration;
    registration.name = "too-big";
    registration.resource = harness.resource;
    registration.backend = harness.backend.id();
    registration.raw_units = bftest::kResourceCapacity;
    registration.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(registration), ErrorCode::CapacityExceeded);
}

BF_TEST(fabric, allocation_and_release_preserve_closure) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());

    auto decision = harness.allocate(40'000);
    BF_REQUIRE(decision.ok());
    BF_CHECK(decision.value().kind == DecisionKind::Grant);
    BF_CHECK_EQ(decision.value().granted_units, u64{40'000});
    BF_CHECK_EQ(decision.value().accounting.allocated, u64{40'000});

    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().allocated_units, u64{40'000});
    BF_CHECK_EQ(explanation.value().free_units, bftest::kPoolRaw - bftest::kPoolProtected - 40'000);
    BF_CHECK(explanation.value().accounting.ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());

    BF_REQUIRE(harness.release(decision.value().allocation).ok());
    auto after = harness.fabric.explain(harness.pool);
    BF_REQUIRE(after.ok());
    BF_CHECK_EQ(after.value().allocated_units, u64{0});
    BF_CHECK_EQ(after.value().free_units, bftest::kPoolRaw - bftest::kPoolProtected);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, protected_headroom_is_never_allocated) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;

    auto decision = harness.allocate(usable + 1'000'000);
    BF_REQUIRE(decision.ok());
    BF_CHECK(decision.value().kind == DecisionKind::GrantPartial);
    BF_CHECK_EQ(decision.value().granted_units, usable);

    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().protected_headroom_units, bftest::kPoolProtected);
    BF_CHECK_EQ(explanation.value().free_units, u64{0});
    BF_CHECK(explanation.value().accounting.ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // A further request is refused; the protected headroom is untouched.
    BF_REQUIRE(harness.observe_current().ok());
    auto refused = harness.allocate(1);
    BF_REQUIRE(refused.ok());
    BF_CHECK_EQ(refused.value().granted_units, u64{0});
    BF_CHECK(refused.value().kind == DecisionKind::Refuse ||
             refused.value().kind == DecisionKind::Reduce);
}

BF_TEST(fabric, partial_grant_can_be_disabled) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.allow_partial_grant = false;
    policy.max_single_request_units = 1000;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(5000);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{0});
    BF_CHECK(decision.value().binding == BindingConstraint::PolicyMaxSingleRequest);
}

BF_TEST(fabric, idempotent_replay_and_attempt_conflict) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());

    const AllocateRequest request = harness.allocate_request(1000);
    auto first = harness.fabric.allocate(request);
    BF_REQUIRE(first.ok());
    BF_CHECK(first.value().kind == DecisionKind::Grant);

    auto replay = harness.fabric.allocate(request);
    BF_REQUIRE(replay.ok());
    BF_CHECK(replay.value().kind == DecisionKind::IdempotentReplay);
    BF_CHECK_EQ(replay.value().allocation.raw(), first.value().allocation.raw());
    BF_CHECK_EQ(replay.value().granted_units, first.value().granted_units);
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // Only one allocation exists even though the request was applied twice.
    auto records = harness.fabric.allocations(harness.pool, 100);
    BF_REQUIRE(records.ok());
    BF_CHECK_EQ(records.value().size(), usize{1});

    AllocateRequest conflicting = request;
    conflicting.requested_units += 1;
    BF_CHECK_CODE(harness.fabric.allocate(conflicting), ErrorCode::AttemptConflict);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, stale_epoch_and_generation_are_refused) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());

    AllocateRequest request = harness.allocate_request(100);
    request.expected_epoch = EpochId::from_raw(harness.fabric.epoch().raw() + 5);
    BF_CHECK_CODE(harness.fabric.allocate(request), ErrorCode::StaleEpoch);

    AllocateRequest second = harness.allocate_request(100);
    second.expected_pool_generation = Generation::from_raw(99);
    BF_CHECK_CODE(harness.fabric.allocate(second), ErrorCode::StaleGeneration);

    AllocateRequest third = harness.allocate_request(100);
    third.expected_policy_generation = Generation::from_raw(99);
    BF_CHECK_CODE(harness.fabric.allocate(third), ErrorCode::StaleGeneration);
}

BF_TEST(fabric, reservation_then_commit) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());

    AllocateRequest request = harness.allocate_request(2000);
    request.commit_immediately = false;
    auto reserved = harness.fabric.allocate(request);
    BF_REQUIRE(reserved.ok());
    BF_CHECK_EQ(reserved.value().granted_units, u64{2000});

    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().reserved_units, u64{2000});
    BF_CHECK_EQ(explanation.value().committed_units, u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    CommitRequest commit;
    commit.attempt = harness.next_attempt();
    commit.allocation = reserved.value().allocation;
    commit.expected_epoch = harness.fabric.epoch();
    BF_REQUIRE(harness.fabric.commit(commit).ok());

    auto committed = harness.fabric.explain(harness.pool);
    BF_REQUIRE(committed.ok());
    BF_CHECK_EQ(committed.value().reserved_units, u64{0});
    BF_CHECK_EQ(committed.value().committed_units, u64{2000});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // Committing twice with a new attempt id is a state error.
    CommitRequest again;
    again.attempt = harness.next_attempt();
    again.allocation = reserved.value().allocation;
    BF_CHECK_CODE(harness.fabric.commit(again), ErrorCode::WrongState);
}

BF_TEST(fabric, reservations_expire) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.reservation_ttl_ticks = 100;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());

    AllocateRequest request = harness.allocate_request(2000);
    request.commit_immediately = false;
    auto reserved = harness.fabric.allocate(request);
    BF_REQUIRE(reserved.ok());
    const AllocationId reservation = reserved.value().allocation;
    BF_CHECK_EQ(harness.own_usage(), u64{2000});

    harness.clock.advance(10'000);
    BF_REQUIRE(harness.fabric.expire_reservations(harness.pool).ok());
    BF_CHECK_EQ(harness.own_usage(), u64{0});

    auto record = harness.fabric.allocation(reservation);
    BF_REQUIRE(record.ok());
    BF_CHECK(record.value().state == AllocationState::Expired);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, partial_release_keeps_identity_and_closure) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());

    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    const AllocationId id = decision.value().allocation;

    ReleaseRequest request;
    request.attempt = harness.next_attempt();
    request.allocation = id;
    request.release_all = false;
    request.units = 400;
    BF_REQUIRE(harness.fabric.release(request).ok());

    auto record = harness.fabric.allocation(id);
    BF_REQUIRE(record.ok());
    BF_CHECK_EQ(record.value().units, u64{600});
    BF_CHECK(record.value().holds_units());
    BF_CHECK_EQ(harness.own_usage(), u64{600});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    BF_REQUIRE(harness.release(id).ok());
    auto finished = harness.fabric.allocation(id);
    BF_REQUIRE(finished.ok());
    BF_CHECK(finished.value().state == AllocationState::Released);
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, releasing_a_released_allocation_is_a_noop) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(500);
    BF_REQUIRE(decision.ok());
    BF_REQUIRE(harness.release(decision.value().allocation).ok());
    auto again = harness.release(decision.value().allocation);
    BF_REQUIRE(again.ok());
    BF_CHECK(again.value().kind == DecisionKind::NoOp);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, bounded_overcommit_is_explicit_and_limited) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.overcommit.mode = OvercommitMode::Bounded;
    policy.overcommit.limit_units = 50'000;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    // A queue with no declared demand does not cap the grant.
    auto unlimited = harness.add_queue(harness.pool, 0, 0);
    BF_REQUIRE(unlimited.ok());
    BF_REQUIRE(harness.observe_current().ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;

    auto decision = harness.allocate(usable + 50'000, harness.pool, unlimited.value());
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, usable + 50'000);

    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().overcommit_used_units, u64{50'000});
    BF_CHECK_EQ(explanation.value().free_units, u64{0});
    BF_CHECK(explanation.value().accounting.ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // Beyond the overcommit limit the request is capped.
    BF_REQUIRE(harness.observe_current().ok());
    auto beyond = harness.allocate(1, harness.pool, unlimited.value());
    BF_REQUIRE(beyond.ok());
    BF_CHECK_EQ(beyond.value().granted_units, u64{0});
}

BF_TEST(fabric, overcommit_requires_policy) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;
    auto decision = harness.allocate(usable + 1);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, usable);
    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().overcommit_used_units, u64{0});
}

BF_TEST(fabric, borrowing_draws_from_ancestor_free_capacity) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.borrow.mode = BorrowMode::FromAncestorFree;
    policy.borrow.limit_units = 200'000;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());

    auto child = harness.add_pool(50'000, 0, harness.pool);
    BF_REQUIRE(child.ok());
    const PoolId child_id = child.value();
    auto child_queue = harness.add_queue(child_id, 500'000);
    BF_REQUIRE(child_queue.ok());

    BF_REQUIRE(harness.observe_current(harness.pool).ok());
    BF_REQUIRE(harness.observe_current(child_id).ok());

    auto decision = harness.allocate(120'000, child_id, child_queue.value());
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{120'000});

    auto child_view = harness.fabric.explain(child_id);
    BF_REQUIRE(child_view.ok());
    BF_CHECK_EQ(child_view.value().borrowed_in_units, u64{70'000});
    BF_CHECK_EQ(child_view.value().allocated_units, u64{120'000});

    auto parent_view = harness.fabric.explain(harness.pool);
    BF_REQUIRE(parent_view.ok());
    BF_CHECK_EQ(parent_view.value().lent_out_units, u64{70'000});
    BF_CHECK(parent_view.value().accounting.ok());
    BF_CHECK(child_view.value().accounting.ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // Releasing returns the borrowed units to the lender.
    BF_REQUIRE(harness.release(decision.value().allocation).ok());
    auto parent_after = harness.fabric.explain(harness.pool);
    BF_REQUIRE(parent_after.ok());
    BF_CHECK_EQ(parent_after.value().lent_out_units, u64{0});
    BF_CHECK_EQ(parent_after.value().free_units, bftest::kPoolRaw - bftest::kPoolProtected);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, borrow_limit_is_enforced) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.borrow.mode = BorrowMode::FromAncestorFree;
    policy.borrow.limit_units = 10'000;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    auto child = harness.add_pool(50'000, 0, harness.pool);
    BF_REQUIRE(child.ok());
    auto child_queue = harness.add_queue(child.value(), 500'000);
    BF_REQUIRE(child_queue.ok());
    BF_REQUIRE(harness.observe_current(harness.pool).ok());
    BF_REQUIRE(harness.observe_current(child.value()).ok());

    auto decision = harness.allocate(200'000, child.value(), child_queue.value());
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{60'000});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, queue_demand_bounds_committed_units) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    // A queue whose authoritative demand is below the pool's free capacity is
    // the binding constraint.
    auto quiet = harness.add_queue(harness.pool, 400'000);
    BF_REQUIRE(quiet.ok());
    BF_REQUIRE(harness.observe_current().ok());
    AllocateRequest request = harness.allocate_request(5'000'000, harness.pool, quiet.value());
    auto decision = harness.fabric.allocate(request);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{400'000});
    BF_CHECK(decision.value().binding == BindingConstraint::QueueShare);
}

BF_TEST(fabric, queue_max_committed_overrides_demand) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    auto limited = harness.add_queue(harness.pool, 0, 1'000);
    BF_REQUIRE(limited.ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(500'000, harness.pool, limited.value());
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(decision.value().granted_units, u64{1'000});
    BF_CHECK(decision.value().binding == BindingConstraint::QueueMaxCommitted);
}

BF_TEST(fabric, retiring_a_queue_fences_its_allocations) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(harness.own_usage(), u64{1000});

    BF_REQUIRE(harness.fabric.retire_queue(harness.queue, Generation::from_raw(1)).ok());

    auto record = harness.fabric.allocation(decision.value().allocation);
    BF_REQUIRE(record.ok());
    BF_CHECK(record.value().state == AllocationState::Fenced);
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, queue_generation_advance_invalidates_allocations) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());

    BF_REQUIRE(harness.fabric.update_queue_demand(harness.queue, 800'000, Generation::from_raw(1))
                   .ok());
    // A demand update is not a structural change: the allocation survives.
    auto record = harness.fabric.allocation(decision.value().allocation);
    BF_REQUIRE(record.ok());
    BF_CHECK(record.value().holds_units());

    // Retargeting is structural: it fences everything bound to the old binding.
    auto second = harness.add_pool(200'000, 0);
    BF_REQUIRE(second.ok());
    BF_REQUIRE(harness.fabric.retarget_queue(harness.queue, second.value(), Generation::from_raw(1))
                   .ok());
    auto after = harness.fabric.allocation(decision.value().allocation);
    BF_REQUIRE(after.ok());
    BF_CHECK(after.value().state == AllocationState::Fenced);
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, shrinking_a_pool_fences_allocations_and_preserves_closure) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(500'000);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(harness.own_usage(), u64{500'000});

    auto outcome = harness.fabric.resize_pool(harness.pool, 600'000, bftest::kPoolProtected,
                                              Generation::from_raw(1));
    BF_REQUIRE(outcome.ok());
    BF_CHECK_EQ(outcome.value().fenced_allocations, u64{1});
    BF_CHECK_EQ(outcome.value().fenced_units, u64{500'000});
    BF_CHECK_EQ(outcome.value().accounting.allocated, u64{0});
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    BF_CHECK_EQ(explanation.value().raw_units, u64{600'000});
    BF_CHECK_EQ(explanation.value().free_units, u64{500'000});
}

BF_TEST(fabric, shrink_below_policy_floor_is_refused) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.usable_floor_units = 500'000;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_CHECK_CODE(harness.fabric.resize_pool(harness.pool, 400'000, 0, Generation::from_raw(1)),
                  ErrorCode::PolicyForbids);
}

BF_TEST(fabric, pool_generation_advances_on_resize) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    auto outcome = harness.fabric.resize_pool(harness.pool, 2'000'000, bftest::kPoolProtected,
                                              Generation::from_raw(1));
    BF_REQUIRE(outcome.ok());
    BF_CHECK_EQ(outcome.value().new_generation.raw(), u64{2});
    BF_CHECK_CODE(harness.fabric.resize_pool(harness.pool, 2'100'000, bftest::kPoolProtected,
                                             Generation::from_raw(1)),
                  ErrorCode::StaleGeneration);
}

BF_TEST(fabric, policy_change_fences_dependent_allocations) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());

    PoolPolicy updated = harness.policy;
    updated.pressure.thresholds.high_bp = 8000;
    auto generation = harness.fabric.publish_policy(updated);
    BF_REQUIRE(generation.ok());
    BF_CHECK_EQ(generation.value().raw(), u64{2});

    auto record = harness.fabric.allocation(decision.value().allocation);
    BF_REQUIRE(record.ok());
    BF_CHECK(record.value().state == AllocationState::Fenced);
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    // Re-publishing the identical policy does not advance the generation.
    auto again = harness.fabric.publish_policy(updated);
    BF_REQUIRE(again.ok());
    BF_CHECK_EQ(again.value().raw(), u64{2});
}

BF_TEST(fabric, pool_removal_requires_a_clean_pool) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_CHECK_CODE(harness.fabric.remove_pool(harness.pool, Generation::from_raw(1)),
                  ErrorCode::LifecycleViolation);
    auto extra = harness.add_pool(1000, 0, harness.pool);
    BF_REQUIRE(extra.ok());
    BF_CHECK_CODE(harness.fabric.remove_pool(harness.pool, Generation::from_raw(1)),
                  ErrorCode::LifecycleViolation);
    auto queue_extra = harness.add_queue(extra.value(), 100);
    BF_REQUIRE(queue_extra.ok());
    // A live bound queue blocks removal; a retired one does not.
    BF_CHECK_CODE(harness.fabric.remove_pool(extra.value(), Generation::from_raw(1)),
                  ErrorCode::LifecycleViolation);
    BF_REQUIRE(harness.fabric.retire_queue(queue_extra.value(), Generation::from_raw(1)).ok());
    BF_REQUIRE(harness.fabric.remove_pool(extra.value(), Generation::from_raw(1)).ok());
    BF_CHECK_CODE(harness.fabric.remove_pool(extra.value(), Generation{}), ErrorCode::UnknownPool);
}

BF_TEST(fabric, duplicate_names_are_rejected) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    PoolRegistration registration;
    registration.name = "harness-pool";
    registration.resource = harness.resource;
    registration.backend = harness.backend.id();
    registration.raw_units = 1000;
    registration.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(registration), ErrorCode::DuplicateName);
}

BF_TEST(fabric, pool_depth_is_bounded) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    PoolId cursor = harness.pool;
    for (u64 depth = 0; depth < kMaxPoolDepth + 2; ++depth) {
        auto created = harness.add_pool(1000, 0, cursor);
        if (!created.ok()) {
            BF_CHECK_CODE(created, ErrorCode::PoolDepthExceeded);
            return;
        }
        cursor = created.value();
    }
    BF_CHECK(false);
}

BF_TEST(fabric, fence_stale_reports_and_fences) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());

    auto clean = harness.fabric.fence_stale(harness.pool);
    BF_REQUIRE(clean.ok());
    BF_CHECK(clean.value().kind == DecisionKind::NoOp);

    auto status = harness.fabric.pool_generation_status(harness.pool);
    BF_REQUIRE(status.ok());
    BF_CHECK_EQ(status.value().allocations_bound, u64{1});
    BF_CHECK_EQ(status.value().allocations_stale, u64{0});
}

BF_TEST(fabric, decision_history_and_metrics_are_consistent) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    for (int i = 0; i < 5; ++i) {
        auto decision = harness.allocate(100);
        BF_REQUIRE(decision.ok());
        BF_REQUIRE(harness.release(decision.value().allocation).ok());
        BF_REQUIRE(harness.observe_current().ok());
    }
    const FabricMetrics metrics = harness.fabric.metrics();
    BF_CHECK(metrics.grants >= 5);
    BF_CHECK_EQ(metrics.live_allocations, u64{0});
    BF_CHECK_EQ(metrics.live_committed_units, u64{0});

    auto history = harness.fabric.decision_history(100);
    BF_REQUIRE(history.ok());
    BF_CHECK(history.value().size() >= 10);
}

BF_TEST(fabric, explanation_is_deterministic) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto first = harness.fabric.explain(harness.pool);
    BF_REQUIRE(first.ok());
    auto second = harness.fabric.explain(harness.pool);
    BF_REQUIRE(second.ok());
    BF_CHECK_EQ(first.value().digest, second.value().digest);
    BF_CHECK_EQ(first.value().to_json().size(), second.value().to_json().size());

    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    auto third = harness.fabric.explain(harness.pool);
    BF_REQUIRE(third.ok());
    BF_CHECK(first.value().digest != third.value().digest);
    BF_CHECK(!third.value().to_json().empty());
    BF_CHECK(!third.value().to_text().empty());
}

BF_TEST(fabric, shutdown_refuses_new_work_but_allows_release) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    harness.fabric.begin_shutdown();
    BF_CHECK(harness.fabric.shutting_down());
    BF_CHECK_CODE(harness.fabric.allocate(harness.allocate_request(10)), ErrorCode::ShuttingDown);
    BF_CHECK(harness.release(decision.value().allocation).ok());
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(fabric, accounting_text_reports_closure) {
    Harness harness;
    BF_REQUIRE(open_harness(&harness).ok());
    auto text = harness.fabric.accounting_text();
    BF_REQUIRE(text.ok());
    BF_CHECK(text.value().find("closure=closed") != std::string::npos);
}
