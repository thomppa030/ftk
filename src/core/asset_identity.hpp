#pragma once

// Reading and writing an asset's UUID in the asset itself, so identity
// survives the file being moved or renamed outside the editor.
//
// Where the id lives depends on what the engine can write into:
//   Fjell JSON (.fjmat, .fjvfx, …)  → an "__uuid" field
//   Fjell binary (.fjmesh, .fjskel) → the format's own name table
//   Foreign (.png, .glb, .wav, …)   → a "<file>.fjid" sidecar
//
// Foreign files get a sidecar because there is nowhere inside a .png to put
// an id. Fjell formats never do — carrying it in two places would make one
// of them wrong.

#include "core/asset_id.hpp"

#include <filesystem>
#include <string>

namespace fjell::asset_identity {

/// The field a Fjell JSON asset stores its id in. Loaders that validate their
/// top-level fields need to treat it as known rather than as a typo.
inline constexpr const char* UUID_KEY = "__uuid";

/// How a given file stores its id.
enum class Carrier {
    fjell_json,   // "__uuid" field in the file
    fjell_binary, // the format's own name table
    sidecar,      // "<file>.fjid" next to it
};

/// Which carrier a file extension uses. Foreign extensions fall back to a
/// sidecar, so an unrecognised-but-trackable type still gets an identity.
[[nodiscard]] Carrier carrier_for(const std::string& extension);

/// Sidecar path for a file ("foo.png" → "foo.png.fjid").
[[nodiscard]] std::filesystem::path sidecar_path(const std::filesystem::path& file);

/// True for the sidecars themselves, which are bookkeeping rather than assets
/// and must never be tracked or shown as content.
[[nodiscard]] bool is_sidecar(const std::filesystem::path& file);

/// Reads the id an asset carries. Returns an invalid AssetId when the file has
/// none yet, is unreadable, or is a format that predates carrying one.
[[nodiscard]] AssetId read(const std::filesystem::path& file);

/// Writes an id into the asset, creating a sidecar if that is its carrier.
/// Returns false if the id could not be stored — the caller keeps the database
/// entry either way, so the asset is still usable, just not movable.
bool write(const std::filesystem::path& file, const AssetId& id);

} // namespace fjell::asset_identity
