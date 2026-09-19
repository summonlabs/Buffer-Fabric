#pragma once

// Wire encodings shared by the coordinator and the worker. Kept in one place
// so that a request or response has exactly one definition.

#include "buffer_fabric/accounting.hpp"
#include "buffer_fabric/codec.hpp"
#include "buffer_fabric/decision.hpp"
#include "buffer_fabric/reclaim.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/transport.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric::protocol {

inline constexpr u32 kProtocolVersion = 1;

struct HelloRequest {
    u32 protocol_version{kProtocolVersion};
    EpochId claimed_epoch{};
    BootIncarnation client_boot{};
};

struct HelloResponse {
    u32 protocol_version{kProtocolVersion};
    EpochId coordinator_epoch{};
    BootIncarnation coordinator_boot{};
    u64 pool_count{0};
    u64 queue_count{0};
};

struct AllocateRequestWire {
    AllocateRequest request{};
};

struct ReleaseRequestWire {
    ReleaseRequest request{};
};

struct EvaluateRequestWire {
    EvaluateRequest request{};
};

struct PressureRequestWire {
    PressureSnapshot snapshot{};
};

/// Response body for Ledger: a bounded list of pool accounting rows.
struct LedgerResponseWire {
    u64 pool_count{0};
};

struct DecisionSummary {
    DecisionKind kind{DecisionKind::Refuse};
    ErrorCode status{ErrorCode::Ok};
    u8 binding{static_cast<u8>(BindingConstraint::None)};
    u8 pressure{static_cast<u8>(PressureState::Unknown)};
    u32 utilization_bp{0};
    u64 sequence{0};
    u64 requested_units{0};
    u64 granted_units{0};
    PoolId pool{};
    QueueId queue{};
    AllocationId allocation{};
    PoolAccounting accounting{};
};

void encode_hello_request(ByteWriter* writer, const HelloRequest& message);
bool decode_hello_request(ByteReader* reader, HelloRequest* message);
void encode_hello_response(ByteWriter* writer, const HelloResponse& message);
bool decode_hello_response(ByteReader* reader, HelloResponse* message);

void encode_allocate(ByteWriter* writer, const AllocateRequest& request);
bool decode_allocate(ByteReader* reader, AllocateRequest* request);
void encode_release(ByteWriter* writer, const ReleaseRequest& request);
bool decode_release(ByteReader* reader, ReleaseRequest* request);
void encode_evaluate(ByteWriter* writer, const EvaluateRequest& request);
bool decode_evaluate(ByteReader* reader, EvaluateRequest* request);
void encode_pressure(ByteWriter* writer, const PressureSnapshot& snapshot);
bool decode_pressure(ByteReader* reader, PressureSnapshot* snapshot);

void encode_accounting(ByteWriter* writer, const PoolAccounting& accounting);
bool decode_accounting(ByteReader* reader, PoolAccounting* accounting);
void encode_decision_summary(ByteWriter* writer, const DecisionSummary& summary);
bool decode_decision_summary(ByteReader* reader, DecisionSummary* summary);

/// Response body: u16 status | u16 message length | message | payload.
void encode_response(ByteWriter* writer, ErrorCode status, std::string_view message,
                     const ByteWriter* payload);
bool decode_response(ByteReader* reader, ErrorCode* status, std::string* message,
                     std::vector<u8>* payload);

}  // namespace buffer_fabric::protocol
