#include "buffer_fabric/time.hpp"

#include <chrono>

namespace buffer_fabric {

IClock::~IClock() = default;

SteadyClock::SteadyClock() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    origin_ = static_cast<Tick>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

Tick SteadyClock::now_ticks() const noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const Tick ticks =
        static_cast<Tick>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return ticks >= origin_ ? ticks - origin_ : 0;
}

u64 SteadyClock::wall_seconds() const noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return seconds > 0 ? static_cast<u64>(seconds) : 0;
}

}  // namespace buffer_fabric
