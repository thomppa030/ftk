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

} // namespace fjell::platform
