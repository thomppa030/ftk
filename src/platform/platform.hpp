#pragma once

#include <filesystem>
#include <string>

namespace fjell::platform {

/// Returns the absolute path of the running executable.
[[nodiscard]] std::filesystem::path executable_path();

/// Runs a shell command, captures stdout+stderr. Returns exit code.
[[nodiscard]] int run_command(const std::string& cmd, std::string& output);

} // namespace fjell::platform
