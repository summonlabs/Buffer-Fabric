#include "buffer_fabric/worker.hpp"

#include <string>

#include "buffer_fabric/transport.hpp"
#include "protocol.hpp"

namespace buffer_fabric {

Result<WorkerReport> run_worker(const WorkerConfig& config) {
    WorkerReport report;
    if (config.port == 0) {
        return Status(ErrorCode::InvalidArgument, "worker port must be non-zero");
    }
    if (config.operations > (1ull << 24)) {
        return Status(ErrorCode::BoundedResourceExhausted, "worker operation count is too large");
    }
    BF_TRY(ensure_sockets_initialised());

    auto connected = Socket::connect_to(config.host, config.port);
    if (!connected.ok()) return connected.status();
    Socket socket = std::move(connected).value();
    report.connected = true;

    auto exchange = [&](const Message& request, Message* reply) -> Status {
        const std::vector<u8> frame = encode_message(request);
        report.bytes_sent += frame.size();
        BF_TRY(socket.send_all(frame.data(), frame.size()));
        auto incoming = socket.receive_frame(kMaxFrameBytes);
        if (!incoming.ok()) return incoming.status();
        report.bytes_received += incoming.value().size();
        auto decoded = decode_message_payload(incoming.value().data(), incoming.value().size(),
                                              kMaxFrameBytes);
        if (!decoded.ok()) return decoded.status();
        *reply = std::move(decoded).value();
        return Status::success();
    };

    Message hello;
    hello.op = MessageOp::Hello;
    hello.request = RequestId::from_raw(1);
    hello.epoch = config.claim_epoch;
    {
        ByteWriter body;
        protocol::HelloRequest handshake;
        handshake.protocol_version = protocol::kProtocolVersion;
        handshake.claimed_epoch = config.claim_epoch;
        protocol::encode_hello_request(&body, handshake);
        hello.body = body.take();
    }
    Message hello_reply;
    BF_TRY(exchange(hello, &hello_reply));
    ByteReader hello_reader(hello_reply.body);
    ErrorCode hello_status = ErrorCode::Ok;
    std::string hello_message;
    std::vector<u8> hello_payload;
    if (!protocol::decode_response(&hello_reader, &hello_status, &hello_message, &hello_payload)) {
        report.protocol_error = true;
        return Status(ErrorCode::ProtocolViolation, "hello response is not decodable");
    }
    if (hello_status == ErrorCode::StaleEpoch) {
        report.stale_epoch_rejected = true;
        return report;
    }
    if (hello_status != ErrorCode::Ok) {
        report.protocol_error = true;
        return Status(hello_status, hello_message);
    }
    if (hello_reply.op != MessageOp::HelloAck) {
        report.protocol_error = true;
        return Status(ErrorCode::ProtocolViolation, "handshake did not produce an acknowledgement");
    }
    {
        ByteReader reader(hello_payload);
        protocol::HelloResponse ack;
        if (!protocol::decode_hello_response(&reader, &ack)) {
            report.protocol_error = true;
            return Status(ErrorCode::ProtocolViolation, "hello acknowledgement is not decodable");
        }
        if (ack.protocol_version != protocol::kProtocolVersion) {
            report.protocol_error = true;
            return Status(ErrorCode::ProtocolVersionMismatch, "coordinator protocol version differs");
        }
        report.coordinator_epoch = ack.coordinator_epoch;
        report.coordinator_boot = ack.coordinator_boot;
    }
    report.handshake_ok = true;

    const EpochId request_epoch =
        config.expected_epoch.valid() ? config.expected_epoch : report.coordinator_epoch;

    u64 attempt_sequence = config.attempt_base + 1;
    for (u64 index = 0; index < config.operations; ++index) {
        AllocateRequest allocate_request;
        allocate_request.attempt = AttemptId::from_raw(attempt_sequence++);
        allocate_request.pool = config.pool;
        allocate_request.queue = config.queue;
        allocate_request.requested_units = config.units;
        allocate_request.expected_epoch = request_epoch;
        Message allocate;
        allocate.op = MessageOp::Allocate;
        allocate.request = RequestId::from_raw(attempt_sequence);
        allocate.epoch = request_epoch;
        {
            ByteWriter body;
            protocol::encode_allocate(&body, allocate_request);
            allocate.body = body.take();
        }
        Message allocate_reply;
        BF_TRY(exchange(allocate, &allocate_reply));
        ByteReader allocate_reader(allocate_reply.body);
        ErrorCode allocate_status = ErrorCode::Ok;
        std::string allocate_message;
        std::vector<u8> allocate_payload;
        if (!protocol::decode_response(&allocate_reader, &allocate_status, &allocate_message,
                                       &allocate_payload)) {
            report.protocol_error = true;
            return Status(ErrorCode::ProtocolViolation, "allocate response is not decodable");
        }
        report.operations_attempted += 1;
        if (allocate_status == ErrorCode::StaleEpoch) {
            report.stale_epoch_rejected = true;
            return report;
        }
        if (allocate_status != ErrorCode::Ok) {
            report.error_responses += 1;
            return Status(allocate_status, allocate_message);
        }
        protocol::DecisionSummary summary;
        {
            ByteReader reader(allocate_payload);
            if (!protocol::decode_decision_summary(&reader, &summary)) {
                report.protocol_error = true;
                return Status(ErrorCode::ProtocolViolation, "decision summary is not decodable");
            }
        }
        report.accounting.closed = true;
        report.accounting.raw_total = summary.accounting.raw;
        report.accounting.protected_total = summary.accounting.protected_units;
        report.accounting.allocated_total = summary.accounting.allocated;
        report.accounting.free_total = summary.accounting.free;
        report.accounting.borrowed_total = summary.accounting.borrowed_in;
        report.accounting.lent_total = summary.accounting.lent_out;
        report.accounting.overcommit_total = summary.accounting.overcommit_used;
        report.accounting.pools_observed = 1;
        if (summary.granted_units == 0 || summary.status != ErrorCode::Ok) {
            report.refusals += 1;
            continue;
        }
        if (summary.kind == DecisionKind::Grant) {
            report.grants += 1;
        } else if (summary.kind == DecisionKind::GrantPartial) {
            report.partial_grants += 1;
        }
        report.granted_units += summary.granted_units;

        ReleaseRequest release_request;
        release_request.attempt = AttemptId::from_raw(attempt_sequence++);
        release_request.allocation = summary.allocation;
        release_request.release_all = true;
        release_request.expected_epoch = request_epoch;
        Message release;
        release.op = MessageOp::Release;
        release.request = RequestId::from_raw(attempt_sequence);
        release.epoch = request_epoch;
        {
            ByteWriter body;
            protocol::encode_release(&body, release_request);
            release.body = body.take();
        }
        Message release_reply;
        BF_TRY(exchange(release, &release_reply));
        ByteReader release_reader(release_reply.body);
        ErrorCode release_status = ErrorCode::Ok;
        std::string release_message;
        std::vector<u8> release_payload;
        if (!protocol::decode_response(&release_reader, &release_status, &release_message,
                                       &release_payload)) {
            report.protocol_error = true;
            return Status(ErrorCode::ProtocolViolation, "release response is not decodable");
        }
        if (release_status != ErrorCode::Ok) {
            report.error_responses += 1;
            return Status(release_status, release_message);
        }
        protocol::DecisionSummary release_summary;
        {
            ByteReader reader(release_payload);
            if (!protocol::decode_decision_summary(&reader, &release_summary)) {
                report.protocol_error = true;
                return Status(ErrorCode::ProtocolViolation, "release summary is not decodable");
            }
        }
        report.release_ok += 1;
        report.released_units += release_summary.granted_units;
    }

    socket.shutdown();
    socket.close();
    return report;
}

}  // namespace buffer_fabric
