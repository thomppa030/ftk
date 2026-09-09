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

    /// How many whole cells the origin moved between two snapped
    /// origins, as an exact integer cell count. Both origins are
    /// snapped to the same cell grid, so their difference is always a
    /// whole multiple of cell_size — the rounding here only absorbs
    /// float representation error, it is not hiding a fractional shift.
    [[nodiscard]] glm::ivec3 shift_in_cells(const glm::vec3& previous_origin) const {
        return glm::ivec3(glm::round((snapped_origin - previous_origin) / cell_size));
    }

    /// Translate a grid coordinate expressed against the CURRENT origin
    /// into the same world cell's coordinate against a PREVIOUS origin.
    ///
    /// This is the core of the reallocation pass: for each cell of the
    /// newly-positioned level, ask "did the old level already have a
    /// probe for this exact world cell?". If the result is covered(),
    /// the cell keeps whatever slot it already had (the common case —
    /// a one-cell shift retains all but one slab). If not, the cell is
    /// newly uncovered and needs a freshly allocated slot with no
    /// carried-over irradiance.
    [[nodiscard]] glm::ivec3 grid_to_previous_grid(const glm::ivec3& grid_coord,
                                                    const glm::vec3& previous_origin) const {
        return grid_coord + shift_in_cells(previous_origin);
    }

    /// Number of cells in this level that were NOT covered by the level
    /// at `previous_origin` — i.e. how many probe slots must be
    /// reallocated and retraced after this shift.
    ///
    /// Equivalently: the count of freed slots, since the level's cell
    /// count is fixed. The reallocation pass relies on those two counts
    /// being equal so it can pair them off directly instead of running
    /// an atomic free-list. Computed in closed form rather than by
    /// iterating, so it stays cheap enough to call per shift on the CPU.
    /// Normalized distance from the nearest face of this level, in cells,
    /// clamped to [0,1]. 0 exactly at the boundary, rising to 1 one cell
    /// inside it. Ported from Lumen's EdgeFade
    /// (LumenRadianceCacheInterpolation.ush, GetRadianceProbeClipmap): a
    /// level is accepted only where this exceeds a per-pixel dither value,
    /// so positions near a level's edge stochastically fall through to the
    /// next coarser level instead of hard-cutting at it.
    [[nodiscard]] float edge_fade(const glm::vec3& world_pos, float inv_fade_cells = 1.0f) const {
        glm::vec3 coord = (world_pos - snapped_origin) / cell_size;
        glm::vec3 low = glm::clamp((coord - 0.5f) * inv_fade_cells, 0.0f, 1.0f);
        glm::vec3 high = glm::clamp(
            (glm::vec3(probe_counts) - 0.5f - coord) * inv_fade_cells, 0.0f, 1.0f);
        return glm::min(glm::min(low.x, glm::min(low.y, low.z)),
                        glm::min(high.x, glm::min(high.y, high.z)));
    }

    [[nodiscard]] int newly_covered_cell_count(const glm::vec3& previous_origin) const {
        glm::ivec3 shift = glm::abs(shift_in_cells(previous_origin));
        // Overlap along each axis is the extent minus the shift,
        // clamped at zero — a shift at or beyond the full extent means
        // no overlap at all on that axis (the teleport / large-jump
        // case, where every cell is newly covered).
        glm::ivec3 overlap = glm::max(probe_counts - shift, glm::ivec3(0));
        int total = probe_counts.x * probe_counts.y * probe_counts.z;
        int retained = overlap.x * overlap.y * overlap.z;
        return total - retained;
    }
};

/// Largest number of clipmap levels the GPU side is sized for. The
/// indirection texture packs levels side by side along X, and each level
/// costs a full probe atlas slice, so this bounds both.
inline constexpr int DDGI_MAX_CLIPMAP_LEVELS = 4;

/// A stack of nested clipmap levels, each covering twice the world extent
/// of the one below it at half the probe density — the same arrangement as
/// Lumen's radiance cache clipmaps (LumenRadianceCache.cpp,
/// FClipmapGeometry::GetCellSize: Level0CellSize * 2^Level).
///
/// Every level holds the same probe_counts and recenters on the same
/// camera; only cell_size differs. So level 0 is dense and close, and each
/// level out doubles reach for the same probe budget. A position is served
/// by the finest level that comfortably contains it (see select_level).
struct DdgiClipmap {
    DdgiClipmapLevel levels[DDGI_MAX_CLIPMAP_LEVELS];
    int level_count{1};

    /// Configure the stack: every level gets the same resolution, and cell
    /// sizes double outward from base_cell_size. Marks all levels for a
    /// full reset, since changing either parameter moves every probe.
    void configure(int count, const glm::ivec3& probe_counts, const glm::vec3& base_cell_size) {
        level_count = glm::clamp(count, 1, DDGI_MAX_CLIPMAP_LEVELS);
        for (int i = 0; i < level_count; ++i) {
            glm::vec3 cell = base_cell_size * static_cast<float>(1 << i);
            if (levels[i].probe_counts != probe_counts || levels[i].cell_size != cell) {
                levels[i].probe_counts = probe_counts;
                levels[i].cell_size = cell;
                levels[i].full_reset = true;
            }
        }
    }

    /// Recenter every level on the camera. Returns true if any level moved
    /// — coarse levels move rarely, since their cells are larger and the
    /// camera has to cross a full one before the snapped origin changes.
    bool recenter(const glm::vec3& camera_position) {
        bool any_moved = false;
        for (int i = 0; i < level_count; ++i) {
            any_moved |= levels[i].recenter(camera_position);
        }
        return any_moved;
    }

    /// Finest level whose edge_fade at this position exceeds `dither`, or
    /// -1 if no level covers it. Matches Lumen's GetRadianceProbeClipmap:
    /// levels are tried finest-first and a level near its own boundary is
    /// stochastically rejected in favour of the next coarser one, so the
    /// transition dissolves per-pixel instead of appearing as a seam.
    /// Lumen never blends two levels' irradiance together.
    [[nodiscard]] int select_level(const glm::vec3& world_pos, float dither,
                                    float inv_fade_cells = 1.0f) const {
        for (int i = 0; i < level_count; ++i) {
            if (levels[i].edge_fade(world_pos, inv_fade_cells) > dither) {
                return i;
            }
        }
        return -1;
    }

    /// World extent covered by the coarsest level — the outer limit of any
    /// GI at all.
    [[nodiscard]] glm::vec3 outer_min() const { return levels[level_count - 1].world_min(); }
    [[nodiscard]] glm::vec3 outer_max() const { return levels[level_count - 1].world_max(); }
};

} // namespace fjell
