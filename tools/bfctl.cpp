// bfctl -- operator command line for Buffer Fabric.
//
// Every invocation opens the fabric from a durable state directory, performs
// one command, and closes it. Because the fabric advances its epoch on every
// open, each invocation is a genuine restart: pressure evidence from a
// previous invocation is reported as STALE and must be revalidated, which is
// exactly the behaviour an operator needs to observe.

#include <cstdio>
#include <exception>
#include <string>

#include "buffer_fabric/backend.hpp"
#include "buffer_fabric/fabric.hpp"
#include "buffer_fabric/version.hpp"
#include "tool_args.hpp"

using namespace buffer_fabric;

namespace {

constexpr u64 kBackendId = 1;
constexpr u64 kResourceId = 1;
constexpr u64 kPolicyId = 1;
constexpr u64 kPoolId = 1;

FabricConfig make_config(const std::string& directory, bool durable) {
    FabricConfig config;
    if (durable && !directory.empty()) {
        config.persistence.directory = directory;
        config.persistence.mode = DurabilityMode::JournalSync;
    }
    return config;
}

int fail(const Status& status) {
    std::fprintf(stderr, "error: %s\n", status.to_string().c_str());
    return 1;
}

int run_command(const std::string& command, const bftool::Arguments& arguments,
                SyntheticBackend& backend, BufferFabric& fabric) {
    if (command == "init") {
        const u64 capacity = arguments.number("--capacity", 1u << 20);
        const u64 pool_units = arguments.number("--pool-units", 1u << 19);
        const u64 protected_units = arguments.number("--protected", 1u << 12);
        const u64 queue_id = arguments.number("--queue-id", 1);
        const u64 demand = arguments.number("--demand", 1u << 17);

        backend.set_resource(ResourceId::from_raw(kResourceId), capacity, Generation::from_raw(1));
        auto capacity_generation = fabric.set_capacity(ResourceId::from_raw(kResourceId),
                                                       backend.id(), capacity, Generation{});
        if (!capacity_generation.ok()) return fail(capacity_generation.status());

        PoolPolicy policy;
        policy.id = PolicyId::from_raw(kPolicyId);
        policy.name = "bfctl";
        auto published = fabric.publish_policy(policy);
        if (!published.ok()) return fail(published.status());

        auto existing_pool = fabric.pool_view(PoolId::from_raw(kPoolId));
        if (!existing_pool.ok()) {
            PoolRegistration registration;
            registration.id = PoolId::from_raw(kPoolId);
            registration.name = "bfctl-pool";
            registration.resource = ResourceId::from_raw(kResourceId);
            registration.backend = backend.id();
            registration.raw_units = pool_units;
            registration.protected_units = protected_units;
            registration.policy = PolicyId::from_raw(kPolicyId);
            auto created = fabric.register_pool(registration);
            if (!created.ok()) return fail(created.status());
        }

        QueueRegistration queue_registration;
        queue_registration.id = QueueId::from_raw(queue_id);
        queue_registration.name = "bfctl-queue";
        queue_registration.pool = PoolId::from_raw(kPoolId);
        queue_registration.demand_units = demand;
        auto queue_created = fabric.register_queue(queue_registration);
        if (!queue_created.ok() && queue_created.code() != ErrorCode::DuplicateQueue &&
            queue_created.code() != ErrorCode::DuplicateName) {
            return fail(queue_created.status());
        }
        std::printf("initialized capacity=%llu pool=%llu protected=%llu queue=%llu\n",
                    static_cast<unsigned long long>(capacity),
                    static_cast<unsigned long long>(pool_units),
                    static_cast<unsigned long long>(protected_units),
                    static_cast<unsigned long long>(queue_id));
        return 0;
    }

    if (command == "summary") {
        auto summary = fabric.summary();
        if (!summary.ok()) return fail(summary.status());
        std::printf("%s\n", summary.value().to_json().c_str());
        return summary.value().accounting.closed ? 0 : 1;
    }
    if (command == "ledger") {
        auto text = fabric.accounting_text();
        if (!text.ok()) return fail(text.status());
        std::printf("%s", text.value().c_str());
        return 0;
    }
    if (command == "verify") {
        const AccountingReport report = fabric.validate_accounting();
        std::printf("pools=%llu closure=%s\n",
                    static_cast<unsigned long long>(report.pools_checked),
                    report.closed ? "closed" : "VIOLATED");
        for (const auto& violation : report.violations) {
            std::printf("  fault=%s pool=%llu %s\n",
                        std::string(to_string(violation.fault)).c_str(),
                        static_cast<unsigned long long>(violation.pool.raw()),
                        violation.detail.c_str());
        }
        return report.closed ? 0 : 1;
    }
    if (command == "checkpoint") {
        const Status status = fabric.checkpoint();
        if (!status.ok()) return fail(status);
        std::printf("checkpoint written\n");
        return 0;
    }
    if (command == "explain") {
        auto explanation = fabric.explain(PoolId::from_raw(arguments.number("--pool", kPoolId)));
        if (!explanation.ok()) return fail(explanation.status());
        std::printf("%s\n", explanation.value().to_json().c_str());
        return 0;
    }
    if (command == "observe") {
        const PoolId pool = PoolId::from_raw(arguments.number("--pool", kPoolId));
        auto explanation = fabric.explain(pool);
        if (!explanation.ok()) return fail(explanation.status());
        PressureSnapshot snapshot;
        snapshot.pool = pool;
        snapshot.pool_generation = explanation.value().pool_generation;
        snapshot.epoch = fabric.epoch();
        snapshot.boot = fabric.boot_incarnation();
        snapshot.ttl_ticks = arguments.number("--ttl", 10'000'000'000ull);
        snapshot.demand_units = arguments.number("--demand", 0);
        snapshot.committed_units = explanation.value().allocated_units -
                                   explanation.value().borrowed_in_units +
                                   explanation.value().lent_out_units;
        snapshot.assert_committed = true;
        const Status status = fabric.observe_pressure(snapshot);
        if (!status.ok()) return fail(status);
        std::printf("observed pool=%llu committed=%llu\n",
                    static_cast<unsigned long long>(pool.raw()),
                    static_cast<unsigned long long>(snapshot.committed_units));
        return 0;
    }
    if (command == "allocate") {
        AllocateRequest request;
        request.attempt = AttemptId::from_raw(arguments.number("--attempt", 1));
        request.pool = PoolId::from_raw(arguments.number("--pool", kPoolId));
        request.queue = QueueId::from_raw(arguments.number("--queue-id", 1));
        request.requested_units = arguments.number("--units", 1);
        request.commit_immediately = !arguments.has("--reserve");
        request.reclaim_class =
            arguments.has("--pinned") ? ReclaimClass::Pinned : ReclaimClass::Reclaimable;
        auto decision = fabric.allocate(request);
        if (!decision.ok()) return fail(decision.status());
        std::printf(
            "kind=%s status=%s granted=%llu requested=%llu allocation=%llu binding=%s "
            "pressure=%s\n",
            std::string(to_string(decision.value().kind)).c_str(),
            std::string(to_string(decision.value().status)).c_str(),
            static_cast<unsigned long long>(decision.value().granted_units),
            static_cast<unsigned long long>(request.requested_units),
            static_cast<unsigned long long>(decision.value().allocation.raw()),
            std::string(to_string(decision.value().binding)).c_str(),
            std::string(to_string(decision.value().pressure)).c_str());
        return decision.value().status == ErrorCode::Ok ? 0 : 1;
    }
    if (command == "release") {
        ReleaseRequest request;
        request.attempt = AttemptId::from_raw(arguments.number("--attempt", 1));
        request.allocation = AllocationId::from_raw(arguments.number("--allocation", 0));
        const u64 units = arguments.number("--units", 0);
        request.release_all = units == 0;
        request.units = units;
        auto decision = fabric.release(request);
        if (!decision.ok()) return fail(decision.status());
        std::printf("kind=%s released=%llu\n",
                    std::string(to_string(decision.value().kind)).c_str(),
                    static_cast<unsigned long long>(decision.value().granted_units));
        return 0;
    }
    std::fprintf(stderr, "unknown command: %s\n", command.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const bftool::Arguments arguments = bftool::parse(argc, argv);
    const std::string command = bftool::first_positional(arguments);
    if (argc < 2 || arguments.has("--help") || command.empty()) {
        bftool::print_usage(
            "bfctl",
            "--state-dir DIR [--volatile] <command> [args]\n"
            "  commands:\n"
            "    capabilities\n"
            "    init --capacity N --pool-units N --protected N --queue-id N [--demand N]\n"
            "    summary | ledger | verify | checkpoint\n"
            "    explain --pool N\n"
            "    observe --pool N [--demand N] [--ttl TICKS]\n"
            "    allocate --pool N --queue-id N --units N --attempt N [--reserve] [--pinned]\n"
            "    release --allocation N --attempt N [--units N]");
        return command.empty() ? 1 : 0;
    }

    if (command == "capabilities") {
        std::printf("%s\n", capabilities().to_json().c_str());
        return 0;
    }

    const std::string state_directory = arguments.value("--state-dir", "");
    const bool durable = !arguments.has("--volatile") && !state_directory.empty();

    SyntheticBackend backend(BackendId::from_raw(kBackendId), Generation::from_raw(1), "bfctl");
    BufferFabric fabric;
    Status opened = fabric.open(make_config(state_directory, durable));
    if (!opened.ok()) return fail(opened);
    const VoidResult attached = fabric.attach_backend(&backend);
    if (!attached.ok()) return fail(attached.status());

    int code = 1;
    try {
        code = run_command(command, arguments, backend, fabric);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unhandled: %s\n", error.what());
        code = 1;
    }
    const VoidResult closed = fabric.close();
    if (!closed.ok() && code == 0) return fail(closed.status());
    return code;
}
