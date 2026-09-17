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

[[nodiscard]] bool inside_hole(glm::uvec2 patch, glm::uvec2 offset) {
    return patch.x >= offset.x && patch.x < offset.x + HOLE && patch.y >= offset.y &&
           patch.y < offset.y + HOLE;
}

/// How wide one patch of a level is, in metres.
[[nodiscard]] float patch_span(std::uint32_t level) {
    return static_cast<float>((1U << level) * WATER_PATCH_QUADS);
}

/// The step a level moves in: two of its patches.
[[nodiscard]] float snap_span(std::uint32_t level) {
    return patch_span(level) * 2.0F;
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
    for (std::uint32_t ox = 0; ox <= RING - HOLE; ++ox) {
      for (std::uint32_t oz = 0; oz <= RING - HOLE; ++oz) {
        const glm::uvec2 offset{ox, oz};
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
}

TEST_CASE("Level 0 is solid and the levels above it leave the hole empty") {
    for (std::uint32_t ox = 0; ox <= RING - HOLE; ++ox) {
      for (std::uint32_t oz = 0; oz <= RING - HOLE; ++oz) {
        const glm::uvec2 offset{ox, oz};
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
}

TEST_CASE("Every level draws its full ring") {
    std::array<std::uint32_t, WATER_CLIPMAP_LEVELS> per_level{};
    for (std::uint32_t i = 0; i < WaterClipmap::patch_count(); ++i) {
        ++per_level[water_patch_address(i, {3, 4}).level];
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
            REQUIRE(std::fmod(entry.origin.x, snap_span(level)) == Approx(0.0F).margin(1e-2F));
            REQUIRE(std::fmod(entry.origin.y, snap_span(level)) == Approx(0.0F).margin(1e-2F));
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

TEST_CASE("A level holds still until the camera crosses one of its own steps") {
    WaterClipmap clipmap;
    clipmap.recenter({0.0F, 0.0F, 0.0F});
    const GpuWaterClipmap before = clipmap.gpu();

    // Level 0 steps in 22 m, so 5 m moves nothing at all. This is the point of
    // snapping: a level that drifted with the camera would slide its vertices
    // through the wave field every frame and the sea would crawl.
    clipmap.recenter({5.0F, 0.0F, 5.0F});
    for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(clipmap.gpu().levels[level].origin.x == Approx(before.levels[level].origin.x));
        REQUIRE(clipmap.gpu().levels[level].origin.y == Approx(before.levels[level].origin.y));
    }

    // 24 m crosses level 0's step but none of the coarser ones, whose steps
    // are 44 m and up.
    clipmap.recenter({24.0F, 0.0F, 0.0F});
    REQUIRE(clipmap.gpu().levels[0].origin.x != Approx(before.levels[0].origin.x));
    for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
        REQUIRE(clipmap.gpu().levels[level].origin.x == Approx(before.levels[level].origin.x));
    }
}

TEST_CASE("A level's edge lands on the vertex grid of the level outside it") {
    // What makes the seam between two levels stitchable at all. A patch is 11
    // cells wide, an odd number, so a level snapped to single patches sits at
    // an odd multiple of its own cell size half the time — and then its outer
    // edge falls *between* two of the coarser level's vertices, leaving a
    // crack with no vertex to stitch to. Snapping in pairs of patches makes
    // the offset even, and the edge lands on a coarse vertex at every camera
    // position rather than at half of them.
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 1.37F;
        clipmap.recenter({camera, 0.0F, camera * 0.5F - 900.0F});

        for (std::uint32_t level = 0; level + 1 < WATER_CLIPMAP_LEVELS; ++level) {
            const auto& fine = clipmap.gpu().levels[level];
            const float coarse_cell = clipmap.gpu().levels[level + 1].cell_size;
            const float span = patch_span(level) * static_cast<float>(RING);

            for (float edge : {fine.origin.x, fine.origin.x + span, fine.origin.y,
                               fine.origin.y + span}) {
                const float cells = edge / coarse_cell;
                REQUIRE(std::abs(cells - std::round(cells)) < 1e-3F);
            }
        }
    }
}

TEST_CASE("A level's hole has exactly the edges of the level filling it") {
    // The one that matters, and the reason levels snap in pairs of patches.
    // A hole larger than the level inside it is a band of sea nothing draws —
    // a tear through to the sky. A hole smaller is two levels of transparent
    // water drawn over each other. The fit is exact, so neither can happen:
    // the inner level is exactly HOLE of this level's patches wide and lands
    // on a whole patch of it.
    //
    // Swept over a long camera path because the alignment is a function of
    // where the camera is, and any single position can be one that happens to
    // work for the wrong reason.
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 1.37F;
        clipmap.recenter({camera, 0.0F, camera * 0.5F - 900.0F});

        for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
            const auto& entry = clipmap.gpu().levels[level];
            const auto& inner = clipmap.gpu().levels[level - 1];
            const float span = patch_span(level);
            const float inner_span = patch_span(level - 1) * static_cast<float>(RING);
            const glm::vec2 offset{entry.hole_offset_xz()};

            const glm::vec2 hole_min = entry.origin + offset * span;
            const glm::vec2 hole_max = hole_min + glm::vec2(span * static_cast<float>(HOLE));

            REQUIRE(hole_min.x == Approx(inner.origin.x).margin(1e-2F));
            REQUIRE(hole_min.y == Approx(inner.origin.y).margin(1e-2F));
            REQUIRE(hole_max.x == Approx(inner.origin.x + inner_span).margin(1e-2F));
            REQUIRE(hole_max.y == Approx(inner.origin.y + inner_span).margin(1e-2F));
        }
    }
}

TEST_CASE("A level's origin is an even number of its own cells") {
    // What the mesh shader's seam stitch rests on. It decides whether the
    // coarser level has a vertex at a given place by the parity of that
    // place's index in level cells — even means shared, odd means the fine
    // level alone has it and it must be dropped onto the coarse edge. That
    // only holds if the level's origin is itself at an even cell, or every
    // parity is inverted and the stitch snaps exactly the vertices it should
    // have left alone.
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 1.37F;
        clipmap.recenter({camera, 0.0F, camera * 0.5F - 900.0F});
        for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
            const auto& entry = clipmap.gpu().levels[level];
            for (float axis : {entry.origin.x, entry.origin.y}) {
                const float cells = axis / entry.cell_size;
                REQUIRE(std::abs(cells - std::round(cells)) < 1e-3F);
                REQUIRE(static_cast<long long>(std::llround(cells)) % 2 == 0);
            }
        }
    }
}

TEST_CASE("The hole stays inside the level it is cut from") {
    WaterClipmap clipmap;
    for (int step = 0; step < 4000; ++step) {
        const float camera = static_cast<float>(step) * 0.83F - 1500.0F;
        clipmap.recenter({camera, 0.0F, -camera * 0.31F});
        for (std::uint32_t level = 1; level < WATER_CLIPMAP_LEVELS; ++level) {
            const glm::uvec2 offset = clipmap.gpu().levels[level].hole_offset_xz();
            REQUIRE(offset.x + HOLE <= RING);
            REQUIRE(offset.y + HOLE <= RING);
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
        // Snapping means "centred" is only exact to within one step.
        REQUIRE(std::abs(centre.x - camera.x) <= snap_span(level));
        REQUIRE(std::abs(centre.y - camera.y) <= snap_span(level));
    }
}

TEST_CASE("The surface reaches past the horizon a standing camera sees") {
    // Roughly sqrt(2 * R * h) for eye height h on a globe of radius R: about
    // 5 km at 2 m. The clipmap is sized to reach that without a skirt.
    REQUIRE(WaterClipmap::half_extent() > 4000.0F);
}

// ── The shader's half of the patch addressing ──
//
// The mesh pipeline resolves a workgroup index to a patch in GLSL, and the
// table above resolves the same index in C++ for the tests to reason about.
// Two walks of the same topology drift, and the drift shows as a square of
// sea drawn twice while another is not drawn at all. Compiled here as C++ and
// compared index by index, the same way test_fjwater.cpp pins the wave
// field's two evaluators together.
#include "glsl_shim.hpp"
#include "../shaders/include/fjell_water_clipmap.glsl"

TEST_CASE("The shader resolves every patch index to the same patch the CPU does") {
    static_assert(FJELL_WATER_CLIPMAP_LEVELS == WATER_CLIPMAP_LEVELS,
                  "level count must match between the shader and the table");

    WaterClipmap clipmap;
    for (int step = 0; step < 40; ++step) {
        clipmap.recenter({static_cast<float>(step) * 37.0F, 0.0F,
                          static_cast<float>(step) * -53.0F});

        // The GPU table, read by the shader's own walk.
        FjellWaterClipmap shader{};
        shader.level_count = clipmap.gpu().level_count;
        shader.ring = clipmap.gpu().ring;
        shader.hole = clipmap.gpu().hole;
        shader.patch_quads = clipmap.gpu().patch_quads;
        for (std::uint32_t level = 0; level < WATER_CLIPMAP_LEVELS; ++level) {
            shader.levels[level].origin = clipmap.gpu().levels[level].origin;
            shader.levels[level].cell_size = clipmap.gpu().levels[level].cell_size;
            shader.levels[level].hole_offset = clipmap.gpu().levels[level].hole_offset;
        }

        for (std::uint32_t i = 0; i < WaterClipmap::patch_count(); ++i) {
            const FjellWaterPatch gpu = fjell_water_patch(shader, i);
            // The shader reads the hole offset out of the table by level; the
            // CPU side takes it as an argument, so the level the shader landed
            // on is what says which offset to check it against.
            const WaterPatchAddress cpu =
                water_patch_address(i, clipmap.gpu().levels[gpu.level].hole_offset_xz());

            REQUIRE(gpu.level == cpu.level);
            REQUIRE(gpu.coord.x == cpu.patch.x);
            REQUIRE(gpu.coord.y == cpu.patch.y);
        }
    }
}
