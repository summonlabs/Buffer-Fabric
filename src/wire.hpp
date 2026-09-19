#pragma once

// Canonical encoding of domain objects. One definition of the byte layout is
// shared by the durable image and the explanation digest, so that a digest and
// a persisted record can never disagree about what they describe.
//
// Every decode returns false on malformed input; callers turn that into a
// Status with the appropriate error code. No decode allocates based on an
// unvalidated length: every length is bounded before use.

#include <string>

#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/codec.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/pressure.hpp"
#include "buffer_fabric/topology.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric::detail {

inline constexpr usize kWireMaxNameBytes = kMaxNameBytes;
inline constexpr usize kWireMaxSourceBytes = 128;

void encode_string(ByteWriter& writer, const std::string& text, usize max_bytes);
bool decode_string(ByteReader& reader, std::string* out, usize max_bytes);

void encode_thresholds(ByteWriter& writer, const Thresholds& thresholds);
bool decode_thresholds(ByteReader& reader, Thresholds* out);

void encode_overcommit(ByteWriter& writer, const OvercommitPolicy& policy);
bool decode_overcommit(ByteReader& reader, OvercommitPolicy* out);

void encode_borrow(ByteWriter& writer, const BorrowPolicy& policy);
bool decode_borrow(ByteReader& reader, BorrowPolicy* out);

void encode_policy(ByteWriter& writer, const PoolPolicy& policy);
bool decode_policy(ByteReader& reader, PoolPolicy* out);

void encode_pool(ByteWriter& writer, const PoolDescriptor& pool);
bool decode_pool(ByteReader& reader, PoolDescriptor* out);

void encode_queue(ByteWriter& writer, const QueueDescriptor& queue);
bool decode_queue(ByteReader& reader, QueueDescriptor* out);

void encode_lineage(ByteWriter& writer, const Lineage& lineage);
bool decode_lineage(ByteReader& reader, Lineage* out);

void encode_bound(ByteWriter& writer, const BoundGenerations& bound);
bool decode_bound(ByteReader& reader, BoundGenerations* out);

void encode_allocation(ByteWriter& writer, const AllocationRecord& allocation);
bool decode_allocation(ByteReader& reader, AllocationRecord* out);

void encode_pressure_snapshot(ByteWriter& writer, const PressureSnapshot& snapshot);
bool decode_pressure_snapshot(ByteReader& reader, PressureSnapshot* out);

void encode_attempt(ByteWriter& writer, const AttemptRecord& attempt);
bool decode_attempt(ByteReader& reader, AttemptRecord* out);

}  // namespace buffer_fabric::detail
