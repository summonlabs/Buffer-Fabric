#include "buffer_fabric/observation.hpp"

#include <utility>

namespace buffer_fabric {

std::string_view to_string(EventKind kind) noexcept {
    switch (kind) {
        case EventKind::PoolRegistered: return "pool_registered";
        case EventKind::PoolRemoved: return "pool_removed";
        case EventKind::PoolResized: return "pool_resized";
        case EventKind::PolicyPublished: return "policy_published";
        case EventKind::QueueRegistered: return "queue_registered";
        case EventKind::QueueRetired: return "queue_retired";
        case EventKind::AllocationReserved: return "allocation_reserved";
        case EventKind::AllocationCommitted: return "allocation_committed";
        case EventKind::AllocationReleased: return "allocation_released";
        case EventKind::AllocationReclaimed: return "allocation_reclaimed";
        case EventKind::AllocationFenced: return "allocation_fenced";
        case EventKind::AllocationRevalidated: return "allocation_revalidated";
        case EventKind::AllocationExpired: return "allocation_expired";
        case EventKind::PressureUpdated: return "pressure_updated";
        case EventKind::PressureRejected: return "pressure_rejected";
        case EventKind::DecisionRefused: return "decision_refused";
        case EventKind::EpochAdvanced: return "epoch_advanced";
        case EventKind::Recovered: return "recovered";
        case EventKind::AccountingViolation: return "accounting_violation";
        case EventKind::ShutdownStarted: return "shutdown_started";
        case EventKind::ShutdownComplete: return "shutdown_complete";
        case EventKind::AttemptConflict: return "attempt_conflict";
        case EventKind::BackendEffectSkipped: return "backend_effect_skipped";
    }
    return "unknown";
}

IEventSink::~IEventSink() = default;

RingEventSink::RingEventSink(usize capacity) : capacity_(capacity == 0 ? 1 : capacity) {
    events_.reserve(capacity_);
}

void RingEventSink::on_event(const FabricEvent& event) {
    emitted_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(mutex_);
    if (events_.size() < capacity_) {
        events_.push_back(event);
        return;
    }
    events_[next_] = event;
    next_ = (next_ + 1) % capacity_;
    dropped_.fetch_add(1, std::memory_order_relaxed);
}

u64 RingEventSink::retained() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return static_cast<u64>(events_.size());
}

std::vector<FabricEvent> RingEventSink::snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<FabricEvent> out;
    out.reserve(events_.size());
    if (events_.size() < capacity_ || next_ == 0) {
        out = events_;
        return out;
    }
    for (usize i = 0; i < events_.size(); ++i) {
        out.push_back(events_[(next_ + i) % events_.size()]);
    }
    return out;
}

void RingEventSink::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    events_.clear();
    next_ = 0;
}

}  // namespace buffer_fabric
