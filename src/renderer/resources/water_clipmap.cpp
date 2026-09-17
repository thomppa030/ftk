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

} // namespace

WaterClipmap::WaterClipmap() {
    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        // The level snaps in whole patches, so a patch is the cell it is
        // recentered on. Its extent is still counted in vertices, which is
        // what makes the coverage come out right.
        levels_[level].cell_size = glm::vec2(patch_span(level));
        levels_[level].cell_counts = glm::ivec2(static_cast<int>(RING));
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

    // Place each hole over the level inside it. Every level snaps to its own
    // grid, so the inner level lands anywhere within one of this level's
    // patches and a hole at a fixed offset would sometimes leave a band of
    // sea undrawn. The hole starts at the first patch that is wholly inside
    // the inner level, which puts it at 3 or 4 patches in and is what makes
    // the coverage gap-free rather than usually-fine.
    //
    // Both axes take the same offset: the two levels' origins differ by the
    // same snapping on each, so a square hole stays square.
    gpu_.levels[0].hole_offset = 0;
    for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
        const float span = patch_span(level);
        const glm::vec2 inner = gpu_.levels[level - 1].origin;
        const glm::vec2 outer = gpu_.levels[level].origin;
        const glm::vec2 first = glm::ceil((inner - outer) / span);
        const auto offset = static_cast<std::uint32_t>(
            std::clamp(std::max(first.x, first.y), 0.0F,
                       static_cast<float>(RING - HOLE)));
        gpu_.levels[level].hole_offset = offset;
    }
}

WaterPatchAddress water_patch_address(std::uint32_t patch_index,
                                      std::uint32_t hole_offset) {
    WaterPatchAddress address{};

    if (patch_index < FULL_PATCHES) {
        address.level = 0;
        address.patch = {patch_index % RING, patch_index / RING};
        return address;
    }

    const std::uint32_t ring_index = patch_index - FULL_PATCHES;
    address.level = 1 + ring_index / RING_PATCHES;
    std::uint32_t local = ring_index % RING_PATCHES;

    const std::uint32_t hole_end = hole_offset + HOLE;

    // Walk the rows, spending each row's drawn patches as we pass it. The
    // rows clear of the hole are full; the ones crossing it are short by the
    // hole's width. A closed form would be shorter and would be the third
    // place this topology is written down — the loop runs twelve times at
    // most and is read once.
    for (std::uint32_t pz = 0; pz < RING; ++pz) {
        const bool row_crosses_hole = pz >= hole_offset && pz < hole_end;
        const std::uint32_t drawn = row_crosses_hole ? RING - HOLE : RING;
        if (local >= drawn) {
            local -= drawn;
            continue;
        }
        std::uint32_t px = local;
        if (row_crosses_hole && px >= hole_offset) {
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
