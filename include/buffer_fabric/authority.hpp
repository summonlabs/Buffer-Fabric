#pragma once

#include <array>
#include <string>

#include "buffer_fabric/allocation.hpp"
#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/pressure.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// One element of the authority vector: the exact identity of an input that a
/// decision depended on.
enum class AuthorityKind : u8 {
    Capacity = 0,
    Pool = 1,
    Queue = 2,
    Policy = 3,
    Pressure = 4,
    Epoch = 5,
    Backend = 6,
    Provenance = 7,
    Attempt = 8,
    BootIncarnation = 9,
    Reservation = 10,
};

[[nodiscard]] BF_API std::string_view to_string(AuthorityKind kind) noexcept;

struct BF_API AuthorityEntry {
    AuthorityKind kind{AuthorityKind::Capacity};
    u64 value{0};
    Generation generation{};
};

class BF_API AuthorityVector {
public:
    AuthorityVector() = default;

    bool bind(AuthorityKind kind, u64 value, Generation generation = Generation{});
    bool bind_generation(AuthorityKind kind, Generation generation) {
        return bind(kind, generation.raw(), generation);
    }
    bool bind_id(AuthorityKind kind, u64 id) { return bind(kind, id, Generation{}); }

    [[nodiscard]] u8 count() const noexcept { return count_; }
    [[nodiscard]] const AuthorityEntry& at(u8 index) const { return entries_[index]; }
    [[nodiscard]] bool full() const noexcept { return count_ >= kMaxAuthorityVectorEntries; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool has(AuthorityKind kind) const noexcept;

    void seal();
    [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
    [[nodiscard]] BoundGenerations bound() const;
    [[nodiscard]] std::string describe() const;

private:
    std::array<AuthorityEntry, kMaxAuthorityVectorEntries> entries_{};
    u8 count_{0};
    Digest digest_{};
};

enum class BindingConstraint : u8 {
    None = 0,
    Capacity = 1,
    ProtectedHeadroom = 2,
    OvercommitLimit = 3,
    BorrowLimit = 4,
    PolicyMaxSingleAllocation = 5,
    PolicyMaxSingleRequest = 6,
    PolicySoftLimit = 7,
    QueueMaxCommitted = 8,
    QueueShare = 9,
    PressureRefuse = 10,
    PressureReduce = 11,
    EvidenceUnknown = 12,
    EvidenceStale = 13,
    GenerationMismatch = 14,
    PoolRetired = 15,
    QueueRetired = 16,
    ReservationLimit = 17,
    ReclaimReserve = 18,
    RequestZero = 19,
    BackendCapacity = 20,
    Shutdown = 21,
};

[[nodiscard]] BF_API std::string_view to_string(BindingConstraint constraint) noexcept;

}  // namespace buffer_fabric
