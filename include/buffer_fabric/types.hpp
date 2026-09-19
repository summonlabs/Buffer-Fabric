#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "buffer_fabric/config.hpp"

namespace buffer_fabric {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

// ---------------------------------------------------------------------------
// Hard bounds
//
// Every externally influenced size, capacity, count and time unit is bounded.
// These constants are the single source of truth for those bounds and are
// enforced by the codec, the topology registry, the journal reader and the
// transport framing.
// ---------------------------------------------------------------------------
inline constexpr u64 kMaxPoolCount = 4096;
inline constexpr u64 kMaxQueueCount = 1u << 20;
inline constexpr u64 kMaxAllocationCount = 1u << 22;
inline constexpr u64 kMaxPolicyCount = 4096;
inline constexpr u64 kMaxSnapshotCount = 1u << 16;
inline constexpr u64 kMaxPoolDepth = 16;
inline constexpr u64 kMaxUnitsPerPool = (u64{1} << 60);
inline constexpr u64 kMaxOvercommitUnits = (u64{1} << 60);
inline constexpr u64 kMaxAttemptMemory = 1u << 16;
inline constexpr u64 kMaxAuthorityVectorEntries = 16;
inline constexpr u64 kMaxNameBytes = 64;
inline constexpr u64 kMaxStatusMessageBytes = 512;
inline constexpr u64 kMaxJournalRecordBytes = 4u << 20;
inline constexpr u64 kMaxSnapshotBytes = 256u << 20;
inline constexpr u64 kMaxFrameBytes = 1u << 20;
inline constexpr u64 kMaxReclaimVictims = 1u << 20;
inline constexpr u64 kMaxJournalRecordsPerOpen = 1u << 24;

// ---------------------------------------------------------------------------
// Strong identity
//
// Every entity in the model is addressed by a distinct type. Raw integers are
// never accepted where an identity is expected, and identities of different
// kinds are not interchangeable. The default-constructed value is the invalid
// identity (raw 0); all validation paths reject it.
// ---------------------------------------------------------------------------
template <class Tag, class Rep = u64>
class StrongId {
public:
    using rep_type = Rep;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep raw) noexcept : raw_(raw) {}

    [[nodiscard]] static constexpr StrongId from_raw(Rep raw) noexcept { return StrongId(raw); }

    [[nodiscard]] constexpr Rep raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return raw_ != Rep{0}; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.raw_ == b.raw_; }
    friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
        return a.raw_ <=> b.raw_;
    }

private:
    Rep raw_{};
};

struct PoolIdTag {};
struct QueueIdTag {};
struct TenantIdTag {};
struct ClassIdTag {};
struct ResourceIdTag {};
struct PolicyIdTag {};
struct AllocationIdTag {};
struct BackendIdTag {};
struct ProvenanceIdTag {};
struct AttemptIdTag {};
struct SnapshotIdTag {};
struct RequestIdTag {};
struct GenerationTag {};
struct EpochTag {};
struct SequenceTag {};
struct WorkerIdTag {};

using PoolId = StrongId<PoolIdTag>;
using QueueId = StrongId<QueueIdTag>;
using TenantId = StrongId<TenantIdTag>;
using ClassId = StrongId<ClassIdTag>;
using ResourceId = StrongId<ResourceIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using AllocationId = StrongId<AllocationIdTag>;
using BackendId = StrongId<BackendIdTag>;
using ProvenanceId = StrongId<ProvenanceIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using SnapshotId = StrongId<SnapshotIdTag>;
using RequestId = StrongId<RequestIdTag>;
using WorkerId = StrongId<WorkerIdTag>;

/// Monotonic revision counter for a durable entity (pool, queue, policy,
/// capacity, backend). Generation 0 means "never published".
using Generation = StrongId<GenerationTag>;
/// Monotonic fabric-incarnation counter. Advances on every durable reopen.
using EpochId = StrongId<EpochTag>;
/// Per-claimant monotonic request sequence.
using Sequence = StrongId<SequenceTag>;

// ---------------------------------------------------------------------------
// Boot incarnation
//
// Identifies one process lifetime of one fabric instance. Durable state records
// the boot incarnation that wrote it. Authority bound to a previous boot
// incarnation is never restored as live.
// ---------------------------------------------------------------------------
struct BootIncarnation {
    u64 hi{0};
    u64 lo{0};

    [[nodiscard]] bool valid() const noexcept { return hi != 0 || lo != 0; }
    friend bool operator==(const BootIncarnation& a, const BootIncarnation& b) noexcept {
        return a.hi == b.hi && a.lo == b.lo;
    }
    friend bool operator!=(const BootIncarnation& a, const BootIncarnation& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const BootIncarnation& a, const BootIncarnation& b) noexcept {
        return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
    }
};

// ---------------------------------------------------------------------------
// Digest
//
// Stable 128-bit content digest (FNV-1a/128 over a canonical byte encoding).
// Used for explanation identity, attempt de-duplication and journal chaining.
// Not a cryptographic hash and not used for authentication.
// ---------------------------------------------------------------------------
struct Digest {
    u64 hi{0};
    u64 lo{0};

    friend bool operator==(const Digest& a, const Digest& b) noexcept {
        return a.hi == b.hi && a.lo == b.lo;
    }
    friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }

    [[nodiscard]] bool valid() const noexcept { return hi != 0 || lo != 0; }
    [[nodiscard]] std::string to_hex() const;
};

// ---------------------------------------------------------------------------
// Checked integer arithmetic
//
// All arithmetic on externally influenced capacities, counts and time units
// goes through these helpers. Overflow is reported, never wrapped.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool add_overflow(u64 a, u64 b, u64* out) noexcept {
    *out = a + b;
    return *out < a;
}
[[nodiscard]] inline bool sub_underflow(u64 a, u64 b, u64* out) noexcept {
    if (a < b) {
        *out = 0;
        return true;
    }
    *out = a - b;
    return false;
}
[[nodiscard]] inline bool mul_overflow(u64 a, u64 b, u64* out) noexcept {
    if (a != 0 && b > (std::numeric_limits<u64>::max)() / a) {
        *out = 0;
        return true;
    }
    *out = a * b;
    return false;
}

/// Compare a*b against c*d exactly, without overflow. Returns <0, 0 or >0.
[[nodiscard]] inline int mul_compare(u64 a, u64 b, u64 c, u64 d) noexcept {
    const auto split_mul = [](u64 x, u64 y, u64& hi, u64& lo) noexcept {
        const u64 x0 = x & 0xFFFFFFFFull;
        const u64 x1 = x >> 32;
        const u64 y0 = y & 0xFFFFFFFFull;
        const u64 y1 = y >> 32;
        const u64 p00 = x0 * y0;
        const u64 p01 = x0 * y1;
        const u64 p10 = x1 * y0;
        const u64 p11 = x1 * y1;
        const u64 mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
        lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    };
    u64 hi1 = 0;
    u64 lo1 = 0;
    u64 hi2 = 0;
    u64 lo2 = 0;
    split_mul(a, b, hi1, lo1);
    split_mul(c, d, hi2, lo2);
    if (hi1 != hi2) return hi1 < hi2 ? -1 : 1;
    if (lo1 != lo2) return lo1 < lo2 ? -1 : 1;
    return 0;
}

/// Ceil-divide a by b. Returns 0 when b == 0.
[[nodiscard]] inline u64 div_ceil(u64 a, u64 b) noexcept {
    if (b == 0) return 0;
    return a / b + (a % b != 0 ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// Bounded text helpers
// ---------------------------------------------------------------------------
[[nodiscard]] BF_API std::string truncate_to(std::string_view text, usize max_bytes);

/// True when the whole string is a valid identifier: ASCII letters, digits,
/// '_', '-', '.', at most kMaxNameBytes bytes, and non-empty.
[[nodiscard]] BF_API bool is_valid_name(std::string_view name) noexcept;

[[nodiscard]] BF_API u64 fnv1a64(std::string_view text) noexcept;

}  // namespace buffer_fabric
