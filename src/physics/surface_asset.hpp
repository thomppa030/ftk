#pragma once

#include "core/result.hpp"

#include <string>

namespace fjell {

/// A physics surface: what kind of ground or wall a collider is, for gameplay
/// that reacts to it — footstep sounds, impact effects, decals. Authored as a
/// `.fjsurface` file and referenced by colliders and terrain layers.
///
/// The runtime identity of a surface is its asset handle: a raycast hit, a
/// character's ground and a contact event all report that id, and a script
/// compares it against the handle of an `Asset<Surface>` it holds. The
/// file carries only what every surface shares; game data keyed by surface
/// belongs in the game, keyed by the same handle.
struct SurfaceAsset {
    /// Display name, for logs and the inspector. Defaults to the file stem.
    std::string name;
};

[[nodiscard]] Result<SurfaceAsset> load_surface_asset_file(const std::string& path);
[[nodiscard]] Result<> save_surface_asset_file(const std::string& path, const SurfaceAsset& asset);

} // namespace fjell
