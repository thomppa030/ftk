#include "platform/platform.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fjell::platform {

std::filesystem::path executable_path() {
    std::array<char, 4096> buf{};
    auto len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len <= 0) {
        throw std::runtime_error("Failed to read /proc/self/exe");
    }
    buf[static_cast<std::size_t>(len)] = '\0';
    return std::filesystem::path(buf.data());
}

int run_command(const std::string& cmd, std::string& output) {
    output.clear();
    auto* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 256> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = pclose(pipe);
    // pclose returns the exit status in the format of wait(2)
    return WEXITSTATUS(status);
}

Result<> spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_detached: empty args");

    pid_t pid = fork();
    if (pid < 0) return make_error("spawn_detached: fork failed");

    if (pid == 0) {
        // First child: fork again so the grandchild is orphaned and
        // reparented to init — no zombie possible.
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(1);
        if (pid2 > 0) _exit(0); // first child exits immediately

        // Grandchild: start a new session and exec
        setsid();

        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execv(argv[0], const_cast<char* const*>(argv.data()));
        _exit(1);
    }

    // Parent: reap the first child (exits immediately after double-fork)
    waitpid(pid, nullptr, 0);
    return {};
}

Result<> exec_replace(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("exec_replace: empty args");

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    execv(argv[0], const_cast<char* const*>(argv.data()));
    // execv only returns on failure
    return make_error("exec_replace: execv failed");
}

Result<> open_path(const std::filesystem::path& path) {
    return spawn_detached({"xdg-open", path.string()});
}

// ── Subprocess with stdout pipe ─────────────────────────────────────────

Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_with_pipe: empty args");

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return make_error("spawn_with_pipe: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return make_error("spawn_with_pipe: fork() failed");
    }

    if (pid == 0) {
        // Child: redirect stdout to pipe write end
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execv(argv[0], const_cast<char* const*>(argv.data()));
        _exit(1);
    }

    // Parent: close write end, set read end to non-blocking
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    SubprocessHandle handle;
    handle.pid = pid;
    handle.stdout_fd = pipefd[0];
    return handle;
}

std::vector<std::string> read_lines(SubprocessHandle& handle, std::string& line_buffer) {
    std::vector<std::string> lines;
    if (handle.stdout_fd < 0) return lines;

    char buf[4096];
    for (;;) {
        auto n = read(handle.stdout_fd, buf, sizeof(buf));
        if (n <= 0) break;
        line_buffer.append(buf, static_cast<size_t>(n));
    }

    // Extract complete lines
    size_t pos = 0;
    while (true) {
        auto nl = line_buffer.find('\n', pos);
        if (nl == std::string::npos) break;
        lines.push_back(line_buffer.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (pos > 0) {
        line_buffer.erase(0, pos);
    }

    return lines;
}

bool poll_exit(SubprocessHandle& handle) {
    if (handle.finished || handle.pid <= 0) return handle.finished;

    int status = 0;
    pid_t result = waitpid(handle.pid, &status, WNOHANG);
    if (result == handle.pid) {
        handle.finished = true;
        handle.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return handle.finished;
}

void close_subprocess(SubprocessHandle& handle) {
    if (handle.pid > 0 && !handle.finished) {
        kill(handle.pid, SIGKILL);
        waitpid(handle.pid, nullptr, 0);
        handle.finished = true;
        handle.exit_code = -1;
    }
    if (handle.stdout_fd >= 0) {
        close(handle.stdout_fd);
        handle.stdout_fd = -1;
    }
    handle.pid = -1;
}

} // namespace fjell::platform
