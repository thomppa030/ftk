#pragma once

// Finding and repointing the places that reference an asset.
//
// An id is a 32-character hex string, so a reference to one is found by
// scanning text for it — no per-format parsing, and a format the engine gains
// later is covered without being taught about. Repointing is the same scan
// with a replacement, which is why it stays correct for reference shapes
// nobody has written yet.

#include "core/asset_id.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fjell {

class AssetDatabase;

/// One file that references an asset, and how many times.
struct AssetReference {
    std::string relative_path;  // the referencing file, relative to the root
    uint32_t count{0};          // occurrences, so a file is listed once
};

/// Files referencing `id`, searched across every text asset in the project.
/// The asset itself is skipped — a mesh holding its own id in its name table
/// is identity, not a reference to something else.
[[nodiscard]] std::vector<AssetReference> find_asset_references(
    const std::filesystem::path& project_root,
    const AssetDatabase& db,
    const AssetId& id);

/// Rewrites every reference to `from` so it points at `to`. Returns how many
/// files changed. Writes are atomic per file; a file that cannot be written is
/// skipped and reported by the return count being lower than the reference
/// count.
uint32_t repoint_asset_references(const std::filesystem::path& project_root,
                                  const AssetDatabase& db,
                                  const AssetId& from,
                                  const AssetId& to);

} // namespace fjell
