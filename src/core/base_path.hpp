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
///
/// A reference may be either a project-relative path or an asset id (32 hex
/// characters). Ids are looked up in the project's asset database, so a
/// reference stored as an id keeps working after the asset is moved. An id that
/// no longer resolves comes back empty — a dangling reference is an error to
/// report, not a path to guess at.
[[nodiscard]] std::string resolve_asset(const std::string& reference,
                                        const Project* project);

/// True if a reference is an asset id rather than a path.
[[nodiscard]] bool is_asset_id(const std::string& reference);

/// The reference to serialize for an asset. Returns the asset's id when the
/// project tracks it, so the reference survives the asset being moved; falls
/// back to the path it was given for anything untracked (engine assets, files
/// outside the project, a project that has not been opened).
[[nodiscard]] std::string asset_reference(const std::string& path_or_id,
                                          const Project* project);

} // namespace base_path
} // namespace fjell
