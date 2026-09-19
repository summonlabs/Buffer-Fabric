// bf_worker -- a real worker process that connects to a coordinator over a
// framed TCP transport, performs a handshake, and then runs allocate/release
// cycles. It reports exactly what happened, including an epoch rejection,
// which is a normal outcome rather than a transport failure.

#include <cstdio>
#include <string>

#include "buffer_fabric/version.hpp"
#include "buffer_fabric/worker.hpp"
#include "tool_args.hpp"

using namespace buffer_fabric;

int main(int argc, char** argv) {
    const bftool::Arguments arguments = bftool::parse(argc, argv);
    if (arguments.has("--help")) {
        bftool::print_usage("bf_worker",
                            "--port N --pool N --queue N [--host H] [--units N] [--ops N] "
                            "[--claim-epoch N] [--expected-epoch N] [--attempt-base N] "
                            "[--label TEXT]");
        return 0;
    }
    WorkerConfig config;
    config.host = arguments.value("--host", "127.0.0.1");
    config.port = static_cast<u16>(arguments.number("--port", 0));
    config.pool = PoolId::from_raw(arguments.number("--pool", 1));
    config.queue = QueueId::from_raw(arguments.number("--queue", 1));
    config.units = arguments.number("--units", 64);
    config.operations = arguments.number("--ops", 16);
    config.claim_epoch = EpochId::from_raw(arguments.number("--claim-epoch", 0));
    config.expected_epoch = EpochId::from_raw(arguments.number("--expected-epoch", 0));
    config.attempt_base = arguments.number("--attempt-base", 0);
    const std::string label = arguments.value("--label", "worker");

    auto outcome = run_worker(config);
    if (!outcome.ok()) {
        std::printf("label=%s\n", label.c_str());
        std::printf("fatal=%s\n", outcome.status().to_string().c_str());
        std::printf("worker_report_end\n");
        std::fflush(stdout);
        return 2;
    }
    const WorkerReport& report = outcome.value();
    std::printf("label=%s\n", label.c_str());
    std::printf("connected=%s\n", report.connected ? "true" : "false");
    std::printf("handshake_ok=%s\n", report.handshake_ok ? "true" : "false");
    std::printf("stale_epoch_rejected=%s\n", report.stale_epoch_rejected ? "true" : "false");
    std::printf("protocol_error=%s\n", report.protocol_error ? "true" : "false");
    std::printf("operations=%llu\n", static_cast<unsigned long long>(report.operations_attempted));
    std::printf("grants=%llu\n", static_cast<unsigned long long>(report.grants));
    std::printf("partial_grants=%llu\n", static_cast<unsigned long long>(report.partial_grants));
    std::printf("refusals=%llu\n", static_cast<unsigned long long>(report.refusals));
    std::printf("release_ok=%llu\n", static_cast<unsigned long long>(report.release_ok));
    std::printf("error_responses=%llu\n", static_cast<unsigned long long>(report.error_responses));
    std::printf("granted_units=%llu\n", static_cast<unsigned long long>(report.granted_units));
    std::printf("released_units=%llu\n", static_cast<unsigned long long>(report.released_units));
    std::printf("bytes_sent=%llu\n", static_cast<unsigned long long>(report.bytes_sent));
    std::printf("bytes_received=%llu\n", static_cast<unsigned long long>(report.bytes_received));
    std::printf("coordinator_epoch=%llu\n",
                static_cast<unsigned long long>(report.coordinator_epoch.raw()));
    std::printf("allocated_total=%llu\n",
                static_cast<unsigned long long>(report.accounting.allocated_total));
    std::printf("free_total=%llu\n", static_cast<unsigned long long>(report.accounting.free_total));
    std::printf("worker_report_end\n");
    std::fflush(stdout);
    return report.protocol_error ? 3 : 0;
}
