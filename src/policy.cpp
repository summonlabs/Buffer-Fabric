#include "buffer_fabric/policy.hpp"

#include <cstdio>

#include "buffer_fabric/hash.hpp"
#include "wire.hpp"

namespace buffer_fabric {

std::string_view to_string(PressureState state) noexcept {
    switch (state) {
        case PressureState::Unknown: return "UNKNOWN";
        case PressureState::Clear: return "CLEAR";
        case PressureState::Elevated: return "ELEVATED";
        case PressureState::High: return "HIGH";
        case PressureState::Critical: return "CRITICAL";
        case PressureState::Stale: return "STALE";
    }
    return "UNKNOWN";
}

bool pressure_is_known(PressureState state) noexcept {
    return state == PressureState::Clear || state == PressureState::Elevated ||
           state == PressureState::High || state == PressureState::Critical;
}

bool pressure_at_least(PressureState state, PressureState floor) noexcept {
    if (!pressure_is_known(state)) return false;
    return static_cast<u8>(state) >= static_cast<u8>(floor);
}

Status Thresholds::validate() const {
    if (elevated_bp == 0) {
        return Status(ErrorCode::InvalidArgument,
                      "elevated threshold must be at least 1 basis point");
    }
    if (elevated_bp >= high_bp) {
        return Status(ErrorCode::InvalidArgument, "elevated threshold must be below high threshold");
    }
    if (high_bp >= critical_bp) {
        return Status(ErrorCode::InvalidArgument,
                      "high threshold must be below critical threshold");
    }
    if (critical_bp > kScale) {
        return Status(ErrorCode::InvalidArgument,
                      "critical threshold must not exceed 10000 basis points");
    }
    return Status::success();
}

bool utilization_at_least(u64 used, u64 usable, u32 band_bp) noexcept {
    if (usable == 0) return used != 0;
    return mul_compare(used, Thresholds::kScale, static_cast<u64>(band_bp), usable) >= 0;
}

PressureObservation classify_pressure(const Thresholds& thresholds, u64 used, u64 usable,
                                      bool overcommitted) noexcept {
    PressureObservation observation;
    observation.overcommitted = overcommitted;

    if (usable == 0) {
        if (used == 0 && !overcommitted) {
            observation.state = PressureState::Clear;
            observation.utilization_bp = 0;
            observation.binding_bp = 0;
            return observation;
        }
        observation.state = PressureState::Critical;
        observation.utilization_bp = Thresholds::kScale;
        observation.binding_bp = thresholds.critical_bp;
        return observation;
    }

    u64 scaled = 0;
    if (mul_overflow(used, Thresholds::kScale, &scaled)) {
        observation.utilization_bp = Thresholds::kScale;
    } else {
        const u64 basis_points = scaled / usable;
        observation.utilization_bp =
            basis_points >= Thresholds::kScale ? Thresholds::kScale
                                               : static_cast<u32>(basis_points);
    }

    if (utilization_at_least(used, usable, thresholds.critical_bp)) {
        observation.state = PressureState::Critical;
        observation.binding_bp = thresholds.critical_bp;
    } else if (utilization_at_least(used, usable, thresholds.high_bp)) {
        observation.state = PressureState::High;
        observation.binding_bp = thresholds.high_bp;
    } else if (utilization_at_least(used, usable, thresholds.elevated_bp)) {
        observation.state = PressureState::Elevated;
        observation.binding_bp = thresholds.elevated_bp;
    } else {
        observation.state = PressureState::Clear;
        observation.binding_bp = 0;
    }
    return observation;
}

Status PoolPolicy::validate() const {
    if (id.raw() == 0) {
        return Status(ErrorCode::InvalidArgument, "policy id must be non-zero");
    }
    if (!name.empty() && !is_valid_name(name)) {
        return Status(ErrorCode::NameTooLong,
                      "policy name must be 1..64 characters of [A-Za-z0-9_.-]");
    }
    BF_TRY(pressure.thresholds.validate());

    if (overcommit.mode == OvercommitMode::Bounded) {
        if (overcommit.limit_units == 0) {
            return Status(ErrorCode::InvalidArgument,
                          "bounded overcommit requires a non-zero limit");
        }
        if (overcommit.limit_units > kMaxOvercommitUnits) {
            return Status(ErrorCode::Overflow, "overcommit limit exceeds the supported bound");
        }
    } else if (overcommit.limit_units != 0) {
        return Status(ErrorCode::InvalidArgument,
                      "overcommit limit must be zero when overcommit is forbidden");
    }

    if (borrow.mode == BorrowMode::Forbid && borrow.limit_units != 0) {
        return Status(ErrorCode::InvalidArgument,
                      "borrow limit must be zero when borrowing is forbidden");
    }
    if (borrow.mode == BorrowMode::FromAncestorFree) {
        if (borrow.limit_units == 0) {
            return Status(ErrorCode::InvalidArgument,
                          "borrowing from ancestors requires a non-zero limit");
        }
        if (borrow.limit_units > kMaxUnitsPerPool) {
            return Status(ErrorCode::Overflow, "borrow limit exceeds the supported bound");
        }
    }

    if (max_single_allocation_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "max_single_allocation_units exceeds the supported bound");
    }
    if (max_single_request_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "max_single_request_units exceeds the supported bound");
    }
    if (soft_limit_bp > Thresholds::kScale) {
        return Status(ErrorCode::InvalidArgument, "soft_limit_bp must not exceed 10000");
    }
    if (usable_floor_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "usable_floor_units exceeds the supported bound");
    }
    if (max_pending_reservations > (1ull << 20)) {
        return Status(ErrorCode::BoundedResourceExhausted,
                      "max_pending_reservations exceeds the supported bound");
    }
    if (reclaim.max_units_per_operation > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "reclaim max_units_per_operation exceeds the bound");
    }
    if (reclaim.reserve_units > kMaxUnitsPerPool) {
        return Status(ErrorCode::Overflow, "reclaim reserve_units exceeds the bound");
    }
    if (pressure.refuse_increase_on_unknown && refuse_grants_at == PressureState::Clear) {
        return Status(ErrorCode::InvalidArgument,
                      "refusing grants at CLEAR would refuse every grant");
    }
    if (refuse_grants_at != PressureState::Unknown && !pressure_is_known(refuse_grants_at) &&
        refuse_grants_at != PressureState::Stale) {
        return Status(ErrorCode::InvalidArgument, "refuse_grants_at is not a valid pressure state");
    }
    if (!pressure_is_known(reduce_at)) {
        return Status(ErrorCode::InvalidArgument, "reduce_at must be a known pressure state");
    }
    return Status::success();
}

Digest PoolPolicy::digest() const {
    ByteWriter writer(256);
    detail::encode_policy(writer, *this);
    return digest_bytes(writer.data().data(), writer.size());
}

}  // namespace buffer_fabric
