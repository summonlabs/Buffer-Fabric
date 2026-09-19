#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;

BF_TEST(pressure, unknown_evidence_never_authorises_an_increase) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    BF_CHECK(decision.value().kind == DecisionKind::Revalidate);
    BF_CHECK(decision.value().status == ErrorCode::EvidenceUnknown);
    BF_CHECK(decision.value().binding == BindingConstraint::EvidenceUnknown);
    BF_CHECK(decision.value().intent.revalidate_required);
    BF_CHECK_EQ(decision.value().granted_units, u64{0});
    BF_CHECK_EQ(harness.own_usage(), u64{0});
}

BF_TEST(pressure, stale_evidence_is_reported_as_stale_not_clear) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());
    harness.clock.advance(bftest::kEvidenceTtl + 1);
    auto evidence = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(evidence.ok());
    BF_CHECK(evidence.value().state == PressureState::Stale);
    BF_CHECK(evidence.value().reason == ErrorCode::StaleEvidence);
    BF_CHECK(!evidence.value().usable());

    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    BF_CHECK(decision.value().kind == DecisionKind::Revalidate);
    BF_CHECK(decision.value().status == ErrorCode::StaleEvidence);
}

BF_TEST(pressure, evidence_from_another_epoch_is_refused_at_ingestion) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PressureSnapshot snapshot;
    snapshot.pool = harness.pool;
    snapshot.pool_generation = Generation::from_raw(1);
    snapshot.epoch = EpochId::from_raw(harness.fabric.epoch().raw() + 1);
    snapshot.observed_at = harness.clock.now_ticks();
    snapshot.ttl_ticks = bftest::kEvidenceTtl;
    BF_CHECK_CODE(harness.fabric.observe_pressure(snapshot), ErrorCode::StaleEpoch);
}

BF_TEST(pressure, evidence_for_another_generation_is_refused) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PressureSnapshot snapshot;
    snapshot.pool = harness.pool;
    snapshot.pool_generation = Generation::from_raw(9);
    snapshot.epoch = harness.fabric.epoch();
    snapshot.observed_at = harness.clock.now_ticks();
    snapshot.ttl_ticks = bftest::kEvidenceTtl;
    BF_CHECK_CODE(harness.fabric.observe_pressure(snapshot), ErrorCode::StaleGeneration);
}

BF_TEST(pressure, future_dated_evidence_is_refused) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PressureSnapshot snapshot;
    snapshot.pool = harness.pool;
    snapshot.pool_generation = Generation::from_raw(1);
    snapshot.epoch = harness.fabric.epoch();
    snapshot.observed_at = harness.clock.now_ticks() + 1000;
    snapshot.ttl_ticks = bftest::kEvidenceTtl;
    BF_CHECK_CODE(harness.fabric.observe_pressure(snapshot), ErrorCode::InvalidArgument);
}

BF_TEST(pressure, evidence_that_disagrees_with_accounting_is_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    PressureSnapshot snapshot;
    snapshot.pool = harness.pool;
    snapshot.pool_generation = explanation.value().pool_generation;
    snapshot.epoch = harness.fabric.epoch();
    snapshot.observed_at = harness.clock.now_ticks();
    snapshot.ttl_ticks = bftest::kEvidenceTtl;
    snapshot.committed_units = 12345;
    snapshot.assert_committed = true;
    BF_REQUIRE(harness.fabric.observe_pressure(snapshot).ok());
    auto evidence = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(evidence.ok());
    BF_CHECK(evidence.value().state == PressureState::Stale);
    BF_CHECK(evidence.value().detail.find("authoritative accounting") != std::string::npos);
}

BF_TEST(pressure, evidence_may_opt_out_of_the_accounting_cross_check) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    auto explanation = harness.fabric.explain(harness.pool);
    BF_REQUIRE(explanation.ok());
    PressureSnapshot snapshot;
    snapshot.pool = harness.pool;
    snapshot.pool_generation = explanation.value().pool_generation;
    snapshot.epoch = harness.fabric.epoch();
    snapshot.observed_at = harness.clock.now_ticks();
    snapshot.ttl_ticks = bftest::kEvidenceTtl;
    snapshot.committed_units = 0;
    snapshot.assert_committed = false;
    BF_REQUIRE(harness.fabric.observe_pressure(snapshot).ok());
    auto evidence = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(evidence.ok());
    BF_CHECK(evidence.value().state == PressureState::Clear);
    BF_CHECK(evidence.value().usable());
}

BF_TEST(pressure, state_transitions_follow_exact_thresholds) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;
    BF_CHECK(harness.pressure_state() == PressureState::Clear);

    // Allocate just past the ELEVATED boundary.
    const u64 elevated_units = (usable / 10000ull) * 7000ull;
    auto decision = harness.allocate(elevated_units + 1);
    BF_REQUIRE(decision.ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto evidence = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(evidence.ok());
    BF_CHECK(evidence.value().state == PressureState::Elevated);
    BF_CHECK(evidence.value().utilization_bp >= 7000);

    // Cross the HIGH band.
    BF_REQUIRE(harness.observe_current().ok());
    auto second = harness.allocate(usable * 16 / 100);
    BF_REQUIRE(second.ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto high = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(high.ok());
    BF_CHECK(high.value().state == PressureState::High ||
             high.value().state == PressureState::Critical);
}

BF_TEST(pressure, refusal_band_refuses_new_grants) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.refuse_grants_at = PressureState::High;
    policy.pressure.refuse_increase_on_unknown = false;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;

    auto first = harness.allocate(usable * 88 / 100);
    BF_REQUIRE(first.ok());
    BF_CHECK(first.value().granted_units > 0);
    BF_REQUIRE(harness.observe_current().ok());

    auto second = harness.allocate(1000);
    BF_REQUIRE(second.ok());
    BF_CHECK_EQ(second.value().granted_units, u64{0});
    BF_CHECK(second.value().kind == DecisionKind::Reduce);
    BF_CHECK(second.value().binding == BindingConstraint::PressureRefuse);
    BF_CHECK(second.value().intent.reduce_required);
}

BF_TEST(pressure, unknown_evidence_may_be_waived_by_policy) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.pressure.refuse_increase_on_unknown = false;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());
    BF_CHECK(decision.value().kind == DecisionKind::Grant);
    // The decision still records that the evidence was unknown.
    BF_CHECK(decision.value().pressure == PressureState::Unknown);
}

BF_TEST(pressure, soft_limit_applies_under_high_pressure) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.soft_limit_bp = 5000;
    policy.pressure.refuse_increase_on_unknown = false;
    policy.refuse_grants_at = PressureState::Critical;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    const u64 usable = bftest::kPoolRaw - bftest::kPoolProtected;

    auto first = harness.allocate(usable * 88 / 100);
    BF_REQUIRE(first.ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto evidence = harness.fabric.pressure(harness.pool);
    BF_REQUIRE(evidence.ok());
    BF_REQUIRE(evidence.value().state == PressureState::High ||
               evidence.value().state == PressureState::Critical);

    auto second = harness.allocate(usable * 30 / 100);
    BF_REQUIRE(second.ok());
    // Usage is already past the soft mark, so nothing further is granted.
    BF_CHECK_EQ(second.value().granted_units, u64{0});
}

BF_TEST(reclaim, plan_is_deterministic_and_ordered) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    std::vector<AllocationId> ids;
    for (u64 index = 0; index < 8; ++index) {
        BF_REQUIRE(harness.observe_current().ok());
        AllocateRequest request = harness.allocate_request(1000);
        request.reclaim_priority = index % 3;
        auto decision = harness.fabric.allocate(request);
        BF_REQUIRE(decision.ok());
        BF_REQUIRE(decision.value().granted_units == 1000);
        ids.push_back(decision.value().allocation);
        harness.clock.advance(10);
    }

    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 3000;
    auto first = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(first.ok());
    auto second = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(second.ok());
    BF_CHECK_EQ(first.value().digest, second.value().digest);
    BF_CHECK_EQ(first.value().planned_units, u64{3000});
    BF_CHECK_EQ(first.value().victims.size(), usize{3});
    // Lowest priority wins, then the oldest allocation.
    for (std::size_t index = 0; index < first.value().victims.size(); ++index) {
        BF_CHECK_EQ(first.value().victims[index].reclaim_priority, u64{0});
    }
    BF_CHECK(first.value().victims[0].last_touched <= first.value().victims[1].last_touched);
}

BF_TEST(reclaim, pinned_allocations_are_never_revoked_by_default) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());
    AllocateRequest pinned = harness.allocate_request(5000);
    pinned.reclaim_class = ReclaimClass::Pinned;
    auto pinned_decision = harness.fabric.allocate(pinned);
    BF_REQUIRE(pinned_decision.ok());

    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 5000;
    auto plan = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(plan.ok());
    BF_CHECK_EQ(plan.value().planned_units, u64{0});
    BF_CHECK_EQ(plan.value().shortfall_units, u64{5000});
    BF_CHECK_EQ(plan.value().pinned_available_units, u64{5000});
    BF_CHECK(!plan.value().protected_reclaim_used);

    auto applied = harness.fabric.apply_reclaim(reclaim);
    BF_REQUIRE(applied.ok());
    BF_CHECK_EQ(applied.value().reclaimed_units, u64{0});
    BF_CHECK_EQ(harness.own_usage(), u64{5000});
    auto record = harness.fabric.allocation(pinned_decision.value().allocation);
    BF_REQUIRE(record.ok());
    BF_CHECK(record.value().holds_units());
}

BF_TEST(reclaim, protected_reclaim_requires_explicit_policy) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.reclaim.allow_protected_reclaim = true;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    AllocateRequest pinned = harness.allocate_request(5000);
    pinned.reclaim_class = ReclaimClass::Pinned;
    BF_REQUIRE(harness.fabric.allocate(pinned).ok());

    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 5000;
    auto plan = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(plan.ok());
    BF_CHECK_EQ(plan.value().planned_units, u64{5000});
    BF_CHECK(plan.value().protected_reclaim_used);
    auto applied = harness.fabric.apply_reclaim(reclaim);
    BF_REQUIRE(applied.ok());
    BF_CHECK_EQ(applied.value().reclaimed_units, u64{5000});
    BF_CHECK_EQ(harness.own_usage(), u64{0});
    BF_CHECK(harness.fabric.validate_accounting().ok());
}

BF_TEST(reclaim, reclaimable_allocations_are_revoked_deterministically) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    std::vector<AllocationId> ids;
    for (u64 index = 0; index < 4; ++index) {
        BF_REQUIRE(harness.observe_current().ok());
        auto decision = harness.allocate(1000);
        BF_REQUIRE(decision.ok());
        ids.push_back(decision.value().allocation);
        harness.clock.advance(10);
    }
    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 2000;
    auto applied = harness.fabric.apply_reclaim(reclaim);
    BF_REQUIRE(applied.ok());
    BF_CHECK_EQ(applied.value().reclaimed_units, u64{2000});
    BF_CHECK_EQ(applied.value().revoked_allocations, u64{2});
    BF_CHECK_EQ(harness.own_usage(), u64{2000});
    BF_CHECK(harness.fabric.validate_accounting().ok());

    auto first = harness.fabric.allocation(ids[0]);
    BF_REQUIRE(first.ok());
    BF_CHECK(first.value().state == AllocationState::Reclaimed);
    auto fourth = harness.fabric.allocation(ids[3]);
    BF_REQUIRE(fourth.ok());
    BF_CHECK(fourth.value().holds_units());
}

BF_TEST(reclaim, reserve_extends_the_plan_beyond_the_target) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.reclaim.reserve_units = 40'000;
    policy.pressure.refuse_increase_on_unknown = false;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(100'000);
    BF_REQUIRE(decision.ok());
    BF_CHECK_EQ(harness.free_units(), bftest::kPoolRaw - bftest::kPoolProtected - 100'000);

    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 1;
    auto plan = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(plan.ok());
    // The plan must free enough to satisfy both the target and the reserve.
    BF_CHECK(plan.value().planned_units >= 1);
    BF_CHECK(plan.value().planned_units <= 100'000);
    BF_CHECK(harness.free_units() + plan.value().planned_units >= policy.reclaim.reserve_units ||
             plan.value().planned_units == 100'000);
}

BF_TEST(reclaim, min_hold_ticks_excludes_recent_allocations) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.reclaim.min_hold_ticks = 1'000'000;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    auto decision = harness.allocate(1000);
    BF_REQUIRE(decision.ok());

    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 1000;
    auto plan = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(plan.ok());
    BF_CHECK_EQ(plan.value().planned_units, u64{0});

    harness.clock.advance(2'000'000);
    auto later = harness.fabric.plan_reclaim(reclaim);
    BF_REQUIRE(later.ok());
    BF_CHECK_EQ(later.value().planned_units, u64{1000});
}

BF_TEST(reclaim, disabled_policy_refuses_reclamation) {
    Harness harness;
    PoolPolicy policy = harness.policy;
    policy.reclaim.enabled = false;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 10;
    BF_CHECK_CODE(harness.fabric.plan_reclaim(reclaim), ErrorCode::PolicyForbids);
}

BF_TEST(reclaim, zero_target_is_refused) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    ReclaimRequest reclaim;
    reclaim.attempt = harness.next_attempt();
    reclaim.pool = harness.pool;
    reclaim.target_units = 0;
    BF_CHECK_CODE(harness.fabric.plan_reclaim(reclaim), ErrorCode::InvalidArgument);
}
