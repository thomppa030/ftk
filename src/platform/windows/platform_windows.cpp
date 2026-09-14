#include "platform/platform.hpp"

#ifndef _WIN32
#error "This file should only be compiled on Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <shellapi.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace fjell::platform {
namespace {

/// CreateProcess takes one mutable command line rather than an argv array, so
/// arguments are joined and any containing a space is quoted.
std::string join_args(const std::vector<std::string>& args) {
    std::string cmd_line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd_line += ' ';
        bool needs_quotes = args[i].find(' ') != std::string::npos;
        if (needs_quotes) cmd_line += '"';
        cmd_line += args[i];
        if (needs_quotes) cmd_line += '"';
    }
    return cmd_line;
}

} // namespace

std::filesystem::path executable_path() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        throw std::runtime_error("Failed to get executable path via GetModuleFileNameW");
    }
    return std::filesystem::path(buf.data());
}

std::uint32_t process_id() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
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

    auto cmd_line = join_args(args);

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

    auto cmd_line = join_args(args);

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

// ── Subprocess with stdout pipe ─────────────────────────────────────────

Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& args) {
    if (args.empty()) return make_error("spawn_with_pipe: empty args");

    // The child inherits the write end; the read end must not be inherited or
    // the pipe never reports EOF once the child exits.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return make_error("spawn_with_pipe: CreatePipe failed");
    }
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return make_error("spawn_with_pipe: SetHandleInformation failed");
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    auto command_line = join_args(args);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr,
                             /*bInheritHandles=*/TRUE, 0, nullptr, nullptr, &si, &pi);

    // The parent has no use for the write end; releasing it is what lets the
    // read end see EOF after the child is gone.
    CloseHandle(write_end);

    if (!ok) {
        CloseHandle(read_end);
        return make_error("spawn_with_pipe: CreateProcessA failed");
    }
    CloseHandle(pi.hThread);

    SubprocessHandle handle;
    handle.process = reinterpret_cast<std::uintptr_t>(pi.hProcess);
    handle.stdout_read = reinterpret_cast<std::uintptr_t>(read_end);
    return handle;
}

std::vector<std::string> read_lines(SubprocessHandle& handle, std::string& line_buffer) {
    std::vector<std::string> lines;
    if (handle.stdout_read == SubprocessHandle::INVALID) return lines;

    auto pipe = reinterpret_cast<HANDLE>(handle.stdout_read);

    // Anonymous pipes have no non-blocking mode, so peek for buffered bytes and
    // only read what is already there — ReadFile would otherwise block until the
    // child writes or exits.
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) break;

        std::array<char, 4096> buf{};
        DWORD want = available < buf.size() ? available : static_cast<DWORD>(buf.size());
        DWORD got = 0;
        if (!ReadFile(pipe, buf.data(), want, &got, nullptr) || got == 0) break;
        line_buffer.append(buf.data(), got);
    }

    size_t pos = 0;
    for (;;) {
        auto nl = line_buffer.find('\n', pos);
        if (nl == std::string::npos) break;
        auto end = nl;
        // fjimport writes CRLF through the console runtime.
        if (end > pos && line_buffer[end - 1] == '\r') --end;
        lines.push_back(line_buffer.substr(pos, end - pos));
        pos = nl + 1;
    }
    if (pos > 0) {
        line_buffer.erase(0, pos);
    }

    return lines;
}

bool poll_exit(SubprocessHandle& handle) {
    if (handle.finished || handle.process == SubprocessHandle::INVALID) return handle.finished;

    auto process = reinterpret_cast<HANDLE>(handle.process);
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return false;

    DWORD code = 0;
    handle.exit_code = GetExitCodeProcess(process, &code) ? static_cast<int>(code) : -1;
    handle.finished = true;
    return true;
}

void close_subprocess(SubprocessHandle& handle) {
    if (handle.process != SubprocessHandle::INVALID) {
        auto process = reinterpret_cast<HANDLE>(handle.process);
        if (!handle.finished) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, INFINITE);
            handle.finished = true;
            handle.exit_code = -1;
        }
        CloseHandle(process);
        handle.process = SubprocessHandle::INVALID;
    }
    if (handle.stdout_read != SubprocessHandle::INVALID) {
        CloseHandle(reinterpret_cast<HANDLE>(handle.stdout_read));
        handle.stdout_read = SubprocessHandle::INVALID;
    }
}

} // namespace fjell::platform
