#include "renderer/resources/water_clipmap.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

namespace {

constexpr std::uint32_t RING = WATER_CLIPMAP_RING;
constexpr std::uint32_t HOLE = WATER_CLIPMAP_HOLE;
constexpr std::uint32_t FULL_PATCHES = RING * RING;
constexpr std::uint32_t RING_PATCHES = FULL_PATCHES - HOLE * HOLE;

// How wide one patch of a level is, in metres.
[[nodiscard]] float patch_span(std::uint32_t level) {
    return static_cast<float>((1U << level) * WATER_PATCH_QUADS);
}

// The step a level moves in: two of its patches, not one.
//
// A patch is an odd number of cells wide (11), so a level snapped to single
// patches sits at an odd multiple of its own cell size half the time — and
// then its edge falls between two of the next level's vertices, which is a
// seam that cannot be closed because the vertex to close it against does not
// exist. Two patches is even, so every level's edge lands on the coarser
// level's vertex grid at every camera position, and the hole and the level
// filling it share their edges exactly. The level moves in 22 m steps instead
// of 11 m at the finest, which nothing can see.
[[nodiscard]] float snap_span(std::uint32_t level) {
    return patch_span(level) * 2.0F;
}

} // namespace

WaterClipmap::WaterClipmap() {
    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        // The level steps in pairs of patches, so that pair is the cell it is
        // recentered on, and the count it is recentered with is half the
        // patches it draws. Its extent is still counted in vertices, which is
        // what makes the coverage come out right.
        levels_[level].cell_size = glm::vec2(snap_span(level));
        levels_[level].cell_counts = glm::ivec2(static_cast<int>(RING / 2));
        gpu_.levels[level].cell_size = static_cast<float>(1U << level);
    }
    gpu_.level_count = WATER_CLIPMAP_LEVELS;
    gpu_.ring = RING;
    gpu_.hole = HOLE;
    gpu_.patch_quads = WATER_PATCH_QUADS;
}

void WaterClipmap::recenter(const glm::vec3& camera_position) {
    const glm::vec2 camera_xz{camera_position.x, camera_position.z};

    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        levels_[level].recenter(camera_xz);
        gpu_.levels[level].origin = levels_[level].snapped_origin;
    }

    // Place each hole over the level inside it. A fixed offset would not do:
    // the levels snap on their own grids, so the inner one sits three or four
    // of this level's patches in depending on where the camera is, and a hole
    // in the wrong place is either a band of sea no level draws or two levels
    // of transparent water drawn over each other.
    //
    // The division is exact. Snapping in pairs of patches puts the inner
    // level's origin on a whole patch of this one, and its span is exactly
    // HOLE of them, so the hole and the level filling it share their edges.
    //
    // Per axis, because the two do not agree: each axis snaps on its own, so
    // the inner level sits three of this level's patches in along one and
    // four along the other most of the time. One offset for both would put
    // the hole beside the level it is meant to be covering.
    gpu_.levels[0].set_hole_offset({0, 0});
    for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
        const float span = patch_span(level);
        const glm::vec2 inner = gpu_.levels[level - 1].origin;
        const glm::vec2 outer = gpu_.levels[level].origin;
        const glm::vec2 offset_patches = glm::round((inner - outer) / span);
        const float limit = static_cast<float>(RING - HOLE);
        gpu_.levels[level].set_hole_offset(
            {static_cast<std::uint32_t>(std::clamp(offset_patches.x, 0.0F, limit)),
             static_cast<std::uint32_t>(std::clamp(offset_patches.y, 0.0F, limit))});
    }
}

WaterPatchAddress water_patch_address(std::uint32_t patch_index, glm::uvec2 hole_offset) {
    WaterPatchAddress address{};

    if (patch_index < FULL_PATCHES) {
        address.level = 0;
        address.patch = {patch_index % RING, patch_index / RING};
        return address;
    }

    const std::uint32_t ring_index = patch_index - FULL_PATCHES;
    address.level = 1 + ring_index / RING_PATCHES;
    std::uint32_t local = ring_index % RING_PATCHES;

    const glm::uvec2 hole_end = hole_offset + glm::uvec2(HOLE);

    // Walk the rows, spending each row's drawn patches as we pass it. The
    // rows clear of the hole are full; the ones crossing it are short by the
    // hole's width. A closed form would be shorter and would be the third
    // place this topology is written down — the loop runs twelve times at
    // most and is read once.
    for (std::uint32_t pz = 0; pz < RING; ++pz) {
        const bool row_crosses_hole = pz >= hole_offset.y && pz < hole_end.y;
        const std::uint32_t drawn = row_crosses_hole ? RING - HOLE : RING;
        if (local >= drawn) {
            local -= drawn;
            continue;
        }
        std::uint32_t px = local;
        if (row_crosses_hole && px >= hole_offset.x) {
            px += HOLE;
        }
        address.patch = {px, pz};
        return address;
    }

    // Unreachable for an index below patch_count(); a caller past the end
    // gets the last patch rather than an out-of-grid coordinate.
    address.patch = {RING - 1, RING - 1};
    return address;
}

} // namespace fjell
