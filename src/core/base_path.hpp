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

/// Directory to put on a game project's include path so its scripts can
/// `#include <fjell/...>`. Points into the engine source tree for a dev build
/// and at the installed SDK headers otherwise.
[[nodiscard]] const std::filesystem::path& sdk_include();

/// Resolves an asset path: uses project root if available, otherwise engine root.
///
/// A reference may be either a project-relative path or an asset id (32 hex
/// characters). Ids are looked up in the project's asset database, so a
/// reference stored as an id keeps working after the asset is moved. An id that
/// no longer resolves comes back empty — a dangling reference is an error to
/// report, not a path to guess at.
[[nodiscard]] std::string resolve_asset(const std::string& reference,
                                        const Project* project);

/// Resolves a file that either tree may own, preferring the project's copy.
///
/// Some references name a file that could belong to the project or to the
/// engine and look identical either way — `shaders/standard.fjsl` ships with
/// the engine, `shaders/water.fjsl` is the game's, and the string says
/// nothing about which. resolve_asset() cannot decide: given a project it
/// always answers with the project's root, whether or not anything is there.
///
/// So try the project, take it if the file exists, and fall back to the
/// engine. A project that wants to replace an engine file does it by putting
/// one at the same relative path. Returns empty when neither tree has it, so
/// the caller reports a missing file rather than opening the wrong one.
[[nodiscard]] std::string resolve_shared(const std::string& reference,
                                         const Project* project);

/// True if a reference is an asset id rather than a path.
[[nodiscard]] bool is_asset_id(const std::string& reference);

/// True if a reference points at an asset of the given extension — either a
/// path ending in it, or an id (which carries no extension of its own).
/// Prefer this over checking the extension directly: a bare `ends_with` test
/// silently drops id references.
[[nodiscard]] bool references_asset(const std::string& reference,
                                    const std::string& extension);

/// The reference to serialize for an asset. Returns the asset's id when the
/// project tracks it, so the reference survives the asset being moved; falls
/// back to the path it was given for anything untracked (engine assets, files
/// outside the project, a project that has not been opened).
[[nodiscard]] std::string asset_reference(const std::string& path_or_id,
                                          const Project* project);

/// The project-relative path a reference points at, for display. An id becomes
/// the path it resolves to; a path is returned as-is. An id the database does
/// not know comes back empty, so callers can say so rather than showing a bare
/// uuid. Use resolve_asset to get an absolute path for loading.
[[nodiscard]] std::string asset_display_path(const std::string& reference,
                                             const Project* project);

} // namespace base_path
} // namespace fjell
