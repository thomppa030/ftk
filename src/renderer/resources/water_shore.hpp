#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <optional>

namespace fjell {

struct FjTerrainData;

namespace water {

/// The ground under a body of water, as the wave field reads it: a terrain's
/// heights, where that terrain stands and where the water stands at rest.
///
/// Waves answer to how deep the water is under them — they grow, shorten and
/// break as it shallows — and a field that does not know the ground puts full
/// waves on dry sand. The bed is the terrain's own CPU heights, shared rather
/// than copied: the same array physics collides with, so a wave dies where a
/// character's feet find the ground.
///
/// A terrain places its field by translation alone (see TerrainAsset), so
/// the whole placement is a corner and a height.
struct ShoreBed {
    std::shared_ptr<const FjTerrainData> terrain;
    /// World XZ of the terrain's min corner, where its field coordinates start.
    glm::vec2 min_corner{0.0f};
    /// World Y of the terrain node, which its heights stand on.
    float ground_y{0.0f};
    /// World Y of the water at rest.
    float surface_y{0.0f};
};

/// How deep the still water is at a point, and how that changes across it.
struct StillDepth {
    /// Metres from the surface at rest down to the bed; negative where the
    /// bed stands above the water.
    float depth{0.0f};
    /// d(depth)/dx and d(depth)/dz. The bed is read bilinearly between its
    /// samples, so this is the slope of that surface, constant along a cell
    /// in one direction and stepping at the sample lines in the other.
    glm::vec2 gradient{0.0f};
};

/// The still-water depth at world `world_xz`, or nothing where the bed is not
/// known: off the terrain, or with no terrain at all. Nothing is open water —
/// no bed near enough to matter — and not a depth of zero.
///
/// Read bilinearly between samples, the way the terrain draws itself, so the
/// waterline is where the drawn ground meets the water.
[[nodiscard]] std::optional<StillDepth> still_depth(const ShoreBed& bed, glm::vec2 world_xz);

} // namespace water
} // namespace fjell
