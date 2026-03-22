#include "platform/platform.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace fjell::platform {

std::filesystem::path executable_path() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        throw std::runtime_error("Failed to get executable path via GetModuleFileNameW");
    }
    return std::filesystem::path(buf.data());
}

int run_command(const std::string& cmd, std::string& output) {
    output.clear();
    // _popen is the MSVC equivalent of POSIX popen
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return -1;

    std::array<char, 256> line{};
    while (fgets(line.data(), static_cast<int>(line.size()), pipe)) {
        output += line.data();
    }

    int status = _pclose(pipe);
    return status;
}

Result<> spawn_detached(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_detached: empty args");

    // Build command line string: quote each argument
    std::string cmd_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd_line += ' ';
        // Quote arguments that contain spaces
        bool needs_quotes = args[i].find(' ') != std::string::npos;
        if (needs_quotes) cmd_line += '"';
        cmd_line += args[i];
        if (needs_quotes) cmd_line += '"';
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_NEW_PROCESS_GROUP detaches the child from our console
    BOOL ok = CreateProcessA(
        nullptr,
        cmd_line.data(), // must be mutable for CreateProcessA
        nullptr, nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) return make_error("spawn_detached: CreateProcessA failed");

    // Close handles — we don't wait on the child
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {};
}

} // namespace fjell::platform
