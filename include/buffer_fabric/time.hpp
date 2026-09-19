#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Monotonic logical tick. All freshness and age decisions are expressed in
/// ticks so that they are deterministic under a test clock.
using Tick = u64;

/// No evidence is ever considered fresh forever.
inline constexpr Tick kNeverFresh = 0;

/// Time source.
///
/// Contract: implementations must be thread-safe, must not block, and must not
/// call back into the BufferFabric they are attached to. A clock is a leaf
/// callback: the runtime samples it before taking its own state lock, so a
/// clock can never observe or re-enter a half-applied mutation, but it also
/// must not depend on the fabric for its own answer.
class BF_API IClock {
public:
    IClock() = default;
    virtual ~IClock();
    IClock(const IClock&) = delete;
    IClock& operator=(const IClock&) = delete;

    /// Monotonic, non-decreasing tick. Never goes backwards.
    [[nodiscard]] virtual Tick now_ticks() const noexcept = 0;
    /// Wall clock seconds since the Unix epoch; 0 when unavailable. Used for
    /// provenance only, never for freshness.
    [[nodiscard]] virtual u64 wall_seconds() const noexcept = 0;
};

/// Steady monotonic clock derived from std::chrono::steady_clock.
class BF_API SteadyClock final : public IClock {
public:
    SteadyClock() noexcept;
    [[nodiscard]] Tick now_ticks() const noexcept override;
    [[nodiscard]] u64 wall_seconds() const noexcept override;

private:
    Tick origin_{};
};

/// Deterministic clock for tests and replay. Ticks advance only when told to.
class BF_API ManualClock final : public IClock {
public:
    explicit ManualClock(Tick start = 1000) noexcept : now_(start) {}
    [[nodiscard]] Tick now_ticks() const noexcept override { return now_.load(std::memory_order_relaxed); }
    [[nodiscard]] u64 wall_seconds() const noexcept override { return wall_; }

    void advance(Tick delta) noexcept {
        now_.fetch_add(delta, std::memory_order_relaxed);
    }
    void set(Tick value) noexcept { now_.store(value, std::memory_order_relaxed); }
    void set_wall(u64 seconds) noexcept { wall_ = seconds; }

private:
    std::atomic<Tick> now_;
    u64 wall_{1700000000ull};
};

/// True when evidence observed at \p observed_at with lifetime \p ttl_ticks is
/// still fresh at \p now. A ttl of 0 means "no lifetime declared", which is
/// never fresh.
[[nodiscard]] inline bool is_fresh(Tick now, Tick observed_at, Tick ttl_ticks) noexcept {
    if (ttl_ticks == 0) return false;
    if (observed_at > now) return false;  // future-dated evidence is not evidence
    return (now - observed_at) <= ttl_ticks;
}

}  // namespace buffer_fabric
