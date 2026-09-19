#include "buffer_fabric/authority.hpp"

#include <cstdio>

#include "buffer_fabric/hash.hpp"
#include "wire.hpp"

namespace buffer_fabric {

std::string_view to_string(AuthorityKind kind) noexcept {
    switch (kind) {
        case AuthorityKind::Capacity: return "capacity";
        case AuthorityKind::Pool: return "pool";
        case AuthorityKind::Queue: return "queue";
        case AuthorityKind::Policy: return "policy";
        case AuthorityKind::Pressure: return "pressure";
        case AuthorityKind::Epoch: return "epoch";
        case AuthorityKind::Backend: return "backend";
        case AuthorityKind::Provenance: return "provenance";
        case AuthorityKind::Attempt: return "attempt";
        case AuthorityKind::BootIncarnation: return "boot";
        case AuthorityKind::Reservation: return "reservation";
    }
    return "unknown";
}

std::string_view to_string(BindingConstraint constraint) noexcept {
    switch (constraint) {
        case BindingConstraint::None: return "none";
        case BindingConstraint::Capacity: return "capacity";
        case BindingConstraint::ProtectedHeadroom: return "protected_headroom";
        case BindingConstraint::OvercommitLimit: return "overcommit_limit";
        case BindingConstraint::BorrowLimit: return "borrow_limit";
        case BindingConstraint::PolicyMaxSingleAllocation: return "policy_max_single_allocation";
        case BindingConstraint::PolicyMaxSingleRequest: return "policy_max_single_request";
        case BindingConstraint::PolicySoftLimit: return "policy_soft_limit";
        case BindingConstraint::QueueMaxCommitted: return "queue_max_committed";
        case BindingConstraint::QueueShare: return "queue_share";
        case BindingConstraint::PressureRefuse: return "pressure_refuse";
        case BindingConstraint::PressureReduce: return "pressure_reduce";
        case BindingConstraint::EvidenceUnknown: return "evidence_unknown";
        case BindingConstraint::EvidenceStale: return "evidence_stale";
        case BindingConstraint::GenerationMismatch: return "generation_mismatch";
        case BindingConstraint::PoolRetired: return "pool_retired";
        case BindingConstraint::QueueRetired: return "queue_retired";
        case BindingConstraint::ReservationLimit: return "reservation_limit";
        case BindingConstraint::ReclaimReserve: return "reclaim_reserve";
        case BindingConstraint::RequestZero: return "request_zero";
        case BindingConstraint::BackendCapacity: return "backend_capacity";
        case BindingConstraint::Shutdown: return "shutdown";
    }
    return "unknown";
}

bool AuthorityVector::bind(AuthorityKind kind, u64 value, Generation generation) {
    if (full()) return false;
    entries_[count_] = AuthorityEntry{kind, value, generation};
    ++count_;
    return true;
}

bool AuthorityVector::has(AuthorityKind kind) const noexcept {
    for (u8 i = 0; i < count_; ++i) {
        if (entries_[i].kind == kind) return true;
    }
    return false;
}

void AuthorityVector::seal() {
    ByteWriter writer(static_cast<usize>(count_) * 24 + 8);
    writer.put_u8(count_);
    for (u8 i = 0; i < count_; ++i) {
        writer.put_u8(static_cast<u8>(entries_[i].kind));
        writer.put_u64(entries_[i].value);
        writer.put_u64(entries_[i].generation.raw());
    }
    digest_ = digest_bytes(writer.data().data(), writer.size());
}

BoundGenerations AuthorityVector::bound() const {
    BoundGenerations out;
    for (u8 i = 0; i < count_; ++i) {
        const AuthorityEntry& entry = entries_[i];
        switch (entry.kind) {
            case AuthorityKind::Capacity:
                out.capacity_generation = entry.generation;
                break;
            case AuthorityKind::Pool:
                out.pool_generation = entry.generation;
                break;
            case AuthorityKind::Queue:
                out.queue_generation = entry.generation;
                break;
            case AuthorityKind::Policy:
                out.policy_generation = entry.generation;
                break;
            case AuthorityKind::Backend:
                out.backend_generation = entry.generation;
                break;
            case AuthorityKind::Epoch:
                out.epoch = EpochId::from_raw(entry.value);
                break;
            case AuthorityKind::BootIncarnation:
                out.boot.hi = entry.value;
                out.boot.lo = entry.generation.raw();
                break;
            default:
                break;
        }
    }
    return out;
}

std::string AuthorityVector::describe() const {
    std::string out;
    for (u8 i = 0; i < count_; ++i) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%s=%llu@%llu", std::string(to_string(entries_[i].kind)).c_str(),
                      static_cast<unsigned long long>(entries_[i].value),
                      static_cast<unsigned long long>(entries_[i].generation.raw()));
        if (!out.empty()) out.push_back(';');
        out.append(buffer);
    }
    return out;
}

}  // namespace buffer_fabric
