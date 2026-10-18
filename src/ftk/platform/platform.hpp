#pragma once

#include "ftk/base/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ftk::platform {

/// Filename extension for executables, for composing sibling tool paths.
#ifdef _WIN32
inline constexpr const char* EXE_EXT = ".exe";
#else
inline constexpr const char* EXE_EXT = "";
#endif

/// Returns the absolute path of the running executable.
[[nodiscard]] std::filesystem::path executable_path();

/// Returns the OS process id of the running process. Unique among
/// concurrently running processes, so it can key per-process scratch names.
[[nodiscard]] std::uint32_t process_id();

/// Where programs keep this user's settings on this machine, a folder of
/// their own inside it: $XDG_CONFIG_HOME, else ~/.config, on Linux;
/// %APPDATA% on Windows. Not made here.
[[nodiscard]] std::filesystem::path config_dir();

/// Where programs keep what they must not lose between runs that is no
/// setting, such as unsaved work kept against a crash: $XDG_STATE_HOME,
/// else ~/.local/state, on Linux; %LOCALAPPDATA% on Windows. Not made here.
[[nodiscard]] std::filesystem::path state_dir();

/// Where programs keep what they can make again but would rather not, such
/// as compiled shaders, a folder of their own inside it: $XDG_CACHE_HOME,
/// else ~/.cache, on Linux; %LOCALAPPDATA% on Windows, where a program
/// keeps it apart from its state. Not made here.
[[nodiscard]] std::filesystem::path cache_dir();

/// The file the program a command names is, found as starting the command
/// would find it: the path itself when `program` names a folder, else the
/// first match along the PATH. On Windows, also where Windows looks before
/// the PATH, with `.exe` added to a name without an extension. Nothing when
/// there is none.
[[nodiscard]] std::optional<std::filesystem::path> find_program(std::string_view program);

/// Runs a shell command, captures stdout+stderr. Returns exit code.
[[nodiscard]] int run_command(const std::string& cmd, std::string& output);

/// Spawn a detached child process. The child runs independently and is
/// not waited on. args[0] is the executable path.
Result<> spawn_detached(const std::vector<std::string>& args);

/// Replace the current process with a new executable.
/// args[0] is the executable path. Does not return on success.
Result<> exec_replace(const std::vector<std::string>& args);

/// Waits for what comes on this program's standard input and reads it into
/// `into`, as bytes, whatever the system would make of them as text: for a
/// program talked to through its standard streams, as a language server
/// is. @return how many bytes, none once the input has ended, or why it
///         couldn't be read
[[nodiscard]] Result<std::size_t> read_standard_input(std::span<char> into);

/// Writes all of `bytes` to this program's standard output as they are,
/// past any buffering. @return nothing, or an error once no one reads it
[[nodiscard]] Result<> write_standard_output(std::string_view bytes);

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

} // namespace ftk::platform
