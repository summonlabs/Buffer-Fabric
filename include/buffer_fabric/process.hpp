#pragma once

#include <string>
#include <vector>

#include "buffer_fabric/config.hpp"
#include "buffer_fabric/status.hpp"
#include "buffer_fabric/types.hpp"

namespace buffer_fabric {

/// Options for launching a real operating-system process.
struct BF_API ProcessOptions {
    std::string executable{};
    std::vector<std::string> arguments{};
    std::string working_directory{};
    /// Redirect the child's standard output into a pipe readable through
    /// ChildProcess::read_line.
    bool capture_stdout{true};
    /// Redirect the child's standard error into the same pipe as stdout.
    bool merge_stderr{true};
    /// Environment overrides, applied on top of the parent environment.
    std::vector<std::string> environment{};
};

/// A launched child process. Reading from the child blocks; there is no
/// timeout, by design: a stalled child is a defect to diagnose, not something
/// to paper over with a watchdog.
class BF_API ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool running() const;
    [[nodiscard]] u64 pid() const noexcept { return pid_; }

    /// Read one newline-terminated line (or up to \p max_bytes) from the
    /// child's captured output. Returns NotFound at clean end of output.
    [[nodiscard]] Result<std::string> read_line(usize max_bytes = 8192);

    /// Wait for the process to exit and report its exit code.
    [[nodiscard]] Result<i32> wait();

    /// Forcefully terminate the process. Returns immediately; the process is
    /// reaped by wait() or by the destructor.
    [[nodiscard]] Status terminate();

private:
    friend Result<ChildProcess> spawn_process(const ProcessOptions& options);

    u64 pid_{0};
    void* process_handle_{nullptr};
    void* read_handle_{nullptr};
    void* write_handle_{nullptr};
    bool exited_{false};
    i32 exit_code_{0};
};

[[nodiscard]] BF_API Result<ChildProcess> spawn_process(const ProcessOptions& options);

/// Absolute path of the directory containing the running executable.
[[nodiscard]] BF_API Result<std::string> executable_directory();

/// Absolute path of the running executable.
[[nodiscard]] BF_API Result<std::string> executable_path();

}  // namespace buffer_fabric
