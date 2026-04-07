#pragma once

#include "core/result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fjell::platform {

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

} // namespace fjell::platform
