#pragma once

#include <glm/glm.hpp>

namespace fjell {

/// Geometry for one level of a camera-centered clipmap — ported from
/// Unreal's Lumen radiance cache (LumenRadianceCache.cpp,
/// FClipmapGeometry/FClipmapLevelGeometry/SnapToGrid).
///
/// A clipmap level recenters on the camera every time the camera crosses a
/// cell boundary at this level's resolution. Its origin is always snapped to
/// the cell grid (floor(origin / cell_size) * cell_size), so recomputing it
/// from the same camera position twice in a row is idempotent — the level
/// simply doesn't move until the camera has moved at least one full cell.
/// That snapping is what makes the contents of a cell reusable across
/// frames: a cell keeps its world position, so whatever was computed for it
/// stays valid until the cell leaves the level entirely.
///
/// `N` is the number of axes the level spans: 2 for a level laid over the
/// ground plane (a water surface, a terrain ring), 3 for a volume (a probe
/// grid). The geometry is identical either way; only the vector width
/// changes.
template <int N>
struct ClipmapLevel {
    using vec = glm::vec<N, float>;
    using ivec = glm::vec<N, int>;

    /// World-space size of one cell on this level. May be non-uniform per
    /// axis.
    vec cell_size{2.0f};

    /// Number of cells along each axis. The level always covers exactly
    /// `cell_counts * cell_size` world units, centered (as closely as
    /// cell-snapping allows) on whatever camera position was passed to
    /// recenter().
    ivec cell_counts{16};

    /// World-space position of cell (0,0[,0]) on this level, snapped to this
    /// level's cell grid. Equivalent to Lumen's SnappedOrigin, but placed so
    /// the level's coverage is centered on the camera rather than being a
    /// "corner" origin — cell_counts/2 cells extend in each direction from
    /// the camera's snapped cell.
    vec snapped_origin{0.0f};

    /// True the first time this level is ever recentered (no previous
    /// snapped_origin to compare against) or whenever fundamental parameters
    /// change (cell_size, cell_counts) — signals a full discard is
    /// unavoidable, matching Lumen's bResetState. Ordinary camera-driven
    /// recentering never sets this.
    bool full_reset{true};

    /// Snap a world position down to a cell grid (floor, not round — matches
    /// Lumen's SnapToGrid so behavior is identical for negative coordinates
    /// too).
    [[nodiscard]] static vec snap_to_grid(const vec& position, const vec& cell_size) {
        return glm::floor(position / cell_size) * cell_size;
    }

    /// Recompute snapped_origin for a new camera position. Idempotent:
    /// calling this again with a camera position inside the same snapped
    /// cell as last time leaves snapped_origin unchanged and full_reset
    /// false. Returns true if the origin actually moved (or this is the
    /// first recenter), which is the "did this level change" signal a caller
    /// needs to know whether any per-cell reallocation work is due.
    bool recenter(const vec& camera_position) {
        vec half_extent = vec(cell_counts) * cell_size * 0.5f;
        vec desired_origin = camera_position - half_extent;
        vec new_snapped_origin = snap_to_grid(desired_origin, cell_size);

        bool first_time = full_reset;
        bool moved = first_time || glm::any(glm::notEqual(new_snapped_origin, snapped_origin));
        snapped_origin = new_snapped_origin;
        // Only the very first recenter() is a full reset — every call after
        // that clears the flag permanently, even if this call itself didn't
        // move the origin. A bug here (leaving full_reset set) would make
        // every subsequent recenter() look like a fresh level to callers,
        // defeating the entire point of cell-snapping: caught by "Recenter
        // within the same snapped cell does not move the origin" in
        // test_clipmap.cpp before this ever reached real code.
        full_reset = false;
        return moved;
    }

    /// World-space bounds this level currently covers, recomputed every
    /// recenter(). For callers that want a plain AABB (culling, debug draw)
    /// without going through per-cell addressing.
    [[nodiscard]] vec world_min() const { return snapped_origin; }
    [[nodiscard]] vec world_max() const {
        return snapped_origin + vec(cell_counts - ivec(1)) * cell_size;
    }

    /// Level-relative grid coordinate for a world position (may be negative
    /// or >= cell_counts if the position isn't currently covered by this
    /// level — callers must range-check with covers() before using it to
    /// index anything).
    [[nodiscard]] ivec world_to_grid(const vec& world_pos) const {
        return ivec(glm::floor((world_pos - snapped_origin) / cell_size));
    }

    /// True if a level-relative grid coordinate falls within this level's
    /// current coverage.
    [[nodiscard]] bool covers(const ivec& grid_coord) const {
        return glm::all(glm::greaterThanEqual(grid_coord, ivec(0)))
            && glm::all(glm::lessThan(grid_coord, cell_counts));
    }

    /// How many whole cells the origin moved between two snapped origins, as
    /// an exact integer cell count. Both origins are snapped to the same cell
    /// grid, so their difference is always a whole multiple of cell_size —
    /// the rounding here only absorbs float representation error, it is not
    /// hiding a fractional shift.
    [[nodiscard]] ivec shift_in_cells(const vec& previous_origin) const {
        return ivec(glm::round((snapped_origin - previous_origin) / cell_size));
    }

    /// Translate a grid coordinate expressed against the CURRENT origin into
    /// the same world cell's coordinate against a PREVIOUS origin.
    ///
    /// This is the core of a reallocation pass: for each cell of the
    /// newly-positioned level, ask "did the old level already have this exact
    /// world cell?". If the result is covers(), the cell keeps whatever it
    /// already had (the common case — a one-cell shift retains all but one
    /// slab). If not, the cell is newly uncovered and needs fresh content.
    [[nodiscard]] ivec grid_to_previous_grid(const ivec& grid_coord,
                                             const vec& previous_origin) const {
        return grid_coord + shift_in_cells(previous_origin);
    }

    /// Number of cells in this level that were NOT covered by the level at
    /// `previous_origin` — i.e. how many must be reallocated after this
    /// shift.
    ///
    /// Equivalently: the count of freed cells, since the level's cell count
    /// is fixed. A reallocation pass can rely on those two counts being equal
    /// and pair them off directly instead of running a free list. Computed in
    /// closed form rather than by iterating, so it stays cheap enough to call
    /// per shift on the CPU.
    [[nodiscard]] int newly_covered_cell_count(const vec& previous_origin) const {
        ivec shift = glm::abs(shift_in_cells(previous_origin));
        // Overlap along each axis is the extent minus the shift, clamped at
        // zero — a shift at or beyond the full extent means no overlap at all
        // on that axis (the teleport / large-jump case, where every cell is
        // newly covered).
        ivec overlap = glm::max(cell_counts - shift, ivec(0));
        int total = 1;
        int retained = 1;
        for (int axis = 0; axis < N; ++axis) {
            total *= cell_counts[axis];
            retained *= overlap[axis];
        }
        return total - retained;
    }
};

/// A clipmap over the ground plane: XZ in world space, so `vec2(x, z)`.
using ClipmapLevel2D = ClipmapLevel<2>;

/// A clipmap over a volume.
using ClipmapLevel3D = ClipmapLevel<3>;

} // namespace fjell
