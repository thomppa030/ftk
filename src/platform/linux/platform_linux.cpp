#include "platform/platform.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
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

} // namespace fjell::platform
