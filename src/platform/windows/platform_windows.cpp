#include "platform/platform.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <shellapi.h>

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

Result<> exec_replace(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("exec_replace: empty args");

    // Build command line string: quote each argument
    std::string cmd_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd_line += ' ';
        bool needs_quotes = args[i].find(' ') != std::string::npos;
        if (needs_quotes) cmd_line += '"';
        cmd_line += args[i];
        if (needs_quotes) cmd_line += '"';
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr, nullptr,
        FALSE,
        0,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) return make_error("exec_replace: CreateProcessA failed");

    // Wait for the child, then exit this process with its exit code
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    ExitProcess(exit_code);
}

Result<> open_path(const std::filesystem::path& path) {
    auto result = ShellExecuteA(nullptr, "open", path.string().c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(result) <= 32) {
        return make_error("open_path: ShellExecuteA failed");
    }
    return {};
}

// ── Subprocess with stdout pipe (stubs — not yet implemented) ───────────

Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& /*args*/) {
    return make_error("spawn_with_pipe: not implemented on Windows yet");
}

std::vector<std::string> read_lines(SubprocessHandle& /*handle*/, std::string& /*line_buffer*/) {
    return {};
}

bool poll_exit(SubprocessHandle& /*handle*/) {
    return true;
}

void close_subprocess(SubprocessHandle& /*handle*/) {}

} // namespace fjell::platform
