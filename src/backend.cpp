#include "buffer_fabric/backend.hpp"

#include <utility>

namespace buffer_fabric {

std::string_view to_string(BackendKind kind) noexcept {
    switch (kind) {
        case BackendKind::Null: return "null";
        case BackendKind::Synthetic: return "synthetic";
        case BackendKind::Device: return "device";
    }
    return "unknown";
}

IBackend::~IBackend() = default;

VoidResult IBackend::program_reservation(ResourceId resource, u64 units,
                                         Generation expected_backend_generation) {
    BF_UNUSED(resource);
    BF_UNUSED(units);
    BF_UNUSED(expected_backend_generation);
    return Status(ErrorCode::Unsupported,
                  "this backend does not program physical device memory");
}

VoidResult IBackend::program_release(ResourceId resource, u64 units) {
    BF_UNUSED(resource);
    BF_UNUSED(units);
    return Status(ErrorCode::Unsupported,
                  "this backend does not program physical device memory");
}

Result<CapacityAuthority> NullBackend::query_capacity(ResourceId resource) const {
    BF_UNUSED(resource);
    return Status(ErrorCode::Unsupported,
                  "no backend is attached; capacity authority is UNKNOWN");
}

SyntheticBackend::SyntheticBackend(BackendId id, Generation generation, std::string name)
    : id_(id), generation_(generation), name_(std::move(name)) {}

void SyntheticBackend::set_resource(ResourceId resource, u64 raw_units, Generation generation) {
    ResourceEntry entry;
    entry.raw_units = raw_units;
    entry.generation = generation;
    entry.ready = true;
    resources_[resource.raw()] = entry;
}

void SyntheticBackend::remove_resource(ResourceId resource) {
    resources_.erase(resource.raw());
}

Result<CapacityAuthority> SyntheticBackend::query_capacity(ResourceId resource) const {
    if (!ready_) {
        return Status(ErrorCode::BackendNotReady, "synthetic backend is not ready");
    }
    if (!resource.valid()) {
        return Status(ErrorCode::InvalidArgument, "resource id must be non-zero");
    }
    const auto it = resources_.find(resource.raw());
    if (it == resources_.end()) {
        return Status(ErrorCode::UnknownResource, "synthetic backend has no such resource");
    }
    CapacityAuthority authority;
    authority.resource = resource;
    authority.backend = id_;
    authority.generation = it->second.generation;
    authority.raw_units = it->second.raw_units;
    authority.observed_at = 0;
    authority.authoritative = it->second.ready && it->second.generation.valid();
    return authority;
}

VoidResult SyntheticBackend::program_reservation(ResourceId resource, u64 units,
                                                 Generation expected_backend_generation) {
    reservation_calls_.fetch_add(1, std::memory_order_relaxed);
    unsupported_calls_.fetch_add(1, std::memory_order_relaxed);
    BF_UNUSED(resource);
    BF_UNUSED(units);
    BF_UNUSED(expected_backend_generation);
    // A synthetic backend has no device to program. Reporting Unsupported here
    // is the honest answer; the fabric records that no hardware effect was
    // applied instead of assuming one succeeded.
    return Status(ErrorCode::Unsupported,
                  "synthetic backend applies no physical device effect");
}

VoidResult SyntheticBackend::program_release(ResourceId resource, u64 units) {
    release_calls_.fetch_add(1, std::memory_order_relaxed);
    unsupported_calls_.fetch_add(1, std::memory_order_relaxed);
    BF_UNUSED(resource);
    BF_UNUSED(units);
    return Status(ErrorCode::Unsupported,
                  "synthetic backend applies no physical device effect");
}

SyntheticBackend::EffectLog SyntheticBackend::effects() const noexcept {
    EffectLog log;
    log.reservation_calls = reservation_calls_.load(std::memory_order_relaxed);
    log.release_calls = release_calls_.load(std::memory_order_relaxed);
    log.reservation_units = reservation_units_.load(std::memory_order_relaxed);
    log.release_units = release_units_.load(std::memory_order_relaxed);
    log.stale_generation_rejections = stale_generation_rejections_.load(std::memory_order_relaxed);
    log.unsupported_calls = unsupported_calls_.load(std::memory_order_relaxed);
    return log;
}

void SyntheticBackend::clear_effects() noexcept {
    reservation_calls_.store(0, std::memory_order_relaxed);
    release_calls_.store(0, std::memory_order_relaxed);
    reservation_units_.store(0, std::memory_order_relaxed);
    release_units_.store(0, std::memory_order_relaxed);
    stale_generation_rejections_.store(0, std::memory_order_relaxed);
    unsupported_calls_.store(0, std::memory_order_relaxed);
}

}  // namespace buffer_fabric
