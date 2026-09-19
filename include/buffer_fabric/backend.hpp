#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/time.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

enum class BackendKind : u8 {
    /// No backend attached. Capacity authority is unavailable by construction:
    /// a Null backend never reports capacity, so pools cannot be founded on it.
    Null = 0,
    /// Deterministic in-process capacity source for tests and benchmarks.
    /// SYNTHETIC: it does not correspond to any physical device.
    Synthetic = 1,
    /// A real device or operating-system facility. No such backend ships with
    /// this release; the interface exists so that one can be attached
    /// explicitly instead of the runtime guessing at hardware.
    Device = 2,
};

[[nodiscard]] BF_API std::string_view to_string(BackendKind kind) noexcept;

/// The backend's statement about a resource's raw buffer capacity. An
/// authority is only usable when authoritative is true and the generation is
/// non-zero; otherwise capacity is UNKNOWN and no pool may be founded or
/// resized on it.
struct BF_API CapacityAuthority {
    ResourceId resource{};
    BackendId backend{};
    Generation generation{};
    u64 raw_units{0};
    Tick observed_at{0};
    bool authoritative{false};

    [[nodiscard]] bool usable() const noexcept {
        return authoritative && resource.valid() && generation.valid();
    }
};

// ---------------------------------------------------------------------------
// Backend interface
//
// Buffer Fabric owns governance of capacity, not the device. Physical device
// memory programming is reachable only through an explicit backend attached by
// the embedder. When no backend is attached, capacity is UNKNOWN and every
// authority decision that depends on it is refused or reported as UNKNOWN.
// Implementations must be safe for concurrent calls from multiple threads.
// ---------------------------------------------------------------------------
class BF_API IBackend {
public:
    IBackend() = default;
    virtual ~IBackend();
    IBackend(const IBackend&) = delete;
    IBackend& operator=(const IBackend&) = delete;

    [[nodiscard]] virtual BackendId id() const noexcept = 0;
    [[nodiscard]] virtual Generation generation() const noexcept = 0;
    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// False when the backend cannot currently vouch for capacity. A backend
    /// that is not ready yields UNKNOWN authority, never zero authority.
    [[nodiscard]] virtual bool ready() const noexcept = 0;

    [[nodiscard]] virtual Result<CapacityAuthority> query_capacity(ResourceId resource) const = 0;

    /// Optional physical programming hooks. Implementations that own no
    /// hardware return Unsupported; the fabric then records that no hardware
    /// effect was applied rather than assuming success.
    [[nodiscard]] virtual VoidResult program_reservation(ResourceId resource, u64 units,
                                                         Generation expected_backend_generation);
    [[nodiscard]] virtual VoidResult program_release(ResourceId resource, u64 units);
};

/// Backend with no attached authority. Every query reports Unsupported.
class BF_API NullBackend final : public IBackend {
public:
    NullBackend() = default;

    [[nodiscard]] BackendId id() const noexcept override { return BackendId::from_raw(0); }
    [[nodiscard]] Generation generation() const noexcept override { return Generation{}; }
    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Null; }
    [[nodiscard]] std::string_view name() const noexcept override { return "null"; }
    [[nodiscard]] bool ready() const noexcept override { return false; }
    [[nodiscard]] Result<CapacityAuthority> query_capacity(ResourceId resource) const override;
};

/// Deterministic synthetic backend used by tests and the benchmark.
///
/// SYNTHETIC: capacities are declared by the embedder. Nothing here observes or
/// programs physical hardware.
class BF_API SyntheticBackend final : public IBackend {
public:
    struct ResourceEntry {
        u64 raw_units{0};
        Generation generation{};
        bool ready{true};
    };

    SyntheticBackend(BackendId id, Generation generation, std::string name);

    void set_resource(ResourceId resource, u64 raw_units, Generation generation);
    void remove_resource(ResourceId resource);
    void set_ready(bool ready) noexcept { ready_ = ready; }
    void set_generation(Generation generation) noexcept { generation_ = generation; }

    /// Recorded physical effect attempts, for tests that assert the backend was
    /// consulted with the exact expected generation.
    struct EffectLog {
        u64 reservation_calls{0};
        u64 release_calls{0};
        u64 reservation_units{0};
        u64 release_units{0};
        u64 stale_generation_rejections{0};
        u64 unsupported_calls{0};
    };
    [[nodiscard]] EffectLog effects() const noexcept;
    void clear_effects() noexcept;

    [[nodiscard]] BackendId id() const noexcept override { return id_; }
    [[nodiscard]] Generation generation() const noexcept override { return generation_; }
    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Synthetic; }
    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] bool ready() const noexcept override { return ready_; }
    [[nodiscard]] Result<CapacityAuthority> query_capacity(ResourceId resource) const override;

    [[nodiscard]] VoidResult program_reservation(ResourceId resource, u64 units,
                                                 Generation expected_backend_generation) override;
    [[nodiscard]] VoidResult program_release(ResourceId resource, u64 units) override;

private:
    BackendId id_{};
    Generation generation_{};
    std::string name_{};
    bool ready_{true};
    std::unordered_map<u64, ResourceEntry> resources_{};
    // Effect counters are individually atomic so that concurrent callers never
    // race and no lock is taken on the query path.
    std::atomic<u64> reservation_calls_{0};
    std::atomic<u64> release_calls_{0};
    std::atomic<u64> reservation_units_{0};
    std::atomic<u64> release_units_{0};
    std::atomic<u64> stale_generation_rejections_{0};
    std::atomic<u64> unsupported_calls_{0};
};

}  // namespace buffer_fabric
