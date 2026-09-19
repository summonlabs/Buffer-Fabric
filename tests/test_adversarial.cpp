#include <string>
#include <vector>

#include "buffer_fabric/transport.hpp"
#include "buffer_fabric/version.hpp"
#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::Harness;

BF_TEST(adversarial, zero_and_out_of_range_allocations_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_REQUIRE(harness.observe_current().ok());

    BF_CHECK_CODE(harness.allocate(0), ErrorCode::InvalidArgument);

    AllocateRequest request = harness.allocate_request(kMaxUnitsPerPool + 1);
    BF_CHECK_CODE(harness.fabric.allocate(request), ErrorCode::Overflow);

    AllocateRequest no_attempt = harness.allocate_request(100);
    no_attempt.attempt = AttemptId{};
    BF_CHECK_CODE(harness.fabric.allocate(no_attempt), ErrorCode::InvalidArgument);

    AllocateRequest no_pool = harness.allocate_request(100);
    no_pool.pool = PoolId{};
    BF_CHECK_CODE(harness.fabric.allocate(no_pool), ErrorCode::InvalidArgument);

    AllocateRequest unknown_pool = harness.allocate_request(100);
    unknown_pool.pool = PoolId::from_raw(9999);
    BF_CHECK_CODE(harness.fabric.allocate(unknown_pool), ErrorCode::UnknownPool);

    AllocateRequest unknown_queue = harness.allocate_request(100);
    unknown_queue.queue = QueueId::from_raw(9999);
    BF_CHECK_CODE(harness.fabric.allocate(unknown_queue), ErrorCode::UnknownQueue);

    AllocateRequest detached_queue = harness.allocate_request(100);
    auto other_pool = harness.add_pool(100'000, 0);
    BF_REQUIRE(other_pool.ok());
    auto other_queue = harness.add_queue(other_pool.value(), 1000);
    BF_REQUIRE(other_queue.ok());
    detached_queue.queue = other_queue.value();
    BF_CHECK_CODE(harness.fabric.allocate(detached_queue), ErrorCode::InvalidArgument);
}

BF_TEST(adversarial, structurally_invalid_pools_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());

    PoolRegistration protected_too_large;
    protected_too_large.name = "bad-protected";
    protected_too_large.resource = harness.resource;
    protected_too_large.backend = harness.backend.id();
    protected_too_large.raw_units = 1000;
    protected_too_large.protected_units = 1001;
    protected_too_large.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(protected_too_large),
                  ErrorCode::ProtectedHeadroomViolation);

    PoolRegistration entirely_protected = protected_too_large;
    entirely_protected.name = "all-protected";
    entirely_protected.protected_units = 1000;
    BF_CHECK_CODE(harness.fabric.register_pool(entirely_protected),
                  ErrorCode::ProtectedHeadroomViolation);

    PoolRegistration zero = protected_too_large;
    zero.name = "zero";
    zero.protected_units = 0;
    zero.raw_units = 0;
    BF_CHECK_CODE(harness.fabric.register_pool(zero), ErrorCode::InvalidArgument);

    PoolRegistration self_parent = protected_too_large;
    self_parent.name = "self-parent";
    self_parent.protected_units = 0;
    self_parent.id = PoolId::from_raw(4242);
    self_parent.parent = PoolId::from_raw(4242);
    BF_CHECK_CODE(harness.fabric.register_pool(self_parent), ErrorCode::PoolCycle);

    PoolRegistration unknown_parent = protected_too_large;
    unknown_parent.name = "unknown-parent";
    unknown_parent.protected_units = 0;
    unknown_parent.parent = PoolId::from_raw(7777);
    BF_CHECK_CODE(harness.fabric.register_pool(unknown_parent), ErrorCode::UnknownPool);

    PoolRegistration unknown_policy = protected_too_large;
    unknown_policy.name = "unknown-policy";
    unknown_policy.protected_units = 0;
    unknown_policy.policy = PolicyId::from_raw(7777);
    BF_CHECK_CODE(harness.fabric.register_pool(unknown_policy), ErrorCode::UnknownPolicy);

    PoolRegistration bad_name = protected_too_large;
    bad_name.name = "bad name with spaces";
    bad_name.protected_units = 0;
    BF_CHECK_CODE(harness.fabric.register_pool(bad_name), ErrorCode::NameTooLong);

    PoolRegistration long_name = protected_too_large;
    long_name.name = std::string(kMaxNameBytes + 1, 'a');
    long_name.protected_units = 0;
    BF_CHECK_CODE(harness.fabric.register_pool(long_name), ErrorCode::NameTooLong);
}

BF_TEST(adversarial, unknown_backend_and_resource_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());

    PoolRegistration registration;
    registration.name = "no-backend";
    registration.resource = harness.resource;
    registration.backend = BackendId::from_raw(42);
    registration.raw_units = 1000;
    registration.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(registration), ErrorCode::UnknownBackend);

    BF_CHECK_CODE(harness.fabric.set_capacity(ResourceId::from_raw(9), harness.backend.id(), 0,
                                              Generation{}),
                  ErrorCode::UnknownResource);
    BF_CHECK_CODE(harness.fabric.set_capacity(ResourceId{}, harness.backend.id(), 0, Generation{}),
                  ErrorCode::InvalidArgument);
    BF_CHECK_CODE(harness.fabric.set_capacity(harness.resource, BackendId::from_raw(42), 0,
                                              Generation{}),
                  ErrorCode::UnknownBackend);
}

BF_TEST(adversarial, not_ready_backend_yields_unknown_capacity_not_zero) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    harness.backend.set_ready(false);
    BF_CHECK_CODE(harness.fabric.set_capacity(harness.resource, harness.backend.id(), 0,
                                              Generation{}),
                  ErrorCode::BackendNotReady);
    harness.backend.set_ready(true);
}

BF_TEST(adversarial, null_construction_paths_are_rejected) {
    BufferFabric fabric;
    BF_REQUIRE(fabric.open(FabricConfig{}).ok());
    BF_CHECK_CODE(fabric.attach_backend(nullptr), ErrorCode::InvalidArgument);
    BF_CHECK_CODE(fabric.explain(PoolId{}), ErrorCode::InvalidArgument);
    BF_CHECK_CODE(fabric.allocation(AllocationId{}), ErrorCode::InvalidArgument);
    BF_CHECK_CODE(fabric.policy(PolicyId{}), ErrorCode::InvalidArgument);
    BF_CHECK_CODE(fabric.remove_pool(PoolId{}, Generation{}), ErrorCode::InvalidArgument);
    BF_CHECK_CODE(fabric.pressure(PoolId{}), ErrorCode::InvalidArgument);
    BF_CHECK(fabric.close().ok());
}

BF_TEST(adversarial, fabric_config_bounds_are_enforced) {
    BufferFabric fabric;
    FabricConfig config;
    config.decision_history_capacity = 0;
    BF_CHECK_CODE(fabric.open(config), ErrorCode::InvalidArgument);
    config = FabricConfig{};
    config.attempt_memory_capacity = kMaxAttemptMemory + 1;
    BF_CHECK_CODE(fabric.open(config), ErrorCode::InvalidArgument);
    config = FabricConfig{};
    config.decision_history_capacity = 1;
    BF_CHECK(fabric.open(config).ok());
    BF_CHECK(fabric.close().ok());
}

BF_TEST(adversarial, malformed_transport_messages_are_rejected) {
    Message message;
    message.op = MessageOp::Ping;
    message.request = RequestId::from_raw(1);
    message.body = {1, 2, 3};
    const std::vector<u8> frame = encode_message(message);

    Message decoded;
    usize consumed = 0;
    BF_CHECK(decode_message(frame.data(), frame.size(), kMaxFrameBytes, &consumed).ok());

    // Wrong magic.
    std::vector<u8> bad_magic = frame;
    bad_magic[0] ^= 0xFF;
    BF_CHECK_CODE(decode_message(bad_magic.data(), bad_magic.size(), kMaxFrameBytes, &consumed),
                  ErrorCode::CorruptRecord);

    // Truncated payload.
    BF_CHECK_CODE(decode_message(frame.data(), frame.size() - 1, kMaxFrameBytes, &consumed),
                  ErrorCode::TruncatedMessage);

    // Corrupted payload.
    std::vector<u8> corrupt = frame;
    corrupt[kFrameHeaderBytes + 3] ^= 0x11;
    BF_CHECK_CODE(decode_message(corrupt.data(), corrupt.size(), kMaxFrameBytes, &consumed),
                  ErrorCode::IntegrityFailure);

    // Body above the declared bound.
    BF_CHECK_CODE(decode_message(frame.data(), frame.size(), 1, &consumed),
                  ErrorCode::OversizedMessage);

    // Structurally invalid body: a valid frame whose body declares more bytes
    // than it carries.
    ByteWriter hostile;
    hostile.put_u8(static_cast<u8>(MessageOp::Ping));
    hostile.put_u64(1);
    hostile.put_u64(0);
    hostile.put_u32(64);
    hostile.put_bytes("short", 5);
    const std::vector<u8> hostile_frame =
        encode_frame(hostile.data().data(), hostile.size(), kTransportMagic,
                     BUFFER_FABRIC_WIRE_FORMAT_VERSION);
    BF_CHECK_CODE(decode_message(hostile_frame.data(), hostile_frame.size(), kMaxFrameBytes,
                                 &consumed),
                  ErrorCode::TruncatedMessage);
}

BF_TEST(adversarial, transport_rejects_unknown_version) {
    ByteWriter body;
    body.put_u8(static_cast<u8>(MessageOp::Ping));
    body.put_u64(1);
    body.put_u64(0);
    body.put_u32(0);
    const std::vector<u8> frame =
        encode_frame(body.data().data(), body.size(), kTransportMagic, 99u);
    Message decoded;
    usize consumed = 0;
    // The frame layer rejects an unsupported wire version before the message
    // layer is reached.
    BF_CHECK_CODE(decode_message(frame.data(), frame.size(), kMaxFrameBytes, &consumed),
                  ErrorCode::VersionMismatch);
}

BF_TEST(adversarial, malformed_pressure_snapshots_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    PressureSnapshot snapshot;
    snapshot.pool = PoolId::from_raw(1234);
    snapshot.epoch = harness.fabric.epoch();
    BF_CHECK_CODE(harness.fabric.observe_pressure(snapshot), ErrorCode::UnknownPool);

    snapshot.pool = harness.pool;
    snapshot.demand_units = kMaxUnitsPerPool + 1;
    snapshot.epoch = harness.fabric.epoch();
    snapshot.pool_generation = Generation::from_raw(1);
    snapshot.observed_at = harness.clock.now_ticks();
    BF_CHECK_CODE(harness.fabric.observe_pressure(snapshot), ErrorCode::Overflow);
}

BF_TEST(adversarial, duplicate_and_conflicting_registrations_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());

    PoolRegistration duplicate_id;
    duplicate_id.id = harness.pool;
    duplicate_id.name = "another-name";
    duplicate_id.resource = harness.resource;
    duplicate_id.backend = harness.backend.id();
    duplicate_id.raw_units = 1000;
    duplicate_id.policy = harness.policy_id;
    BF_CHECK_CODE(harness.fabric.register_pool(duplicate_id), ErrorCode::DuplicatePool);

    QueueRegistration duplicate_queue;
    duplicate_queue.id = harness.queue;
    duplicate_queue.name = "another-queue";
    duplicate_queue.pool = harness.pool;
    BF_CHECK_CODE(harness.fabric.register_queue(duplicate_queue), ErrorCode::DuplicateQueue);
}

BF_TEST(adversarial, queue_operations_on_retired_or_unknown_queues_are_rejected) {
    Harness harness;
    BF_REQUIRE(harness.open().ok());
    BF_CHECK_CODE(harness.fabric.update_queue_demand(QueueId::from_raw(9999), 1, Generation{}),
                  ErrorCode::UnknownQueue);
    BF_CHECK_CODE(harness.fabric.retire_queue(QueueId::from_raw(9999), Generation{}),
                  ErrorCode::UnknownQueue);
    BF_REQUIRE(harness.fabric.retire_queue(harness.queue, Generation::from_raw(1)).ok());
    BF_CHECK_CODE(harness.fabric.update_queue_demand(harness.queue, 10, Generation{}),
                  ErrorCode::WrongState);
    BF_REQUIRE(harness.fabric.retire_queue(harness.queue, Generation{}).ok());
}

BF_TEST(adversarial, backend_programming_is_opt_in_and_recorded) {
    {
        // Default configuration: the runtime never touches the hooks.
        Harness harness;
        BF_REQUIRE(harness.open().ok());
        BF_CHECK_EQ(harness.backend.effects().reservation_calls, u64{0});
        BF_CHECK_EQ(harness.fabric.metrics().backend_effects_attempted, u64{0});
    }
    {
        // Opted in: the attempt is made and recorded, and a backend that owns
        // no programmable memory is tolerated rather than assumed to succeed.
        Harness harness;
        FabricConfig config;
        config.program_backend_effects = true;
        BF_REQUIRE(harness.open(config).ok());
        BF_CHECK_EQ(harness.backend.effects().reservation_calls, u64{1});
        const FabricMetrics metrics = harness.fabric.metrics();
        BF_CHECK_EQ(metrics.backend_effects_attempted, u64{1});
        BF_CHECK_EQ(metrics.backend_effects_unsupported, u64{1});
        BF_CHECK_EQ(harness.backend.effects().unsupported_calls, u64{1});
    }
}

BF_TEST(adversarial, capabilities_are_reported_honestly) {
    Capabilities caps = capabilities();
    BF_CHECK(caps.durability);
    BF_CHECK(caps.multiprocess_transport);
    // No physical device backend ships with this release.
    BF_CHECK(!caps.physical_device_backend);
    BF_CHECK(!caps.distributed_consensus);
    const std::string json = caps.to_json();
    BF_CHECK(json.find("\"physical_device_backend\":false") != std::string::npos);
    BF_CHECK(json.find("\"version\":\"1.0.0\"") != std::string::npos);
}

BF_TEST(adversarial, decision_history_is_bounded) {
    Harness harness;
    FabricConfig config;
    config.decision_history_capacity = 8;
    BF_REQUIRE(harness.open(config).ok());
    harness.events.clear();
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    for (int index = 0; index < 64; ++index) {
        AllocateRequest request = harness.allocate_request(16);
        auto decision = harness.fabric.allocate(request);
        BF_REQUIRE(decision.ok());
    }
    auto history = harness.fabric.decision_history(1000);
    BF_REQUIRE(history.ok());
    BF_CHECK_EQ(history.value().size(), usize{8});
}

BF_TEST(adversarial, attempt_memory_is_bounded) {
    Harness harness;
    FabricConfig config;
    config.attempt_memory_capacity = 4;
    BF_REQUIRE(harness.open(config).ok());
    PoolPolicy policy = harness.policy;
    policy.pressure.evidence_ttl_ticks = 1ull << 62;
    BF_REQUIRE(harness.fabric.publish_policy(policy).ok());
    BF_REQUIRE(harness.observe_current().ok());
    for (int index = 0; index < 32; ++index) {
        AllocateRequest request = harness.allocate_request(16);
        auto decision = harness.fabric.allocate(request);
        BF_REQUIRE(decision.ok());
        if (decision.value().granted_units != 0) {
            BF_REQUIRE(harness.release(decision.value().allocation).ok());
        }
    }
    auto attempts = harness.fabric.attempt_history(1000);
    BF_REQUIRE(attempts.ok());
    BF_CHECK(attempts.value().size() <= usize{4});
}
