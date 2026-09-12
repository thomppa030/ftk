#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell::platform {

/// Filename extension for executables, for composing sibling tool paths.
#ifdef _WIN32
inline constexpr const char* EXE_EXT = ".exe";
#else
inline constexpr const char* EXE_EXT = "";
#endif

/// Returns the absolute path of the running executable.
[[nodiscard]] std::filesystem::path executable_path();

/// Runs a shell command, captures stdout+stderr. Returns exit code.
[[nodiscard]] int run_command(const std::string& cmd, std::string& output);

/// Spawn a detached child process. The child runs independently and is
/// not waited on. args[0] is the executable path.
Result<> spawn_detached(const std::vector<std::string>& args);

/// Replace the current process with a new executable.
/// args[0] is the executable path. Does not return on success.
Result<> exec_replace(const std::vector<std::string>& args);

/// Open a file or directory with the system default handler.
/// (xdg-open on Linux, ShellExecuteA on Windows)
Result<> open_path(const std::filesystem::path& path);

// ── Subprocess with stdout pipe ─────────────────────────────────────────

/// Handle for a child process whose stdout is captured via a pipe.
///
/// `process` and `stdout_read` are opaque OS handles: a pid and a file
/// descriptor on POSIX, a HANDLE and a pipe HANDLE on Windows. Only the
/// platform backend interprets them; callers pass the struct around and read
/// `finished`/`exit_code`. `INVALID` marks an unset handle on both.
struct SubprocessHandle {
    static constexpr std::uintptr_t INVALID = static_cast<std::uintptr_t>(-1);

    std::uintptr_t process{INVALID};
    std::uintptr_t stdout_read{INVALID};
    bool finished{false};
    int exit_code{-1};
};

/// Spawn a child process with its stdout connected to a readable pipe.
/// args[0] is the executable path. The pipe fd is set to non-blocking.
[[nodiscard]] Result<SubprocessHandle> spawn_with_pipe(const std::vector<std::string>& args);

/// Read available complete lines from the subprocess stdout (non-blocking).
/// Partial lines are buffered internally until a newline arrives.
[[nodiscard]] std::vector<std::string> read_lines(SubprocessHandle& handle, std::string& line_buffer);

/// Check if the subprocess has exited. Updates handle.finished and handle.exit_code.
bool poll_exit(SubprocessHandle& handle);

/// Kill the subprocess (SIGKILL) if still running, close the pipe fd, reap.
void close_subprocess(SubprocessHandle& handle);

} // namespace fjell::platform
