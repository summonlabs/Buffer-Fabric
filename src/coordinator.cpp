#include "buffer_fabric/coordinator.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <string>

#include "buffer_fabric/backend.hpp"
#include "buffer_fabric/json.hpp"
#include "buffer_fabric/version.hpp"
#include "protocol.hpp"

namespace buffer_fabric {

struct Coordinator::Impl {
    CoordinatorConfig config_{};
    BufferFabric fabric_{};
    SyntheticBackend backend_{BackendId::from_raw(1), Generation::from_raw(1), "coordinator"};
    // Note: the backend id is fixed at construction; the resource it describes
    // is configured by bootstrap_topology().
    Socket listener_{};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> started_{false};
    std::atomic<u64> served_{0};
    std::atomic<u64> rejected_{0};
    std::thread accept_thread_{};
    std::mutex clients_mutex_{};
    std::vector<std::shared_ptr<Socket>> clients_{};
    std::vector<std::thread> sessions_{};
    u64 resource_id_{1};
    u64 pool_id_{1};
    u64 queue_id_{1};
    u64 policy_id_{1};
    u64 capacity_units_{0};

    void accept_loop();
    void serve(std::shared_ptr<Socket> peer);
    [[nodiscard]] Status dispatch(const Message& request, Message* reply, bool* close_after);
    [[nodiscard]] Status bootstrap_topology();
    void retire_socket(const std::shared_ptr<Socket>& peer);
};

Coordinator::Coordinator() : impl_(std::make_unique<Impl>()) {}
Coordinator::~Coordinator() {
    if (impl_) {
        impl_->shutdown_requested_.store(true);
        if (impl_->listener_.valid()) {
            impl_->listener_.shutdown();
            impl_->listener_.close();
        }
        {
            std::lock_guard<std::mutex> guard(impl_->clients_mutex_);
            for (auto& client : impl_->clients_) {
                if (client) client->shutdown();
            }
        }
        if (impl_->accept_thread_.joinable()) impl_->accept_thread_.join();
        for (auto& session : impl_->sessions_) {
            if (session.joinable()) session.join();
        }
    }
}

Status Coordinator::Impl::bootstrap_topology() {
    ResourceId resource = ResourceId::from_raw(resource_id_);
    PoolId pool = PoolId::from_raw(pool_id_);
    QueueId queue = QueueId::from_raw(queue_id_);
    PolicyId policy_id = PolicyId::from_raw(policy_id_);

    resource_id_ = config_.bootstrap_resource_id;
    pool_id_ = config_.bootstrap_pool_id;
    queue_id_ = config_.bootstrap_queue_id;
    policy_id_ = config_.bootstrap_policy_id;
    capacity_units_ = config_.bootstrap_resource_units;
    resource = ResourceId::from_raw(resource_id_);
    pool = PoolId::from_raw(pool_id_);
    queue = QueueId::from_raw(queue_id_);
    policy_id = PolicyId::from_raw(policy_id_);

    backend_.set_resource(resource, config_.bootstrap_resource_units, Generation::from_raw(1));
    BF_TRY(fabric_.attach_backend(&backend_));
    {
        auto capacity_generation = fabric_.set_capacity(
            resource, backend_.id(), config_.bootstrap_resource_units, Generation{});
        if (!capacity_generation.ok()) return capacity_generation.status();
    }

    PoolPolicy policy;
    policy.id = policy_id;
    policy.name = "bootstrap";
    policy.overcommit.mode = OvercommitMode::Forbid;
    policy.borrow.mode = BorrowMode::Forbid;
    policy.pressure.evidence_ttl_ticks = config_.evidence_ttl_ticks;
    policy.pressure.thresholds = Thresholds{};
    policy.reclaim.enabled = true;
    {
        auto policy_generation = fabric_.publish_policy(policy);
        if (!policy_generation.ok()) return policy_generation.status();
    }

    // Bootstrap is idempotent: after a restart the topology is already present
    // in the durable image and is left exactly as it was restored.
    auto existing_pool = fabric_.pool_view(pool);
    if (!existing_pool.ok()) {
        if (existing_pool.code() != ErrorCode::UnknownPool) return existing_pool.status();
        PoolRegistration pool_registration;
        pool_registration.id = pool;
        pool_registration.name = "bootstrap-pool";
        pool_registration.resource = resource;
        pool_registration.backend = backend_.id();
        pool_registration.raw_units = config_.bootstrap_pool_units;
        pool_registration.protected_units = config_.bootstrap_protected_units;
        pool_registration.policy = policy_id;
        auto created_pool = fabric_.register_pool(pool_registration);
        if (!created_pool.ok()) return created_pool.status();
    }

    QueueRegistration queue_registration;
    queue_registration.id = queue;
    queue_registration.name = "bootstrap-queue";
    queue_registration.pool = pool;
    queue_registration.demand_units = config_.bootstrap_queue_demand_units;
    auto created_queue = fabric_.register_queue(queue_registration);
    if (!created_queue.ok() && created_queue.code() != ErrorCode::DuplicateQueue &&
        created_queue.code() != ErrorCode::DuplicateName) {
        return created_queue.status();
    }

    // Bootstrap evidence makes the pool decidable immediately. It is a real
    // observation subject to the same freshness, epoch and generation rules as
    // any other; the worker's own observations replace it.
    PressureSnapshot snapshot;
    snapshot.pool = pool;
    snapshot.pool_generation = Generation{};
    snapshot.observed_at = 0;
    snapshot.demand_units = config_.bootstrap_queue_demand_units;
    snapshot.committed_units = 0;
    snapshot.assert_committed = false;
    {
        auto status = fabric_.pool_generation_status(pool);
        if (!status.ok()) return status.status();
        snapshot.pool_generation = status.value().pool_generation;
    }
    snapshot.epoch = fabric_.epoch();
    snapshot.boot = fabric_.boot_incarnation();
    snapshot.ttl_ticks = config_.evidence_ttl_ticks;
    // A bootstrap snapshot is an ordinary observation: it is accepted or
    // refused by the same rules as any other, and a refusal is not fatal
    // because the pool simply stays undecidable until real evidence arrives.
    const Status observed = fabric_.observe_pressure(snapshot);
    if (observed.ok()) return Status::success();
    return Status::success();
}

Status Coordinator::start(const CoordinatorConfig& config) {
    if (impl_->started_.load()) {
        return Status(ErrorCode::LifecycleViolation, "coordinator is already started");
    }
    impl_->config_ = config;
    BF_TRY(ensure_sockets_initialised());

    FabricConfig fabric_config;
    fabric_config.decision_history_capacity = config.decision_history_capacity;
    if (!config.state_directory.empty()) {
        fabric_config.persistence.directory = config.state_directory;
        fabric_config.persistence.mode = config.durability;
    }
    BF_TRY(impl_->fabric_.open(fabric_config));
    if (config.bootstrap_topology) {
        const Status status = impl_->bootstrap_topology();
        if (!status.ok()) {
            const VoidResult closed = impl_->fabric_.close();
            BF_UNUSED(closed);
            return status;
        }
    }

    u16 bound_port = 0;
    auto listener = Socket::listen(config.bind_address, config.port, &bound_port);
    if (!listener.ok()) {
        const VoidResult closed = impl_->fabric_.close();
        BF_UNUSED(closed);
        return listener.status();
    }
    impl_->listener_ = std::move(listener).value();
    impl_->shutdown_requested_.store(false);
    impl_->started_.store(true);
    impl_->accept_thread_ = std::thread([this] { impl_->accept_loop(); });
    return Status::success();
}

void Coordinator::Impl::retire_socket(const std::shared_ptr<Socket>& peer) {
    if (!peer) return;
    peer->shutdown();
}

void Coordinator::Impl::accept_loop() {
    while (!shutdown_requested_.load()) {
        auto peer = listener_.accept_peer();
        if (!peer.ok()) {
            if (shutdown_requested_.load()) break;
            // A failed accept on a live listener is transient; the loop must
            // not spin hot, so it yields before retrying.
            std::this_thread::yield();
            continue;
        }
        auto socket = std::make_shared<Socket>(std::move(peer).value());
        {
            std::lock_guard<std::mutex> guard(clients_mutex_);
            if (clients_.size() >= config_.max_clients) {
                Message reply;
                reply.op = MessageOp::HelloAck;
                ByteWriter body;
                protocol::encode_response(&body, ErrorCode::BoundedResourceExhausted,
                                          "coordinator client capacity reached", nullptr);
                reply.body = body.take();
                const auto frame = encode_message(reply);
                const Status sent = socket->send_all(frame.data(), frame.size());
                BF_UNUSED(sent);
                socket->shutdown();
                socket->close();
                rejected_.fetch_add(1);
                continue;
            }
            clients_.push_back(socket);
        }
        sessions_.emplace_back([this, socket] { serve(socket); });
    }
}

Status Coordinator::Impl::dispatch(const Message& request, Message* reply, bool* close_after) {
    if (reply == nullptr) {
        return Status(ErrorCode::InvalidArgument, "reply pointer is null");
    }
    *close_after = false;
    reply->op = request.op;
    reply->request = request.request;
    reply->epoch = fabric_.epoch();

    auto respond = [&](ErrorCode code, std::string_view message, const ByteWriter* payload) {
        ByteWriter body;
        protocol::encode_response(&body, code, message, payload);
        reply->body = body.take();
        return Status::success();
    };

    if (request.op == MessageOp::Hello) {
        ByteReader reader(request.body);
        protocol::HelloRequest hello;
        if (!protocol::decode_hello_request(&reader, &hello) || !reader.empty()) {
            rejected_.fetch_add(1);
            *close_after = true;
            return respond(ErrorCode::ProtocolViolation, "malformed hello", nullptr);
        }
        if (hello.protocol_version != protocol::kProtocolVersion) {
            rejected_.fetch_add(1);
            std::string text = "wire protocol version " + std::to_string(hello.protocol_version) +
                               " is not supported";
            Status status = respond(ErrorCode::ProtocolVersionMismatch, text, nullptr);
            *close_after = true;
            return status;
        }
        const EpochId current = fabric_.epoch();
        if (hello.claimed_epoch.valid() && hello.claimed_epoch != current) {
            rejected_.fetch_add(1);
            std::string text = "claimed epoch " + std::to_string(hello.claimed_epoch.raw()) +
                               " is not the current epoch " + std::to_string(current.raw());
            Status status = respond(ErrorCode::StaleEpoch, text, nullptr);
            *close_after = true;
            return status;
        }
        const FabricSummary summary = fabric_.summary().value_or(FabricSummary{});
        protocol::HelloResponse ack;
        ack.protocol_version = protocol::kProtocolVersion;
        ack.coordinator_epoch = current;
        ack.coordinator_boot = fabric_.boot_incarnation();
        ack.pool_count = summary.pool_count;
        ack.queue_count = summary.queue_count;
        ByteWriter payload;
        protocol::encode_hello_response(&payload, ack);
        reply->op = MessageOp::HelloAck;
        ByteWriter body;
        protocol::encode_response(&body, ErrorCode::Ok, "hello accepted", &payload);
        reply->body = body.take();
        return Status::success();
    }

    if (request.op == MessageOp::Ping) {
        return respond(ErrorCode::Ok, "pong", nullptr);
    }

    // Every other operation must present the current epoch. A request bound to
    // a previous incarnation can never reach authoritative state.
    if (!request.epoch.valid() || request.epoch != fabric_.epoch()) {
        rejected_.fetch_add(1);
        *close_after = true;
        return respond(ErrorCode::StaleEpoch, "request epoch is not the current fabric epoch",
                       nullptr);
    }

    switch (request.op) {
        case MessageOp::Allocate: {
            ByteReader reader(request.body);
            AllocateRequest allocate_request;
            if (!protocol::decode_allocate(&reader, &allocate_request) || !reader.empty()) {
                rejected_.fetch_add(1);
                return respond(ErrorCode::ProtocolViolation, "malformed allocate request", nullptr);
            }
            auto outcome = fabric_.allocate(allocate_request);
            if (!outcome.ok()) {
                return respond(outcome.code(), outcome.status().message(), nullptr);
            }
            protocol::DecisionSummary summary;
            summary.kind = outcome.value().kind;
            summary.status = outcome.value().status;
            summary.binding = static_cast<u8>(outcome.value().binding);
            summary.pressure = static_cast<u8>(outcome.value().pressure);
            summary.utilization_bp = outcome.value().utilization_bp;
            summary.sequence = outcome.value().sequence;
            summary.requested_units = outcome.value().requested_units;
            summary.granted_units = outcome.value().granted_units;
            summary.pool = outcome.value().pool;
            summary.queue = outcome.value().queue;
            summary.allocation = outcome.value().allocation;
            summary.accounting = outcome.value().accounting;
            ByteWriter payload;
            protocol::encode_decision_summary(&payload, summary);
            served_.fetch_add(1);
            return respond(ErrorCode::Ok, "allocate accepted", &payload);
        }
        case MessageOp::Release: {
            ByteReader reader(request.body);
            ReleaseRequest release_request;
            if (!protocol::decode_release(&reader, &release_request) || !reader.empty()) {
                rejected_.fetch_add(1);
                return respond(ErrorCode::ProtocolViolation, "malformed release request", nullptr);
            }
            auto outcome = fabric_.release(release_request);
            if (!outcome.ok()) {
                return respond(outcome.code(), outcome.status().message(), nullptr);
            }
            protocol::DecisionSummary summary;
            summary.kind = outcome.value().kind;
            summary.status = outcome.value().status;
            summary.binding = static_cast<u8>(outcome.value().binding);
            summary.pressure = static_cast<u8>(outcome.value().pressure);
            summary.utilization_bp = outcome.value().utilization_bp;
            summary.sequence = outcome.value().sequence;
            summary.requested_units = outcome.value().requested_units;
            summary.granted_units = outcome.value().granted_units;
            summary.pool = outcome.value().pool;
            summary.queue = outcome.value().queue;
            summary.allocation = outcome.value().allocation;
            summary.accounting = outcome.value().accounting;
            ByteWriter payload;
            protocol::encode_decision_summary(&payload, summary);
            served_.fetch_add(1);
            return respond(ErrorCode::Ok, "release accepted", &payload);
        }
        case MessageOp::Evaluate: {
            ByteReader reader(request.body);
            EvaluateRequest evaluate_request;
            if (!protocol::decode_evaluate(&reader, &evaluate_request) || !reader.empty()) {
                rejected_.fetch_add(1);
                return respond(ErrorCode::ProtocolViolation, "malformed evaluate request", nullptr);
            }
            auto outcome = fabric_.evaluate(evaluate_request);
            if (!outcome.ok()) {
                return respond(outcome.code(), outcome.status().message(), nullptr);
            }
            protocol::DecisionSummary summary;
            summary.kind = outcome.value().decision.kind;
            summary.status = outcome.value().decision.status;
            summary.binding = static_cast<u8>(outcome.value().decision.binding);
            summary.pressure = static_cast<u8>(outcome.value().decision.pressure);
            summary.utilization_bp = outcome.value().decision.utilization_bp;
            summary.sequence = outcome.value().decision.sequence;
            summary.requested_units = outcome.value().decision.requested_units;
            summary.granted_units = outcome.value().decision.granted_units;
            summary.pool = outcome.value().decision.pool;
            summary.queue = outcome.value().decision.queue;
            summary.accounting = outcome.value().accounting;
            ByteWriter payload;
            protocol::encode_decision_summary(&payload, summary);
            served_.fetch_add(1);
            return respond(ErrorCode::Ok, "evaluation complete", &payload);
        }
        case MessageOp::ObservePressure: {
            ByteReader reader(request.body);
            PressureSnapshot snapshot;
            if (!protocol::decode_pressure(&reader, &snapshot) || !reader.empty()) {
                rejected_.fetch_add(1);
                return respond(ErrorCode::ProtocolViolation, "malformed pressure request", nullptr);
            }
            snapshot.epoch = fabric_.epoch();
            snapshot.boot = fabric_.boot_incarnation();
            if (snapshot.ttl_ticks == 0) {
                snapshot.ttl_ticks = config_.evidence_ttl_ticks;
            }
            if (snapshot.observed_at == 0) {
                snapshot.observed_at = 1;
            }
            // The fabric validates the pool generation itself and refuses a
            // snapshot that describes a different pool revision.
            auto pool_status = fabric_.pool_generation_status(snapshot.pool);
            if (!pool_status.ok()) {
                return respond(pool_status.code(), pool_status.status().message(), nullptr);
            }
            snapshot.pool_generation = pool_status.value().pool_generation;
            const Status status = fabric_.observe_pressure(snapshot);
            served_.fetch_add(1);
            if (!status.ok()) return respond(status.code(), status.message(), nullptr);
            return respond(ErrorCode::Ok, "pressure observed", nullptr);
        }
        case MessageOp::Explain:
        case MessageOp::Summary:
        case MessageOp::Ledger:
        case MessageOp::Metrics: {
            std::string text;
            if (request.op == MessageOp::Summary) {
                text = fabric_.summary().value_or(FabricSummary{}).to_json();
            } else if (request.op == MessageOp::Metrics) {
                const FabricMetrics metrics = fabric_.metrics();
                JsonWriter writer(1024);
                writer.begin_object();
                writer.key("decisions");
                writer.value_u64(metrics.decisions);
                writer.key("grants");
                writer.value_u64(metrics.grants);
                writer.key("refusals");
                writer.value_u64(metrics.refusals);
                writer.key("live_allocations");
                writer.value_u64(metrics.live_allocations);
                writer.key("live_committed_units");
                writer.value_u64(metrics.live_committed_units);
                writer.end_object();
                text = writer.str();
            } else if (request.op == MessageOp::Ledger) {
                JsonWriter writer(8192);
                writer.begin_array();
                for (const auto& row : fabric_.ledger().value_or(std::vector<PoolAccounting>{})) {
                    writer.begin_object();
                    writer.key("pool");
                    writer.value_u64(row.pool.raw());
                    writer.key("raw");
                    writer.value_u64(row.raw);
                    writer.key("protected");
                    writer.value_u64(row.protected_units);
                    writer.key("allocated");
                    writer.value_u64(row.allocated);
                    writer.key("free");
                    writer.value_u64(row.free);
                    writer.end_object();
                }
                writer.end_array();
                text = writer.str();
            } else {
                ByteReader reader(request.body);
                u64 pool_raw = 0;
                if (!reader.get_u64(&pool_raw) || !reader.empty()) {
                    rejected_.fetch_add(1);
                    return respond(ErrorCode::ProtocolViolation, "malformed explain request",
                                   nullptr);
                }
                auto explanation = fabric_.explain(PoolId::from_raw(pool_raw));
                if (!explanation.ok()) {
                    return respond(explanation.code(), explanation.status().message(), nullptr);
                }
                text = explanation.value().to_text();
            }
            ByteWriter payload;
            if (text.size() > kMaxFrameBytes - 64) {
                text.resize(kMaxFrameBytes - 64);
            }
            payload.put_u32(static_cast<u32>(text.size()));
            payload.put_bytes(text.data(), text.size());
            served_.fetch_add(1);
            return respond(ErrorCode::Ok, "ok", &payload);
        }
        case MessageOp::Shutdown: {
            served_.fetch_add(1);
            Status status = respond(ErrorCode::Ok, "shutdown requested", nullptr);
            shutdown_requested_.store(true);
            listener_.shutdown();
            listener_.close();
            *close_after = true;
            return status;
        }
        case MessageOp::Hello:
        case MessageOp::HelloAck:
        case MessageOp::Ping:
        case MessageOp::Pong:
            break;
    }
    rejected_.fetch_add(1);
    return respond(ErrorCode::ProtocolViolation, "unsupported operation", nullptr);
}

void Coordinator::Impl::serve(std::shared_ptr<Socket> peer) {
    for (;;) {
        if (shutdown_requested_.load()) break;
        auto frame = peer->receive_frame(config_.max_frame_bytes);
        if (!frame.ok()) break;
        // receive_frame has already verified and removed the transport frame,
        // so the payload is decoded directly.
        auto message = decode_message_payload(frame.value().data(), frame.value().size(),
                                              config_.max_frame_bytes);
        Message reply;
        reply.request = RequestId::from_raw(0);
        if (!message.ok()) {
            ByteWriter body;
            protocol::encode_response(&body, message.code(), message.status().message(), nullptr);
            reply.op = MessageOp::Pong;
            reply.epoch = fabric_.epoch();
            reply.body = body.take();
            const auto encoded = encode_message(reply);
            const Status sent = peer->send_all(encoded.data(), encoded.size());
            BF_UNUSED(sent);
            rejected_.fetch_add(1);
            break;
        }
        bool close_after = false;
        const Status status = dispatch(message.value(), &reply, &close_after);
        if (!status.ok()) break;
        const auto encoded = encode_message(reply);
        const Status sent = peer->send_all(encoded.data(), encoded.size());
        if (!sent.ok()) break;
        if (close_after) break;
    }
    peer->shutdown();
    peer->close();
}

Result<u16> Coordinator::port() const {
    if (!impl_->listener_.valid()) {
        return Status(ErrorCode::LifecycleViolation, "coordinator is not listening");
    }
    return impl_->listener_.local_port();
}

EpochId Coordinator::epoch() const { return impl_->fabric_.epoch(); }

void Coordinator::request_shutdown() noexcept {
    impl_->shutdown_requested_.store(true);
    if (impl_->listener_.valid()) {
        impl_->listener_.shutdown();
        impl_->listener_.close();
    }
    std::lock_guard<std::mutex> guard(impl_->clients_mutex_);
    for (auto& client : impl_->clients_) {
        if (client) client->shutdown();
    }
}

Status Coordinator::stop() {
    if (!impl_->started_.load()) return Status::success();
    request_shutdown();
    if (impl_->accept_thread_.joinable()) impl_->accept_thread_.join();
    for (auto& session : impl_->sessions_) {
        if (session.joinable()) session.join();
    }
    impl_->sessions_.clear();
    {
        std::lock_guard<std::mutex> guard(impl_->clients_mutex_);
        impl_->clients_.clear();
    }
    impl_->started_.store(false);
    // The fabric lock is not held here, and no session thread is running, so
    // closing the fabric cannot invert lock order with a worker.
    return impl_->fabric_.close().status();
}

bool Coordinator::stopped() const noexcept { return impl_->shutdown_requested_.load(); }

FabricMetrics Coordinator::metrics() const { return impl_->fabric_.metrics(); }
u64 Coordinator::served_requests() const noexcept { return impl_->served_.load(); }
u64 Coordinator::rejected_requests() const noexcept { return impl_->rejected_.load(); }
BufferFabric& Coordinator::fabric() noexcept { return impl_->fabric_; }

}  // namespace buffer_fabric
