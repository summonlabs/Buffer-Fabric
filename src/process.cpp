#include "buffer_fabric/process.hpp"

#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace buffer_fabric {
namespace {

#if defined(_WIN32)
[[nodiscard]] std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<usize>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(),
                          needed);
    return out;
}

/// Quote one argument for the Windows command line.
[[nodiscard]] std::string quote_argument(const std::string& argument) {
    std::string out;
    out.push_back('"');
    usize backslashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() {
#if defined(_WIN32)
    if (process_handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
    }
#else
    if (process_handle_ != nullptr) {
        // Nothing to close on POSIX; the child is reaped by wait().
        process_handle_ = nullptr;
    }
#endif
    if (read_handle_ != nullptr) {
#if defined(_WIN32)
        ::CloseHandle(static_cast<HANDLE>(read_handle_));
#else
        ::close(*static_cast<int*>(read_handle_));
        delete static_cast<int*>(read_handle_);
#endif
        read_handle_ = nullptr;
    }
    if (write_handle_ != nullptr) {
#if defined(_WIN32)
        ::CloseHandle(static_cast<HANDLE>(write_handle_));
#else
        ::close(*static_cast<int*>(write_handle_));
        delete static_cast<int*>(write_handle_);
#endif
        write_handle_ = nullptr;
    }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_(other.pid_),
      process_handle_(other.process_handle_),
      read_handle_(other.read_handle_),
      write_handle_(other.write_handle_),
      exited_(other.exited_),
      exit_code_(other.exit_code_) {
    other.pid_ = 0;
    other.process_handle_ = nullptr;
    other.read_handle_ = nullptr;
    other.write_handle_ = nullptr;
    other.exited_ = false;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        this->~ChildProcess();
        new (this) ChildProcess(std::move(other));
    }
    return *this;
}

bool ChildProcess::valid() const noexcept { return process_handle_ != nullptr; }

bool ChildProcess::running() const { return valid() && !exited_; }

Result<std::string> ChildProcess::read_line(usize max_bytes) {
    if (read_handle_ == nullptr) {
        return Status(ErrorCode::LifecycleViolation, "child output is not captured");
    }
    std::string line;
    while (line.size() < max_bytes) {
        char c = 0;
#if defined(_WIN32)
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(read_handle_), &c, 1, &got, nullptr)) {
            return Status(ErrorCode::PersistenceError, "ReadFile on child pipe failed");
        }
        if (got == 0) {
            return line.empty() ? Result<std::string>(Status(ErrorCode::NotFound, "end of child output"))
                                : Result<std::string>(line);
        }
#else
        const ssize_t got = ::read(*static_cast<int*>(read_handle_), &c, 1);
        if (got < 0) {
            return Status(ErrorCode::PersistenceError, "read on child pipe failed");
        }
        if (got == 0) {
            return line.empty() ? Result<std::string>(Status(ErrorCode::NotFound, "end of child output"))
                                : Result<std::string>(line);
        }
#endif
        if (c == '\n') return line;
        if (c == '\r') continue;
        line.push_back(c);
    }
    return Status(ErrorCode::OversizedMessage, "child output line exceeds the bound");
}

Result<i32> ChildProcess::wait() {
    if (!valid()) return Status(ErrorCode::LifecycleViolation, "no child process");
    if (exited_) return exit_code_;
#if defined(_WIN32)
    const DWORD rc = ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
    if (rc != WAIT_OBJECT_0) {
        return Status(ErrorCode::PersistenceError, "WaitForSingleObject failed");
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code)) {
        return Status(ErrorCode::PersistenceError, "GetExitCodeProcess failed");
    }
    exited_ = true;
    exit_code_ = static_cast<i32>(code);
    return exit_code_;
#else
    int status = 0;
    if (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
        return Status(ErrorCode::PersistenceError, "waitpid failed");
    }
    exited_ = true;
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return exit_code_;
#endif
}

Status ChildProcess::terminate() {
    if (!valid()) return Status(ErrorCode::LifecycleViolation, "no child process");
    if (exited_) return Status::success();
#if defined(_WIN32)
    if (!::TerminateProcess(static_cast<HANDLE>(process_handle_), 137)) {
        return Status(ErrorCode::PersistenceError, "TerminateProcess failed");
    }
    ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), INFINITE);
    exited_ = true;
    return Status::success();
#else
    if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0) {
        return Status(ErrorCode::PersistenceError, "kill failed");
    }
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    exited_ = true;
    return Status::success();
#endif
}

Result<ChildProcess> spawn_process(const ProcessOptions& options) {
    if (options.executable.empty()) {
        return Status(ErrorCode::InvalidArgument, "executable path is empty");
    }
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (options.capture_stdout) {
        if (!::CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0)) {
            return Status(ErrorCode::PersistenceError, "CreatePipe failed");
        }
        ::SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (options.capture_stdout) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = child_stdout_write;
        startup.hStdError = options.merge_stderr ? child_stdout_write : ::GetStdHandle(STD_ERROR_HANDLE);
        startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION info{};
    std::wstring command = widen(options.executable);
    for (const auto& argument : options.arguments) {
        command.push_back(L' ');
        command.append(widen(quote_argument(argument)));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const std::wstring working = widen(options.working_directory);

    const BOOL ok = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                     nullptr, working.empty() ? nullptr : working.c_str(), &startup,
                                     &info);
    if (child_stdout_write != nullptr) ::CloseHandle(child_stdout_write);
    if (!ok) {
        if (child_stdout_read != nullptr) ::CloseHandle(child_stdout_read);
        return Status(ErrorCode::NotFound, "CreateProcess failed for the requested executable");
    }
    ::CloseHandle(info.hThread);

    ChildProcess child;
    child.pid_ = static_cast<u64>(info.dwProcessId);
    child.process_handle_ = info.hProcess;
    child.read_handle_ = child_stdout_read;
    return child;
#else
    int fds[2] = {-1, -1};
    if (options.capture_stdout && ::pipe(fds) != 0) {
        return Status(ErrorCode::PersistenceError, "pipe failed");
    }
    std::vector<std::string> storage;
    storage.reserve(options.arguments.size() + 1);
    storage.push_back(options.executable);
    for (const auto& argument : options.arguments) storage.push_back(argument);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (options.capture_stdout) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        if (options.merge_stderr) posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    }
    if (!options.working_directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, options.working_directory.c_str());
    }
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, storage.front().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (options.capture_stdout) ::close(fds[1]);
    if (rc != 0) {
        if (options.capture_stdout) ::close(fds[0]);
        return Status(ErrorCode::NotFound, "posix_spawn failed for the requested executable");
    }
    ChildProcess child;
    child.pid_ = static_cast<u64>(pid);
    child.process_handle_ = reinterpret_cast<void*>(1);
    if (options.capture_stdout) {
        child.read_handle_ = new int(fds[0]);
    }
    return child;
#endif
}

Result<std::string> executable_path() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(4096);
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return Status(ErrorCode::NotFound, "GetModuleFileName failed");
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length),
                                             nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<usize>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(length), out.data(), needed,
                          nullptr, nullptr);
    return out;
#else
    std::vector<char> buffer(4096);
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return Status(ErrorCode::NotFound, "readlink(/proc/self/exe) failed");
    return std::string(buffer.data(), static_cast<usize>(length));
#endif
}

Result<std::string> executable_directory() {
    auto path = executable_path();
    if (!path.ok()) return path.status();
    std::string value = std::move(path).value();
    const usize slash = value.find_last_of("/\\");
    if (slash == std::string::npos) return std::string(".");
    return value.substr(0, slash);
}

}  // namespace buffer_fabric
