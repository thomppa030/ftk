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

bool spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // Child: build argv for execv
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execv(argv[0], const_cast<char* const*>(argv.data()));
        _exit(1); // execv only returns on failure
    }

    // Parent: don't wait — child runs independently.
    // Reap to avoid zombie (double-fork would be cleaner but SIGCHLD=SIG_IGN works on Linux)
    return true;
}

} // namespace fjell::platform
