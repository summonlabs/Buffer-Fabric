// Real multi-process validation.
//
// Every test here launches real operating-system processes that communicate
// over a real framed TCP transport. Nothing is simulated in-process.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "buffer_fabric/process.hpp"
#include "buffer_fabric/transport.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

using namespace buffer_fabric;
using bftest::TempDirectory;

namespace {

constexpr const char* kCoordinatorExe = BF_COORDINATOR_EXE;
constexpr const char* kWorkerExe = BF_WORKER_EXE;

struct ToolRun {
    i32 exit_code{0};
    std::string output{};
    bool completed{false};
};

ToolRun run_tool(const std::string& executable, const std::vector<std::string>& arguments) {
    ProcessOptions options;
    options.executable = executable;
    options.arguments = arguments;
    options.capture_stdout = true;
    options.merge_stderr = true;
    ToolRun run;
    auto child = spawn_process(options);
    if (!child.ok()) {
        run.output = "spawn failed: " + child.status().to_string();
        return run;
    }
    ChildProcess process = std::move(child).value();
    for (;;) {
        auto line = process.read_line();
        if (!line.ok()) break;
        run.output.append(line.value());
        run.output.push_back('\n');
    }
    auto code = process.wait();
    if (code.ok()) run.exit_code = code.value();
    run.completed = true;
    return run;
}

std::string field(const std::string& output, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t position = output.find(needle);
    while (position != std::string::npos) {
        const bool at_line_start = position == 0 || output[position - 1] == '\n';
        if (at_line_start) {
            const std::size_t begin = position + needle.size();
            const std::size_t end = output.find('\n', begin);
            return output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        }
        position = output.find(needle, position + 1);
    }
    return std::string{};
}

u64 field_u64(const std::string& output, const std::string& key, u64 fallback = 0) {
    const std::string text = field(output, key);
    if (text.empty()) return fallback;
    return std::strtoull(text.c_str(), nullptr, 10);
}

/// The value that follows \p prefix on the first line that starts with it.
std::string line_prefixed_value(const std::string& output, const std::string& prefix) {
    std::size_t position = 0;
    while (position <= output.size()) {
        const std::size_t end = output.find('\n', position);
        const std::size_t length =
            end == std::string::npos ? std::string::npos : end - position;
        const std::string line = output.substr(position, length);
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        if (end == std::string::npos) break;
        position = end + 1;
    }
    return std::string{};
}

/// Launch a coordinator and wait for its READY line. The wait is bounded by
/// the coordinator's own progress, not by a watchdog: the read blocks until
/// the child writes or exits.
struct RunningCoordinator {
    ChildProcess process{};
    u16 port{0};
    std::string preamble{};
    bool ready{false};
    TempDirectory* directory{nullptr};

    ~RunningCoordinator() {
        if (process.valid()) {
            const Status terminated = process.terminate();
            BF_UNUSED(terminated);
            (void)process.wait();
        }
    }
};

bool start_coordinator(RunningCoordinator* running, TempDirectory* directory,
                       const std::vector<std::string>& extra = {}) {
    std::vector<std::string> arguments = {"--port", "0", "--state-dir", directory->path()};
    for (const auto& item : extra) arguments.push_back(item);
    ProcessOptions options;
    options.executable = kCoordinatorExe;
    options.arguments = arguments;
    options.capture_stdout = true;
    options.merge_stderr = true;
    auto child = spawn_process(options);
    if (!child.ok()) return false;
    running->process = std::move(child).value();
    running->directory = directory;
    for (;;) {
        auto line = running->process.read_line();
        if (!line.ok()) return false;
        running->preamble.append(line.value());
        running->preamble.push_back('\n');
        const std::string ready_line = line_prefixed_value(running->preamble, "READY ");
        if (!ready_line.empty()) {
            running->port = static_cast<u16>(std::strtoul(ready_line.c_str(), nullptr, 10));
            running->ready = running->port != 0;
            return running->ready;
        }
    }
}

}  // namespace

BF_TEST(multiprocess, worker_allocates_and_releases_against_a_real_coordinator) {
    TempDirectory directory("mp-basic");
    RunningCoordinator coordinator;
    BF_REQUIRE(start_coordinator(&coordinator, &directory));

    const ToolRun run = run_tool(kWorkerExe,
                                 {"--port", std::to_string(coordinator.port), "--pool", "1",
                                  "--queue", "1", "--units", "4096", "--ops", "32",
                                  "--label", "basic"});
    BF_CHECK(run.completed);
    BF_CHECK_EQ(run.exit_code, i32{0});
    BF_CHECK_EQ(field(run.output, "connected"), std::string("true"));
    BF_CHECK_EQ(field(run.output, "handshake_ok"), std::string("true"));
    BF_CHECK_EQ(field(run.output, "stale_epoch_rejected"), std::string("false"));
    BF_CHECK_EQ(field(run.output, "protocol_error"), std::string("false"));
    BF_CHECK_EQ(field_u64(run.output, "operations"), u64{32});
    BF_CHECK_EQ(field_u64(run.output, "grants"), u64{32});
    BF_CHECK_EQ(field_u64(run.output, "release_ok"), u64{32});
    BF_CHECK_EQ(field_u64(run.output, "granted_units"), u64{32 * 4096});
    BF_CHECK_EQ(field_u64(run.output, "released_units"), u64{32 * 4096});
    BF_CHECK(field_u64(run.output, "bytes_sent") > 0);
    BF_CHECK(field_u64(run.output, "bytes_received") > 0);
    const u64 epoch = field_u64(run.output, "coordinator_epoch");
    BF_CHECK(epoch > 0);
}

BF_TEST(multiprocess, malformed_traffic_does_not_disturb_the_coordinator) {
    TempDirectory directory("mp-malformed");
    RunningCoordinator coordinator;
    BF_REQUIRE(start_coordinator(&coordinator, &directory));

    // A peer that sends bytes which are not a frame.
    {
        auto socket = Socket::connect_to("127.0.0.1", coordinator.port);
        BF_REQUIRE(socket.ok());
        const char garbage[] = "this is not a framed message at all";
        BF_CHECK(socket.value().send_all(garbage, sizeof(garbage) - 1).ok());
        socket.value().shutdown();
        socket.value().close();
    }
    // A peer that sends a well-formed frame carrying a version the coordinator
    // does not support.
    {
        auto socket = Socket::connect_to("127.0.0.1", coordinator.port);
        BF_REQUIRE(socket.ok());
        ByteWriter body;
        body.put_u8(static_cast<u8>(MessageOp::Ping));
        body.put_u64(1);
        body.put_u64(0);
        body.put_u32(0);
        const std::vector<u8> frame =
            encode_frame(body.data().data(), body.size(), kTransportMagic, 4242u);
        BF_CHECK(socket.value().send_all(frame.data(), frame.size()).ok());
        socket.value().shutdown();
        socket.value().close();
    }
    // The coordinator is still healthy and still serves real workers.
    const ToolRun run = run_tool(kWorkerExe,
                                 {"--port", std::to_string(coordinator.port), "--pool", "1",
                                  "--queue", "1", "--units", "128", "--ops", "4"});
    BF_CHECK_EQ(run.exit_code, i32{0});
    BF_CHECK_EQ(field(run.output, "handshake_ok"), std::string("true"));
}

BF_TEST(multiprocess, hard_kill_and_restart_advances_the_epoch_and_fences_stale_workers) {
    TempDirectory directory("mp-restart");

    EpochId first_epoch{};
    u16 first_port = 0;
    {
        RunningCoordinator coordinator;
        BF_REQUIRE(start_coordinator(&coordinator, &directory));
        first_port = coordinator.port;
        const ToolRun run = run_tool(kWorkerExe,
                                     {"--port", std::to_string(first_port), "--pool", "1",
                                      "--queue", "1", "--units", "256", "--ops", "8",
                                      "--label", "before"});
        BF_REQUIRE(run.exit_code == 0);
        first_epoch = EpochId::from_raw(field_u64(run.output, "coordinator_epoch"));
        BF_CHECK(first_epoch.valid());

        // Hard kill: no graceful shutdown, no flush, no cleanup.
        BF_REQUIRE(coordinator.process.terminate().ok());
        const auto code = coordinator.process.wait();
        BF_CHECK(code.ok());
        coordinator.ready = false;
    }

    {
        RunningCoordinator restarted;
        BF_REQUIRE(start_coordinator(&restarted, &directory));
        BF_CHECK(restarted.port != 0);

        // A worker that still believes in the previous epoch must be refused.
        const ToolRun stale = run_tool(kWorkerExe,
                                       {"--port", std::to_string(restarted.port), "--pool", "1",
                                        "--queue", "1", "--units", "256", "--ops", "4",
                                        "--claim-epoch", std::to_string(first_epoch.raw()),
                                        "--label", "stale"});
        BF_CHECK(stale.completed);
        BF_CHECK_EQ(field(stale.output, "stale_epoch_rejected"), std::string("true"));
        BF_CHECK_EQ(field(stale.output, "handshake_ok"), std::string("false"));
        BF_CHECK_EQ(field_u64(stale.output, "operations"), u64{0});

        // A worker that accepts the new epoch is served normally, and the new
        // epoch is strictly greater than the one before the kill.
        const ToolRun fresh = run_tool(kWorkerExe,
                                       {"--port", std::to_string(restarted.port), "--pool", "1",
                                        "--queue", "1", "--units", "256", "--ops", "8",
                                        "--label", "after"});
        BF_CHECK_EQ(fresh.exit_code, i32{0});
        BF_CHECK_EQ(field(fresh.output, "handshake_ok"), std::string("true"));
        const u64 new_epoch = field_u64(fresh.output, "coordinator_epoch");
        BF_CHECK(new_epoch > first_epoch.raw());

        // Committed allocations survive the restart: the durability image is
        // real, and the accounting still closes.
        BF_REQUIRE(restarted.process.terminate().ok());
        const auto code = restarted.process.wait();
        BF_CHECK(code.ok());
    }
}

BF_TEST(multiprocess, coordinator_stops_cleanly_on_request) {
    TempDirectory directory("mp-shutdown");
    {
        RunningCoordinator coordinator;
        BF_REQUIRE(start_coordinator(&coordinator, &directory, {"--run-seconds", "1"}));
        const auto code = coordinator.process.wait();
        BF_REQUIRE(code.ok());
        BF_CHECK_EQ(code.value(), i32{0});
    }
}

BF_TEST(multiprocess, concurrent_workers_share_one_authoritative_fabric) {
    TempDirectory directory("mp-concurrent");
    RunningCoordinator coordinator;
    BF_REQUIRE(start_coordinator(&coordinator, &directory));

    std::vector<ToolRun> runs(4);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < runs.size(); ++index) {
        threads.emplace_back([&, index] {
            // Attempt identities are fabric-wide, so each worker gets its own
            // range; otherwise a worker would receive an idempotent replay of
            // another worker's request.
            runs[index] =
                run_tool(kWorkerExe,
                         {"--port", std::to_string(coordinator.port), "--pool", "1",
                          "--queue", "1", "--units", "512", "--ops", "24",
                          "--attempt-base", std::to_string(index * 1000000ull),
                          "--label", "worker-" + std::to_string(index)});
        });
    }
    for (auto& thread : threads) thread.join();

    u64 total_grants = 0;
    for (const auto& run : runs) {
        BF_CHECK_EQ(run.exit_code, i32{0});
        BF_CHECK_EQ(field(run.output, "handshake_ok"), std::string("true"));
        total_grants += field_u64(run.output, "grants");
    }
    BF_CHECK_EQ(total_grants, u64{4 * 24});
}
