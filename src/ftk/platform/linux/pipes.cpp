#include "ftk/platform/linux/pipes.hpp"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <format>
#include <system_error>

namespace ftk::platform::detail {

namespace {

std::string reason(int error) {
    return std::error_code(error, std::generic_category()).message();
}

} // namespace

Result<std::size_t> read_some(int fd, std::span<char> into, std::string_view what) {
    if (fd < 0) return std::size_t{0};
    for (;;) {
        const ssize_t got = ::read(fd, into.data(), into.size());
        if (got >= 0) return static_cast<std::size_t>(got);
        if (errno != EINTR) return make_error(std::format("can't read from {}: {}", what, reason(errno)));
    }
}

Result<> write_all(int fd, std::string_view bytes, std::string_view what) {
    if (fd < 0) return make_error(std::format("{} is closed", what));
    // SIGPIPE is held back on this thread while writing, and taken away if
    // the write raised it.
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
        const ssize_t wrote = ::write(fd, bytes.data(), bytes.size());
        if (wrote < 0) {
            if (errno == EINTR) continue;
            result = make_error(std::format("can't write to {}: {}", what, reason(errno)));
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

} // namespace ftk::platform::detail
