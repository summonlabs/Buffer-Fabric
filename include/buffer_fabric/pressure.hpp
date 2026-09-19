#pragma once

#include <string>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/policy.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Opaque provenance handle describing where an authoritative input came from.
struct BF_API Provenance {
    ProvenanceId id{};
    Generation generation{};
    std::string source{};
    u64 source_sequence{0};
};

// ---------------------------------------------------------------------------
// Pressure evidence
//
// A snapshot is an externally supplied observation. It is never authoritative
// on its own: it is authoritative only when its epoch, pool generation and age
// all match the fabric state at decision time. Anything else yields UNKNOWN or
// STALE and is never promoted to positive authority.
// ---------------------------------------------------------------------------
struct BF_API PressureSnapshot {
    SnapshotId id{};
    PoolId pool{};
    Generation pool_generation{};
    EpochId epoch{};
    BootIncarnation boot{};
    Tick observed_at{0};
    Tick ttl_ticks{0};
    u64 demand_units{0};
    u64 committed_units{0};
    u64 reclaimable_units{0};
    ProvenanceId provenance{};
    /// When true (the default) the fabric requires \ref committed_units to
    /// equal its own authoritative committed usage at decision time. Evidence
    /// that describes a different state is not evidence for this decision and
    /// is rejected as stale. A producer that cannot know the instantaneous
    /// value sets this to false, which is recorded explicitly.
    bool assert_committed{true};
    /// Digest supplied by the producer over its own canonical encoding. Zero
    /// means the producer supplied no digest, which is recorded but never
    /// treated as a substitute for the fabric's own integrity checks.
    Digest evidence_digest{};
};

/// The resolved evidence actually used by an evaluation, with the exact reason
/// a state was chosen.
struct BF_API PressureEvidence {
    PressureState state{PressureState::Unknown};
    u32 utilization_bp{0};
    u32 binding_bp{0};
    SnapshotId snapshot{};
    Generation pool_generation{};
    EpochId epoch{};
    Tick observed_at{0};
    Tick age_ticks{0};
    bool overcommitted{false};
    bool epoch_match{false};
    bool generation_match{false};
    bool fresh{false};
    /// Why the state is what it is. Ok only when fresh evidence was used.
    ErrorCode reason{ErrorCode::EvidenceUnknown};
    std::string detail{};

    [[nodiscard]] bool usable() const noexcept { return pressure_is_known(state); }
};

}  // namespace buffer_fabric
