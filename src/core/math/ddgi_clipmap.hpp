#pragma once

#include <glm/glm.hpp>

namespace fjell {

/// Geometry for one level of a camera-centered DDGI probe clipmap —
/// ported from Unreal's Lumen radiance cache (LumenRadianceCache.cpp,
/// FClipmapGeometry/FClipmapLevelGeometry/SnapToGrid), adapted to a
/// single level for now. See Plans/ddgi-camera-centered-clipmap.md for
/// the full design (multi-level, indirection texture, sample selection).
///
/// Unlike Fjell's original DDGI grid (fit once to scene bounds, then
/// frozen — see DdgiConfig's doc comment), a clipmap level recenters on
/// the camera every time the camera crosses a cell boundary at this
/// level's resolution. Its origin is always snapped to the cell grid
/// (floor(origin / cell_size) * cell_size), so recomputing this from the
/// same camera position twice in a row is idempotent — the level simply
/// doesn't move until the camera has moved at least one full cell.
struct DdgiClipmapLevel {
    /// World-space size of one probe cell on this level (may be
    /// non-uniform per axis, matching DdgiConfig::probe_spacing).
    glm::vec3 cell_size{2.0f};

    /// Number of probes along each axis. The level always covers exactly
    /// `probe_counts * cell_size` world units, centered (as closely as
    /// cell-snapping allows) on whatever camera position was passed to
    /// recenter().
    glm::ivec3 probe_counts{16, 8, 16};

    /// World-space position of probe (0,0,0) on this level, snapped to
    /// this level's cell grid. Equivalent to Lumen's SnappedOrigin, but
    /// placed so the level's coverage is centered on the camera rather
    /// than being a "corner" origin — probe_counts/2 cells extend in
    /// each direction from the camera's snapped cell.
    glm::vec3 snapped_origin{0.0f};

    /// True the first time this level is ever recentered (no previous
    /// snapped_origin to compare against) or whenever fundamental
    /// parameters change (cell_size, probe_counts) — signals a full
    /// discard is unavoidable, matching Lumen's bResetState. Ordinary
    /// camera-driven recentering never sets this; the indirection layer
    /// (not yet implemented — see the plan doc) is what makes ordinary
    /// recentering cheap instead of a full reset.
    bool full_reset{true};

    /// Snap a world position down to this level's cell grid (floor, not
    /// round — matches Lumen's SnapToGrid so behavior is identical for
    /// negative coordinates too).
    [[nodiscard]] static glm::vec3 snap_to_grid(const glm::vec3& position,
                                                 const glm::vec3& cell_size) {
        return glm::floor(position / cell_size) * cell_size;
    }

    /// Recompute snapped_origin for a new camera position. Idempotent:
    /// calling this again with a camera position inside the same snapped
    /// cell as last time leaves snapped_origin unchanged and full_reset
    /// false. Returns true if the origin actually moved (or this is the
    /// first recenter), matching the "did this level change" signal a
    /// caller needs to know whether to run the indirection-update pass.
    bool recenter(const glm::vec3& camera_position) {
        glm::vec3 half_extent = glm::vec3(probe_counts) * cell_size * 0.5f;
        glm::vec3 desired_origin = camera_position - half_extent;
        glm::vec3 new_snapped_origin = snap_to_grid(desired_origin, cell_size);

        bool first_time = full_reset;
        bool moved = first_time || glm::any(glm::notEqual(new_snapped_origin, snapped_origin));
        snapped_origin = new_snapped_origin;
        // Only the very first recenter() is a full reset — every call
        // after that clears the flag permanently, even if this call
        // itself didn't move the origin. A bug here (leaving full_reset
        // set) would make every subsequent recenter() look like a fresh
        // grid to callers, defeating the entire point of cell-snapping:
        // caught by "Recenter within the same snapped cell does not move
        // the origin" in test_ddgi_clipmap.cpp before this ever reached
        // real DDGI code.
        full_reset = false;
        return moved;
    }

    /// World-space bounds this level currently covers — the same shape
    /// as the old DdgiConfig's frozen grid_position/probe_counts/
    /// probe_spacing box, just recomputed every recenter() instead of
    /// once. Kept for reuse in places that still want a plain AABB
    /// (SDF burial bounds, debug draw, etc.) without going through
    /// per-probe indirection.
    [[nodiscard]] glm::vec3 world_min() const { return snapped_origin; }
    [[nodiscard]] glm::vec3 world_max() const {
        return snapped_origin + glm::vec3(probe_counts - glm::ivec3(1)) * cell_size;
    }

    /// Level-relative grid coordinate for a world position (may be
    /// negative or >= probe_counts if the position isn't currently
    /// covered by this level — callers must range-check before using it
    /// to index anything).
    [[nodiscard]] glm::ivec3 world_to_grid(const glm::vec3& world_pos) const {
        return glm::ivec3(glm::floor((world_pos - snapped_origin) / cell_size));
    }

    /// True if a level-relative grid coordinate falls within this
    /// level's current coverage.
    [[nodiscard]] bool covers(const glm::ivec3& grid_coord) const {
        return glm::all(glm::greaterThanEqual(grid_coord, glm::ivec3(0)))
            && glm::all(glm::lessThan(grid_coord, probe_counts));
    }
};

} // namespace fjell
