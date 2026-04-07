#pragma once

#include <filesystem>
#include <string>

namespace fjell {

class Project; // defined in core/project.hpp

namespace base_path {

/// Returns the directory containing the running executable.
[[nodiscard]] const std::filesystem::path& dir();

/// Resolves a relative path against the executable directory.
[[nodiscard]] std::string resolve(const std::string& relative);

/// Resolves an asset path: uses project root if available, otherwise engine root.
[[nodiscard]] std::string resolve_asset(const std::string& relative,
                                        const Project* project);

} // namespace base_path
} // namespace fjell
