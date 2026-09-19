#include "protocol.hpp"

namespace buffer_fabric::protocol {

namespace {

void put_id(ByteWriter* writer, u64 value) { writer->put_u64(value); }

bool get_id(ByteReader* reader, u64* value) { return reader->get_u64(value); }

}  // namespace

void encode_hello_request(ByteWriter* writer, const HelloRequest& message) {
    writer->put_u32(message.protocol_version);
    put_id(writer, message.claimed_epoch.raw());
    writer->put_u64(message.client_boot.hi);
    writer->put_u64(message.client_boot.lo);
}

bool decode_hello_request(ByteReader* reader, HelloRequest* message) {
    if (!reader->get_u32(&message->protocol_version)) return false;
    u64 epoch = 0;
    if (!get_id(reader, &epoch)) return false;
    message->claimed_epoch = EpochId::from_raw(epoch);
    if (!reader->get_u64(&message->client_boot.hi)) return false;
    if (!reader->get_u64(&message->client_boot.lo)) return false;
    return true;
}

void encode_hello_response(ByteWriter* writer, const HelloResponse& message) {
    writer->put_u32(message.protocol_version);
    put_id(writer, message.coordinator_epoch.raw());
    writer->put_u64(message.coordinator_boot.hi);
    writer->put_u64(message.coordinator_boot.lo);
    writer->put_u64(message.pool_count);
    writer->put_u64(message.queue_count);
}

bool decode_hello_response(ByteReader* reader, HelloResponse* message) {
    if (!reader->get_u32(&message->protocol_version)) return false;
    u64 epoch = 0;
    if (!get_id(reader, &epoch)) return false;
    message->coordinator_epoch = EpochId::from_raw(epoch);
    if (!reader->get_u64(&message->coordinator_boot.hi)) return false;
    if (!reader->get_u64(&message->coordinator_boot.lo)) return false;
    if (!reader->get_u64(&message->pool_count)) return false;
    if (!reader->get_u64(&message->queue_count)) return false;
    return true;
}

void encode_allocate(ByteWriter* writer, const AllocateRequest& request) {
    put_id(writer, request.attempt.raw());
    put_id(writer, request.pool.raw());
    put_id(writer, request.queue.raw());
    writer->put_u64(request.requested_units);
    writer->put_u8(static_cast<u8>(request.reclaim_class));
    writer->put_u64(request.reclaim_priority);
    writer->put_u64(request.ttl_ticks);
    writer->put_bool(request.commit_immediately);
    put_id(writer, request.provenance.raw());
    put_id(writer, request.expected_pool_generation.raw());
    put_id(writer, request.expected_policy_generation.raw());
    put_id(writer, request.expected_queue_generation.raw());
    put_id(writer, request.expected_capacity_generation.raw());
    put_id(writer, request.expected_epoch.raw());
}

bool decode_allocate(ByteReader* reader, AllocateRequest* request) {
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    request->attempt = AttemptId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->pool = PoolId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->queue = QueueId::from_raw(value);
    if (!reader->get_u64(&request->requested_units)) return false;
    u8 klass = 0;
    if (!reader->get_u8(&klass)) return false;
    if (klass > static_cast<u8>(ReclaimClass::Reclaimable)) return false;
    request->reclaim_class = static_cast<ReclaimClass>(klass);
    if (!reader->get_u64(&request->reclaim_priority)) return false;
    if (!reader->get_u64(&request->ttl_ticks)) return false;
    if (!reader->get_bool(&request->commit_immediately)) return false;
    if (!get_id(reader, &value)) return false;
    request->provenance = ProvenanceId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_pool_generation = Generation::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_policy_generation = Generation::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_queue_generation = Generation::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_capacity_generation = Generation::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_epoch = EpochId::from_raw(value);
    return true;
}

void encode_release(ByteWriter* writer, const ReleaseRequest& request) {
    put_id(writer, request.attempt.raw());
    put_id(writer, request.allocation.raw());
    writer->put_bool(request.release_all);
    writer->put_u64(request.units);
    put_id(writer, request.expected_pool_generation.raw());
    put_id(writer, request.expected_epoch.raw());
}

bool decode_release(ByteReader* reader, ReleaseRequest* request) {
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    request->attempt = AttemptId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->allocation = AllocationId::from_raw(value);
    if (!reader->get_bool(&request->release_all)) return false;
    if (!reader->get_u64(&request->units)) return false;
    if (!get_id(reader, &value)) return false;
    request->expected_pool_generation = Generation::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->expected_epoch = EpochId::from_raw(value);
    return true;
}

void encode_evaluate(ByteWriter* writer, const EvaluateRequest& request) {
    put_id(writer, request.pool.raw());
    put_id(writer, request.queue.raw());
    writer->put_u64(request.requested_units);
    writer->put_u8(static_cast<u8>(request.reclaim_class));
    put_id(writer, request.expected_epoch.raw());
}

bool decode_evaluate(ByteReader* reader, EvaluateRequest* request) {
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    request->pool = PoolId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    request->queue = QueueId::from_raw(value);
    if (!reader->get_u64(&request->requested_units)) return false;
    u8 klass = 0;
    if (!reader->get_u8(&klass)) return false;
    if (klass > static_cast<u8>(ReclaimClass::Reclaimable)) return false;
    request->reclaim_class = static_cast<ReclaimClass>(klass);
    if (!get_id(reader, &value)) return false;
    request->expected_epoch = EpochId::from_raw(value);
    return true;
}

void encode_pressure(ByteWriter* writer, const PressureSnapshot& snapshot) {
    put_id(writer, snapshot.pool.raw());
    writer->put_u64(snapshot.observed_at);
    writer->put_u64(snapshot.ttl_ticks);
    writer->put_u64(snapshot.demand_units);
    writer->put_u64(snapshot.committed_units);
    writer->put_u64(snapshot.reclaimable_units);
    put_id(writer, snapshot.provenance.raw());
    writer->put_bool(snapshot.assert_committed);
}

bool decode_pressure(ByteReader* reader, PressureSnapshot* snapshot) {
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    snapshot->pool = PoolId::from_raw(value);
    if (!reader->get_u64(&snapshot->observed_at)) return false;
    if (!reader->get_u64(&snapshot->ttl_ticks)) return false;
    if (!reader->get_u64(&snapshot->demand_units)) return false;
    if (!reader->get_u64(&snapshot->committed_units)) return false;
    if (!reader->get_u64(&snapshot->reclaimable_units)) return false;
    if (!get_id(reader, &value)) return false;
    snapshot->provenance = ProvenanceId::from_raw(value);
    if (!reader->get_bool(&snapshot->assert_committed)) return false;
    return true;
}

void encode_accounting(ByteWriter* writer, const PoolAccounting& accounting) {
    put_id(writer, accounting.pool.raw());
    put_id(writer, accounting.pool_generation.raw());
    writer->put_u64(accounting.raw);
    writer->put_u64(accounting.protected_units);
    writer->put_u64(accounting.usable);
    writer->put_u64(accounting.allocated);
    writer->put_u64(accounting.reserved);
    writer->put_u64(accounting.committed);
    writer->put_u64(accounting.pinned);
    writer->put_u64(accounting.reclaimable);
    writer->put_u64(accounting.borrowed_in);
    writer->put_u64(accounting.lent_out);
    writer->put_u64(accounting.overcommit_used);
    writer->put_u64(accounting.overcommit_limit);
    writer->put_u64(accounting.free);
}

bool decode_accounting(ByteReader* reader, PoolAccounting* accounting) {
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    accounting->pool = PoolId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    accounting->pool_generation = Generation::from_raw(value);
    if (!reader->get_u64(&accounting->raw)) return false;
    if (!reader->get_u64(&accounting->protected_units)) return false;
    if (!reader->get_u64(&accounting->usable)) return false;
    if (!reader->get_u64(&accounting->allocated)) return false;
    if (!reader->get_u64(&accounting->reserved)) return false;
    if (!reader->get_u64(&accounting->committed)) return false;
    if (!reader->get_u64(&accounting->pinned)) return false;
    if (!reader->get_u64(&accounting->reclaimable)) return false;
    if (!reader->get_u64(&accounting->borrowed_in)) return false;
    if (!reader->get_u64(&accounting->lent_out)) return false;
    if (!reader->get_u64(&accounting->overcommit_used)) return false;
    if (!reader->get_u64(&accounting->overcommit_limit)) return false;
    if (!reader->get_u64(&accounting->free)) return false;
    return true;
}

void encode_decision_summary(ByteWriter* writer, const DecisionSummary& summary) {
    writer->put_u8(static_cast<u8>(summary.kind));
    writer->put_u16(static_cast<u16>(summary.status));
    writer->put_u8(summary.binding);
    writer->put_u8(summary.pressure);
    writer->put_u32(summary.utilization_bp);
    writer->put_u64(summary.sequence);
    writer->put_u64(summary.requested_units);
    writer->put_u64(summary.granted_units);
    put_id(writer, summary.pool.raw());
    put_id(writer, summary.queue.raw());
    put_id(writer, summary.allocation.raw());
    encode_accounting(writer, summary.accounting);
}

bool decode_decision_summary(ByteReader* reader, DecisionSummary* summary) {
    u8 kind = 0;
    if (!reader->get_u8(&kind)) return false;
    if (kind > static_cast<u8>(DecisionKind::IdempotentReplay)) return false;
    summary->kind = static_cast<DecisionKind>(kind);
    u16 status = 0;
    if (!reader->get_u16(&status)) return false;
    if (status > static_cast<u16>(ErrorCode::ProtocolVersionMismatch)) return false;
    summary->status = static_cast<ErrorCode>(status);
    if (!reader->get_u8(&summary->binding)) return false;
    if (summary->binding > static_cast<u8>(BindingConstraint::Shutdown)) return false;
    if (!reader->get_u8(&summary->pressure)) return false;
    if (summary->pressure > static_cast<u8>(PressureState::Stale)) return false;
    if (!reader->get_u32(&summary->utilization_bp)) return false;
    if (!reader->get_u64(&summary->sequence)) return false;
    if (!reader->get_u64(&summary->requested_units)) return false;
    if (!reader->get_u64(&summary->granted_units)) return false;
    u64 value = 0;
    if (!get_id(reader, &value)) return false;
    summary->pool = PoolId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    summary->queue = QueueId::from_raw(value);
    if (!get_id(reader, &value)) return false;
    summary->allocation = AllocationId::from_raw(value);
    return decode_accounting(reader, &summary->accounting);
}

void encode_response(ByteWriter* writer, ErrorCode status, std::string_view message,
                     const ByteWriter* payload) {
    const usize length = message.size() > 4096 ? usize{4096} : message.size();
    writer->put_u16(static_cast<u16>(status));
    writer->put_u16(static_cast<u16>(length));
    writer->put_bytes(message.data(), length);
    if (payload != nullptr) {
        writer->put_u32(static_cast<u32>(payload->size()));
        writer->put_bytes(payload->data().data(), payload->size());
    } else {
        writer->put_u32(0);
    }
}

bool decode_response(ByteReader* reader, ErrorCode* status, std::string* message,
                     std::vector<u8>* payload) {
    u16 code = 0;
    u16 length = 0;
    if (!reader->get_u16(&code)) return false;
    if (code > static_cast<u16>(ErrorCode::ProtocolVersionMismatch)) return false;
    if (!reader->get_u16(&length)) return false;
    if (length > 4096) return false;
    const u8* bytes = nullptr;
    if (!reader->get_bytes(&bytes, length)) return false;
    message->assign(reinterpret_cast<const char*>(bytes), length);
    *status = static_cast<ErrorCode>(code);
    u32 payload_bytes = 0;
    if (!reader->get_u32(&payload_bytes)) return false;
    if (payload_bytes > kMaxFrameBytes) return false;
    const u8* body = nullptr;
    if (!reader->get_bytes(&body, payload_bytes)) return false;
    payload->assign(body, body + payload_bytes);
    return true;
}

}  // namespace buffer_fabric::protocol
