// bf_coordinator -- hosts an authoritative Buffer Fabric and serves framed
// requests from real worker processes over a real TCP socket on an explicitly
// chosen address.
//
// The process prints exactly one line, "READY <port>", once it is listening,
// and then serves until it is asked to shut down or is terminated.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "buffer_fabric/coordinator.hpp"
#include "buffer_fabric/version.hpp"
#include "tool_args.hpp"

using namespace buffer_fabric;

int main(int argc, char** argv) {
    const bftool::Arguments arguments = bftool::parse(argc, argv);
    if (arguments.has("--help")) {
        bftool::print_usage("bf_coordinator",
                            "[--address A] [--port N] [--state-dir DIR] [--no-durability] "
                            "[--run-seconds N] [--pool-units N] [--protected-units N]");
        return 0;
    }

    CoordinatorConfig config;
    config.bind_address = arguments.value("--address", "127.0.0.1");
    config.port = static_cast<u16>(arguments.number("--port", 0));
    config.state_directory = arguments.value("--state-dir", "");
    if (arguments.has("--no-durability")) {
        config.durability = DurabilityMode::None;
    }
    config.bootstrap_pool_units = arguments.number("--pool-units", config.bootstrap_pool_units);
    config.bootstrap_protected_units =
        arguments.number("--protected-units", config.bootstrap_protected_units);
    config.bootstrap_resource_units =
        arguments.number("--resource-units", config.bootstrap_resource_units);

    Coordinator coordinator;
    const Status started = coordinator.start(config);
    if (!started.ok()) {
        std::fprintf(stderr, "coordinator failed to start: %s\n", started.to_string().c_str());
        return 1;
    }
    auto port = coordinator.port();
    if (!port.ok()) {
        std::fprintf(stderr, "coordinator has no bound port\n");
        return 1;
    }
    std::printf("READY %u\n", static_cast<unsigned>(port.value()));
    std::fflush(stdout);

    const unsigned long long run_seconds = arguments.number("--run-seconds", 0);
    if (run_seconds != 0) {
        std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
        coordinator.request_shutdown();
    } else {
        // Serve until the listener is closed by a shutdown request or by the
        // operating system tearing the process down.
        while (!coordinator.stopped()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    const Status stopped = coordinator.stop();
    if (!stopped.ok()) {
        std::fprintf(stderr, "coordinator failed to stop cleanly: %s\n",
                     stopped.to_string().c_str());
        return 1;
    }
    std::printf("STOPPED served=%llu rejected=%llu\n",
                static_cast<unsigned long long>(coordinator.served_requests()),
                static_cast<unsigned long long>(coordinator.rejected_requests()));
    return 0;
}
