#include "platform/platform.hpp"

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

} // namespace fjell::platform
