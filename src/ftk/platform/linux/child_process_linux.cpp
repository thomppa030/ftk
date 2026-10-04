#include "ftk/platform/child_process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <mutex>
#include <system_error>
#include <vector>

namespace ftk::platform {

struct ChildProcess::Impl {
    pid_t pid{-1};
    int input{-1};
    int output{-1};
    int errors{-1};
    // Reaping and killing, which must never meet: a pid reaped may be
    // another process's by the time it is signalled.
    std::mutex lock;
    bool reaped{false};
    int code{-1};
};

namespace {

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

std::string reason(int error) {
    return std::error_code(error, std::generic_category()).message();
}

// How a status from waitpid() ended the child: its exit code, or -1 when a
// signal ended it.
int code_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

Result<std::size_t> read_from(int fd, std::span<char> into) {
    if (fd < 0) return std::size_t{0};
    for (;;) {
        const ssize_t got = ::read(fd, into.data(), into.size());
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) return make_error(std::format("can't read from the child: {}", reason(errno)));
    }
}

// A pipe both of whose ends close as a program is run, so no child keeps
// another's.
bool make_pipe(int (&ends)[2]) {
    return ::pipe2(ends, O_CLOEXEC) == 0;
}

} // namespace

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<ChildProcess>> ChildProcess::start(std::span<const std::string> args,
                                                           const ChildOptions& options) {
    if (args.empty()) return make_error("a child needs a program to run");

    // Everything the child needs is made before it is split off: from then
    // on, a child of a program with threads may only make the calls that
    // are safe in a signal handler.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const std::string folder = options.folder.string();

    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    int errors[2] = {-1, -1};
    // Carries the child's errno when it can't start the program; closes
    // unwritten when it can.
    int report[2] = {-1, -1};
    int nowhere = -1;
    const auto close_all = [&] {
        for (int* ends : {input, output, errors, report}) {
            close_fd(ends[0]);
            close_fd(ends[1]);
        }
        close_fd(nowhere);
    };
    bool made = make_pipe(input) && make_pipe(output) && make_pipe(report);
    if (made && options.errors == ChildErrors::Kept) made = make_pipe(errors);
    if (made && options.errors == ChildErrors::Discarded) {
        nowhere = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        made = nowhere >= 0;
    }
    if (!made) {
        const int error = errno;
        close_all();
        return make_error(std::format("can't make pipes for {}: {}", args[0], reason(error)));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int error = errno;
        close_all();
        return make_error(std::format("can't start {}: {}", args[0], reason(error)));
    }
    if (pid == 0) {
        // The child: its ends of the pipes as its standard streams. dup2
        // leaves the copies open across the exec; the originals close.
        ::dup2(input[0], STDIN_FILENO);
        ::dup2(output[1], STDOUT_FILENO);
        if (options.errors == ChildErrors::Kept) ::dup2(errors[1], STDERR_FILENO);
        if (options.errors == ChildErrors::Discarded) ::dup2(nowhere, STDERR_FILENO);
        if (folder.empty() || ::chdir(folder.c_str()) == 0) ::execvp(argv[0], const_cast<char* const*>(argv.data()));
        const int error = errno;
        (void)!::write(report[1], &error, sizeof error);
        ::_exit(127);
    }

    // The parent keeps its own ends.
    close_fd(input[0]);
    close_fd(output[1]);
    close_fd(errors[1]);
    close_fd(nowhere);
    close_fd(report[1]);
    int failure = 0;
    ssize_t got = 0;
    do {
        got = ::read(report[0], &failure, sizeof failure);
    } while (got < 0 && errno == EINTR);
    close_fd(report[0]);
    if (got == static_cast<ssize_t>(sizeof failure)) {
        ::waitpid(pid, nullptr, 0);
        close_all();
        return make_error(std::format("can't run {}: {}", args[0], reason(failure)));
    }

    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->input = input[1];
    impl->output = output[0];
    impl->errors = errors[0];
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::~ChildProcess() {
    kill();
    (void)wait();
    close_fd(impl_->input);
    close_fd(impl_->output);
    close_fd(impl_->errors);
}

Result<> ChildProcess::write(std::string_view bytes) {
    if (impl_->input < 0) return make_error("the child's input is closed");
    // Writing to a pipe no one reads raises SIGPIPE, which would end this
    // program: it is held back on this thread while writing, and taken
    // away if the write raised it.
    sigset_t pipe_signal;
    sigset_t before;
    ::sigemptyset(&pipe_signal);
    ::sigaddset(&pipe_signal, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &pipe_signal, &before);
    sigset_t pending;
    ::sigpending(&pending);
    const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;

    Result<> result{};
    while (!bytes.empty()) {
        const ssize_t wrote = ::write(impl_->input, bytes.data(), bytes.size());
        if (wrote < 0) {
            if (errno == EINTR) continue;
            result = make_error(std::format("can't write to the child: {}", reason(errno)));
            break;
        }
        bytes.remove_prefix(static_cast<std::size_t>(wrote));
    }

    if (!already_pending) {
        ::sigpending(&pending);
        if (::sigismember(&pending, SIGPIPE) == 1) {
            const timespec now{};
            ::sigtimedwait(&pipe_signal, nullptr, &now);
        }
    }
    ::pthread_sigmask(SIG_SETMASK, &before, nullptr);
    return result;
}

void ChildProcess::close_input() {
    close_fd(impl_->input);
}

Result<std::size_t> ChildProcess::read(std::span<char> into) {
    return read_from(impl_->output, into);
}

Result<std::size_t> ChildProcess::read_errors(std::span<char> into) {
    return read_from(impl_->errors, into);
}

std::optional<int> ChildProcess::exit_code() {
    const std::lock_guard held(impl_->lock);
    if (!impl_->reaped) {
        int status = 0;
        if (::waitpid(impl_->pid, &status, WNOHANG) != impl_->pid) return std::nullopt;
        impl_->reaped = true;
        impl_->code = code_of(status);
    }
    return impl_->code;
}

int ChildProcess::wait() {
    // Waited for without being reaped, so its pid stays its own, and
    // kill() safe, until the reaping below.
    siginfo_t info{};
    while (::waitid(P_PID, static_cast<id_t>(impl_->pid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR) {
    }
    const std::lock_guard held(impl_->lock);
    if (!impl_->reaped) {
        int status = 0;
        while (::waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
        }
        impl_->reaped = true;
        impl_->code = code_of(status);
    }
    return impl_->code;
}

void ChildProcess::kill() {
    const std::lock_guard held(impl_->lock);
    if (!impl_->reaped) ::kill(impl_->pid, SIGKILL);
}

} // namespace ftk::platform
