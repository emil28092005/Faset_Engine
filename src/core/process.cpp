#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <faset/core/process.hpp>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace faset {
namespace {
#ifdef _WIN32
std::wstring widen(const std::string& value) {
    if (value.empty())
        return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (!length)
        throw std::runtime_error("Invalid UTF-8 process argument");
    std::wstring result(length, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes{};
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result += c;
        } else {
            result.append(slashes, L'\\');
            result += c;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}
struct CaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};
#else
std::filesystem::path resolve_program(const std::string& name, const std::string& path,
                                      const std::filesystem::path& cwd) {
    if (name.find('/') != std::string::npos) {
        auto file = std::filesystem::path(name);
        if (file.is_relative())
            file = cwd / file;
        if (::access(file.c_str(), X_OK) == 0 && !std::filesystem::is_directory(file))
            return std::filesystem::absolute(file);
        throw std::runtime_error("Executable is missing or not executable: " + name);
    }
    std::size_t from{};
    do {
        auto end = path.find(':', from);
        auto directory = path.substr(from, end == std::string::npos ? end : end - from);
        auto file = (directory.empty() ? cwd : std::filesystem::path(directory)) / name;
        if (file.is_relative())
            file = cwd / file;
        if (::access(file.c_str(), X_OK) == 0 && !std::filesystem::is_directory(file))
            return std::filesystem::absolute(file);
        if (end == std::string::npos)
            break;
        from = end + 1;
    } while (true);
    throw std::runtime_error("Executable not found on PATH: " + name);
}
#endif
} // namespace
std::filesystem::path find_executable(const std::string& name) {
#ifdef _WIN32
    auto wide = widen(name);
    std::vector<wchar_t> buffer(32768);
    DWORD length = SearchPathW(nullptr, wide.c_str(), L".exe", static_cast<DWORD>(buffer.size()),
                               buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size())
        throw std::runtime_error("Executable not found on PATH: " + name);
    return std::filesystem::path(std::wstring(buffer.data(), length));
#else
    const char* path = std::getenv("PATH");
    return resolve_program(name, path ? path : "", std::filesystem::current_path());
#endif
}
void launch_detached(const std::vector<std::string>& arguments,
                     const std::filesystem::path& working_directory) {
    if (arguments.empty() || arguments.front().empty())
        throw std::invalid_argument("External application requires an executable");
    for (const auto& argument : arguments)
        if (argument.find('\0') != std::string::npos)
            throw std::invalid_argument("NUL in external application argument");
    const auto cwd = working_directory.empty() ? std::filesystem::current_path()
                                               : std::filesystem::absolute(working_directory);
    if (!std::filesystem::is_directory(cwd))
        throw std::runtime_error("External application working directory does not exist");
#ifdef _WIN32
    auto program = std::filesystem::path(widen(arguments.front()));
    if (program.has_parent_path() && program.is_relative())
        program = cwd / program;
    const auto executable =
        program.has_parent_path() ? program : find_executable(arguments.front());
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty())
            command += L' ';
        command += quote(widen(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, cwd.c_str(), &startup,
                        &process))
        throw std::runtime_error("Cannot launch external application: " + arguments.front());
    // Deliberately no kill-on-close job: the user's editor must outlive this Editor session.
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const char* path = std::getenv("PATH");
    const auto executable = resolve_program(arguments.front(), path ? path : "", cwd);
    std::vector<char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    int errors[2];
    if (::pipe2(errors, O_CLOEXEC) < 0)
        throw std::runtime_error("Cannot create external application status pipe");
    // A host may have closed a standard stream. Keep the status pipe out of dup2's targets.
    for (auto& descriptor : errors)
        if (descriptor <= STDERR_FILENO) {
            const auto replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (replacement < 0) {
                ::close(errors[0]);
                ::close(errors[1]);
                throw std::runtime_error("Cannot configure external application status pipe");
            }
            ::close(descriptor);
            descriptor = replacement;
        }
    const auto child = ::fork();
    if (child == 0) {
        // After fork in the multi-threaded Editor, only async-signal-safe calls are allowed.
        ::close(errors[0]);
        auto fail = [&](int error) {
            while (::write(errors[1], &error, sizeof(error)) < 0 && errno == EINTR) {
            }
            ::_exit(127);
        };
        if (::setsid() < 0)
            fail(errno);
        const auto grandchild = ::fork();
        if (grandchild < 0)
            fail(errno);
        if (grandchild > 0)
            ::_exit(0);
        const auto input = ::open("/dev/null", O_RDWR);
        if (input < 0)
            fail(errno);
        if (::dup2(input, STDIN_FILENO) < 0 || ::dup2(input, STDOUT_FILENO) < 0 ||
            ::dup2(input, STDERR_FILENO) < 0 || ::chdir(cwd.c_str()) < 0)
            fail(errno);
        if (input > STDERR_FILENO)
            ::close(input);
        ::execve(executable.c_str(), argv.data(), environ);
        fail(errno);
    }
    ::close(errors[1]);
    if (child < 0) {
        ::close(errors[0]);
        throw std::runtime_error("Cannot fork external application");
    }
    int status{};
    pid_t reaped;
    do {
        reaped = ::waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    int error{};
    ssize_t count;
    do {
        count = ::read(errors[0], &error, sizeof(error));
    } while (count < 0 && errno == EINTR);
    ::close(errors[0]);
    if (count != 0 || reaped < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("Cannot launch external application: " + arguments.front() +
                                 (count > 0 ? ": " + std::string(std::strerror(error)) : ""));
#endif
}
struct Process::Impl {
#ifdef _WIN32
    HANDLE process{}, thread{}, job{}, output{};
#else
    pid_t pid{-1};
    int output{-1};
#endif
    bool running{};
    std::optional<int> exit_code;
    ~Impl() {
        try {
            cancel();
        } catch (...) {
        }
#ifdef _WIN32
        if (output)
            CloseHandle(output);
        if (thread)
            CloseHandle(thread);
        if (process)
            CloseHandle(process);
        if (job)
            CloseHandle(job);
#else
        if (output >= 0)
            ::close(output);
#endif
    }
#ifndef _WIN32
    bool collect_exit() {
        // WNOWAIT keeps the exited leader's PID reserved until its owned group has
        // been stopped. Never signal a PGID after reaping the leader: it may be reused.
        siginfo_t information{};
        int result;
        do {
            result =
                ::waitid(P_PID, static_cast<id_t>(pid), &information, WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            if (errno == ECHILD) {
                running = false;
                pid = -1;
            }
            throw std::runtime_error("Cannot collect child process");
        }
        if (information.si_pid == 0)
            return false;
        ::kill(-pid, SIGKILL);
        int status{};
        pid_t reaped;
        do {
            reaped = ::waitpid(pid, &status, 0);
        } while (reaped < 0 && errno == EINTR);
        running = false;
        pid = -1;
        if (reaped < 0)
            throw std::runtime_error("Cannot reap child process");
        exit_code = WIFEXITED(status)     ? WEXITSTATUS(status)
                    : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                          : 1;
        return true;
    }
#endif
    ProcessPoll poll() {
        std::string text;
        char buffer[8192];
#ifdef _WIN32
        if (output) {
            DWORD available{};
            while (PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available) {
                DWORD read{};
                if (!ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &read,
                              nullptr) ||
                    !read)
                    break;
                text.append(buffer, read);
            }
        }
        if (running && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            DWORD code{};
            if (!GetExitCodeProcess(process, &code))
                throw std::runtime_error("Cannot read process exit status");
            running = false;
            exit_code = static_cast<int>(code);
        }
#else
        if (output >= 0) {
            while (true) {
                auto count = ::read(output, buffer, sizeof(buffer));
                if (count > 0) {
                    text.append(buffer, static_cast<std::size_t>(count));
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                if (count == 0) {
                    ::close(output);
                    output = -1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK)
                    throw std::runtime_error("Cannot read child output");
                break;
            }
        }
        if (running)
            collect_exit();
#endif
        if (!running) {
#ifdef _WIN32
            DWORD available{};
            while (output && PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) &&
                   available) {
                DWORD read{};
                if (!ReadFile(output, buffer, std::min<DWORD>(available, sizeof(buffer)), &read,
                              nullptr) ||
                    !read)
                    break;
                text.append(buffer, read);
            }
#else
            if (output >= 0)
                while (true) {
                    auto count = ::read(output, buffer, sizeof(buffer));
                    if (count > 0) {
                        text.append(buffer, static_cast<std::size_t>(count));
                        continue;
                    }
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count == 0) {
                        ::close(output);
                        output = -1;
                    }
                    break;
                }
#endif
        }
        return {running, exit_code, std::move(text)};
    }
    void cancel() {
        if (!running)
            return;
#ifdef _WIN32
        if (job)
            TerminateJobObject(job, 130);
        TerminateProcess(process, 130);
        WaitForSingleObject(process, INFINITE);
        DWORD code{};
        GetExitCodeProcess(process, &code);
        exit_code = static_cast<int>(code);
        running = false;
#else
        ::kill(-pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            if (collect_exit())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ::kill(-pid, SIGKILL);
        int status{};
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        running = false;
        pid = -1;
        exit_code = 130;
#endif
    }
};
Process::Process(const ProcessOptions& options) : impl_(std::make_unique<Impl>()) {
    if (options.arguments.empty() || options.arguments[0].empty())
        throw std::invalid_argument("Process requires an executable");
    for (const auto& argument : options.arguments)
        if (argument.find('\0') != std::string::npos)
            throw std::invalid_argument("NUL in process argument");
    auto cwd = options.working_directory.empty()
                   ? std::filesystem::current_path()
                   : std::filesystem::absolute(options.working_directory);
    if (!std::filesystem::is_directory(cwd))
        throw std::runtime_error("Process working directory does not exist");
#ifdef _WIN32
    auto program = std::filesystem::path(widen(options.arguments[0]));
    if (program.has_parent_path() && program.is_relative())
        program = cwd / program;
    auto executable = program.has_parent_path() ? program : find_executable(options.arguments[0]);
    std::wstring command;
    for (const auto& argument : options.arguments) {
        if (!command.empty())
            command += L' ';
        command += quote(widen(argument));
    }
    std::map<std::wstring, std::wstring, CaseInsensitive> environment;
    auto block = GetEnvironmentStringsW();
    if (!block)
        throw std::runtime_error("Cannot read process environment");
    for (auto entry = block; *entry; entry += wcslen(entry) + 1) {
        std::wstring text(entry);
        auto equals = text.find(L'=', text[0] == L'=' ? 1 : 0);
        if (equals != std::wstring::npos)
            environment[text.substr(0, equals)] = text.substr(equals + 1);
    }
    FreeEnvironmentStringsW(block);
    for (auto& [key, value] : options.environment) {
        if (key.empty() || key.find('=') != std::string::npos ||
            key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid environment entry");
        environment[widen(key)] = widen(value);
    }
    std::vector<wchar_t> env;
    for (auto& [key, value] : environment) {
        auto item = key + L"=" + value;
        env.insert(env.end(), item.begin(), item.end());
        env.push_back(0);
    }
    env.push_back(0);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output_write{}, input{};
    if (!CreatePipe(&impl_->output, &output_write, &security, 0))
        throw std::runtime_error("Cannot create process pipe");
    SetHandleInformation(impl_->output, HANDLE_FLAG_INHERIT, 0);
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != 0;
    HANDLE handles[] = {output_write, input};
    bool updated =
        initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                 handles, sizeof(handles), nullptr, nullptr) != 0;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output_write;
    startup.StartupInfo.hStdInput = input;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    bool launched =
        updated && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                  CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                                      CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                                  env.data(), cwd.c_str(), &startup.StartupInfo, &process) != 0;
    if (initialized)
        DeleteProcThreadAttributeList(attributes);
    CloseHandle(output_write);
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (!launched)
        throw std::runtime_error("CreateProcess failed for " + options.arguments[0]);
    impl_->process = process.hProcess;
    impl_->thread = process.hThread;
    impl_->running = true;
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    if (!impl_->job)
        throw std::runtime_error("Cannot create compiler job object");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits)) ||
        !AssignProcessToJobObject(impl_->job, impl_->process))
        throw std::runtime_error("Cannot isolate compiler process group");
    if (ResumeThread(impl_->thread) == DWORD(-1))
        throw std::runtime_error("Cannot start compiler process");
#else
    std::map<std::string, std::string> environment;
    for (char** item = environ; *item; ++item) {
        std::string text(*item);
        auto separator = text.find('=');
        if (separator != std::string::npos)
            environment[text.substr(0, separator)] = text.substr(separator + 1);
    }
    for (const auto& [key, value] : options.environment) {
        if (key.empty() || key.find('=') != std::string::npos ||
            key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid environment entry");
        environment[key] = value;
    }
    auto executable = resolve_program(options.arguments[0], environment["PATH"], cwd);
    std::vector<std::string> env_storage;
    std::vector<char*> argv, envp;
    for (const auto& argument : options.arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    for (const auto& [key, value] : environment)
        env_storage.push_back(key + "=" + value);
    for (auto& entry : env_storage)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC) < 0)
        throw std::runtime_error("Cannot create process pipe");
    int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        ::close(pipes[0]);
        ::close(pipes[1]);
        throw std::runtime_error("Cannot open process input");
    }
    auto pid = ::fork();
    if (pid == 0) {
        ::setpgid(0, 0);
        ::close(pipes[0]);
        if (::dup2(input, STDIN_FILENO) < 0 || ::dup2(pipes[1], STDOUT_FILENO) < 0 ||
            ::dup2(pipes[1], STDERR_FILENO) < 0 || ::chdir(cwd.c_str()) < 0)
            ::_exit(126);
        ::close(input);
        ::close(pipes[1]);
        ::execve(executable.c_str(), argv.data(), envp.data());
        constexpr char message[] = "Cannot execute child process\n";
        ::write(STDERR_FILENO, message, sizeof(message) - 1);
        ::_exit(127);
    }
    ::close(input);
    ::close(pipes[1]);
    if (pid < 0) {
        ::close(pipes[0]);
        throw std::runtime_error("Cannot fork process");
    }
    ::setpgid(pid, pid);
    impl_->pid = pid;
    impl_->output = pipes[0];
    impl_->running = true;
    int flags = ::fcntl(pipes[0], F_GETFL, 0);
    if (flags < 0 || ::fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot configure process output pipe");
#endif
}
Process::~Process() = default;
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;
ProcessPoll Process::poll() {
    return impl_->poll();
}
void Process::cancel() {
    impl_->cancel();
}
} // namespace faset
