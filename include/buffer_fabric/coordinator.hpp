#pragma once

#include <memory>
#include <string>

#include "buffer_fabric/fabric.hpp"
#include "buffer_fabric/observation.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/transport.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

struct BF_API CoordinatorConfig {
    std::string bind_address{"127.0.0.1"};
    /// Zero requests an ephemeral port; the bound port is reported by port().
    u16 port{0};
    /// Durable state directory. Empty disables durability for the coordinator.
    std::string state_directory{};
    DurabilityMode durability{DurabilityMode::JournalSync};
    /// Maximum number of concurrently served client connections.
    u64 max_clients{32};
    u64 max_frame_bytes{kMaxFrameBytes};
    /// Per-request decision history retained by the fabric.
    u64 decision_history_capacity{4096};

    // --- bootstrap topology -------------------------------------------------
    // A coordinator that owns its fabric can publish a minimal, explicit
    // topology so that workers have something authoritative to allocate
    // against. Every value here is an operator input, not a default that is
    // guessed at runtime.
    bool bootstrap_topology{true};
    u64 bootstrap_backend_id{1};
    u64 bootstrap_resource_id{1};
    u64 bootstrap_pool_id{1};
    u64 bootstrap_queue_id{1};
    u64 bootstrap_policy_id{1};
    u64 bootstrap_resource_units{1u << 20};
    u64 bootstrap_pool_units{1u << 19};
    u64 bootstrap_protected_units{1u << 12};
    u64 bootstrap_queue_demand_units{1u << 17};
    u64 evidence_ttl_ticks{3'600'000'000'000ull};
};

/// A real multi-process coordinator.
///
/// The coordinator owns the authoritative BufferFabric, serves framed requests
/// over a real TCP socket on an explicitly chosen address, and fences every
/// request by the fabric epoch. A request that presents an epoch other than the
/// current one is refused with StaleEpoch and the connection is closed; it can
/// never reach authoritative state.
///
/// Every client session runs on its own thread. The coordinator never holds the
/// fabric lock while accepting, reading, writing or joining: the fabric's own
/// lock is taken and released inside each fabric call.
class BF_API Coordinator {
public:
    Coordinator();
    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    [[nodiscard]] Status start(const CoordinatorConfig& config);
    [[nodiscard]] Result<u16> port() const;
    [[nodiscard]] EpochId epoch() const;

    /// Ask the accept loop and every session to stop. Safe to call from any
    /// thread, including a session thread.
    void request_shutdown() noexcept;

    /// True once a shutdown has been requested, whether by request_shutdown(),
    /// by a Shutdown message, or by a failed listener. Polling this is the
    /// supported way for an embedding process to wait for termination without
    /// holding any lock.
    [[nodiscard]] bool stopped() const noexcept;

    /// Join every worker thread and close the fabric. Never called from a
    /// session thread.
    [[nodiscard]] Status stop();

    [[nodiscard]] FabricMetrics metrics() const;
    [[nodiscard]] u64 served_requests() const noexcept;
    [[nodiscard]] u64 rejected_requests() const noexcept;
    [[nodiscard]] BufferFabric& fabric() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace buffer_fabric
