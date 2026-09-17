#include "renderer/resources/water_clipmap.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <set>
#include <utility>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr std::uint32_t RING = WATER_CLIPMAP_RING;
constexpr std::uint32_t HOLE = WATER_CLIPMAP_HOLE;

[[nodiscard]] bool inside_hole(glm::uvec2 patch, std::uint32_t offset) {
    return patch.x >= offset && patch.x < offset + HOLE && patch.y >= offset &&
           patch.y < offset + HOLE;
}

/// How wide one patch of a level is, in metres.
[[nodiscard]] float patch_span(std::uint32_t level) {
    return static_cast<float>((1U << level) * WATER_PATCH_QUADS);
}

} // namespace

TEST_CASE("Patch count is the solid level plus a ring per level above it") {
    const std::uint32_t full = RING * RING;
    REQUIRE(WaterClipmap::patch_count() ==
            full + (WATER_CLIPMAP_LEVELS - 1) * (full - HOLE * HOLE));
}

TEST_CASE("Every patch index resolves to a distinct patch of its level") {
    // The addressing is what the mesh shader uses to turn a workgroup id into
    // a piece of sea. A collision draws one patch twice and leaves a square
    // hole somewhere else, which on water reads as a tear straight through to
    // the sky rather than as a missing object.
    for (std::uint32_t offset = 0; offset <= RING - HOLE; ++offset) {
        std::set<std::pair<std::uint32_t, std::uint64_t>> seen;
        for (std::uint32_t i = 0; i < WaterClipmap::patch_count(); ++i) {
            const WaterPatchAddress address = water_patch_address(i, offset);
            REQUIRE(address.level < WATER_CLIPMAP_LEVELS);
            REQUIRE(address.patch.x < RING);
            REQUIRE(address.patch.y < RING);
            const auto key =
                std::pair{address.level,
                          static_cast<std::uint64_t>(address.patch.y) * RING + address.patch.x};
            REQUIRE(seen.insert(key).second);
        }
        REQUIRE(seen.size() == WaterClipmap::patch_count());
    }
}

TEST_CASE("Level 0 is solid and the levels above it leave the hole empty") {
    for (std::uint32_t offset = 0; offset <= RING - HOLE; ++offset) {
        std::uint32_t level0 = 0;
        for (std::uint32_t i = 0; i < WaterClipmap::patch_count(); ++i) {
            const WaterPatchAddress address = water_patch_address(i, offset);
            if (address.level == 0) {
                ++level0;
                continue;
            }
            // A patch drawn inside the hole overlaps the finer level covering
            // the same water, and the two z-fight through each other.
            REQUIRE_FALSE(inside_hole(address.patch, offset));
        }
        REQUIRE(level0 == RING * RING);
    }
}

TEST_CASE("Every level draws its full ring") {
    std::array<std::uint32_t, WATER_CLIPMAP_LEVELS> per_level{};
    for (std::uint32_t i = 0; i < WaterClipmap::patch_count(); ++i) {
        ++per_level[water_patch_address(i, 3).level];
    }
    REQUIRE(per_level[0] == RING * RING);
    for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(per_level[level] == RING * RING - HOLE * HOLE);
    }
}

TEST_CASE("Levels snap to whole patches so a shift keeps the grid aligned") {
    WaterClipmap clipmap;
    for (float camera : {0.0F, 137.0F, -412.5F, 9001.25F}) {
        clipmap.recenter({camera, 0.0F, -camera});
        for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
            const auto& entry = clipmap.gpu().levels[level];
            // A fractional patch offset would put every patch boundary
            // somewhere different after a shift, and a ring would stop being
            // a ring.
            REQUIRE(std::fmod(entry.origin.x, patch_span(level)) == Approx(0.0F).margin(1e-2F));
            REQUIRE(std::fmod(entry.origin.y, patch_span(level)) == Approx(0.0F).margin(1e-2F));
        }
    }
}

TEST_CASE("Cell size doubles per level and level 0 is a metre") {
    WaterClipmap clipmap;
    clipmap.recenter({0.0F, 0.0F, 0.0F});
    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(clipmap.gpu().levels[level].cell_size ==
                Approx(static_cast<float>(1U << level)));
    }
    REQUIRE(WaterClipmap::finest_spacing() == Approx(1.0F));
}

TEST_CASE("A level holds still until the camera crosses one of its own patches") {
    WaterClipmap clipmap;
    clipmap.recenter({0.0F, 0.0F, 0.0F});
    const GpuWaterClipmap before = clipmap.gpu();

    // Level 0's patch is 11 m, so 5 m moves nothing at all. This is the point
    // of snapping: a level that drifted with the camera would slide its
    // vertices through the wave field every frame and the sea would crawl.
    clipmap.recenter({5.0F, 0.0F, 5.0F});
    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(clipmap.gpu().levels[level].origin.x == Approx(before.levels[level].origin.x));
        REQUIRE(clipmap.gpu().levels[level].origin.y == Approx(before.levels[level].origin.y));
    }

    // 12 m crosses level 0's patch but none of the coarser ones, whose
    // patches are 22 m and up.
    clipmap.recenter({12.0F, 0.0F, 0.0F});
    REQUIRE(clipmap.gpu().levels[0].origin.x != Approx(before.levels[0].origin.x));
    for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(clipmap.gpu().levels[level].origin.x == Approx(before.levels[level].origin.x));
    }
}

TEST_CASE("Each level's hole is wholly covered by the level inside it") {
    // The one that matters. Every level snaps to its own grid, so the inner
    // level lands anywhere within one of the outer level's patches; a hole at
    // a fixed offset is only covered when the two happen to align, and
    // otherwise leaves a band of sea that no level draws — a tear through to
    // the sky, up to 352 m wide on the outer levels. Swept over a long camera
    // path because the misalignment is a function of where the camera is, and
    // any single position can easily be one of the ones that happens to work.
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 1.37F;
        clipmap.recenter({camera, 0.0F, camera * 0.5F - 900.0F});

        for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
            const auto& entry = clipmap.gpu().levels[level];
            const auto& inner = clipmap.gpu().levels[level - 1];
            const float span = patch_span(level);
            const float inner_span = patch_span(level - 1) * static_cast<float>(RING);

            const glm::vec2 hole_min =
                entry.origin + glm::vec2(span * static_cast<float>(entry.hole_offset));
            const glm::vec2 hole_max =
                entry.origin + glm::vec2(span * static_cast<float>(entry.hole_offset + HOLE));

            REQUIRE(hole_min.x >= inner.origin.x - 1e-2F);
            REQUIRE(hole_min.y >= inner.origin.y - 1e-2F);
            REQUIRE(hole_max.x <= inner.origin.x + inner_span + 1e-2F);
            REQUIRE(hole_max.y <= inner.origin.y + inner_span + 1e-2F);
        }
    }
}

TEST_CASE("The hole stays inside the level it is cut from") {
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 0.83F - 1500.0F;
        clipmap.recenter({camera, 0.0F, -camera * 0.31F});
        for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
            REQUIRE(clipmap.gpu().levels[level].hole_offset + HOLE <= RING);
        }
    }
}

TEST_CASE("Each level is centred on the camera to within one of its patches") {
    WaterClipmap clipmap;
    const glm::vec2 camera{137.0F, -412.0F};
    clipmap.recenter({camera.x, 0.0F, camera.y});

    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        const auto& entry = clipmap.gpu().levels[level];
        const float span = patch_span(level) * static_cast<float>(RING);
        const glm::vec2 centre = entry.origin + glm::vec2(span * 0.5F);
        // Snapping means "centred" is only exact to within one patch.
        REQUIRE(std::abs(centre.x - camera.x) <= patch_span(level));
        REQUIRE(std::abs(centre.y - camera.y) <= patch_span(level));
    }
}

TEST_CASE("The surface reaches past the horizon a standing camera sees") {
    // Roughly sqrt(2 * R * h) for eye height h on a globe of radius R: about
    // 5 km at 2 m. The clipmap is sized to reach that without a skirt.
    REQUIRE(WaterClipmap::half_extent() > 4000.0F);
}
