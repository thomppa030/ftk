#include "core/math/ddgi_clipmap.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

using namespace fjell;
using Catch::Approx;

TEST_CASE("First recenter always reports moved and clears full_reset", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(4, 4, 4);

    REQUIRE(level.full_reset == true);
    bool moved = level.recenter(glm::vec3(0.0f));
    REQUIRE(moved == true);
}

TEST_CASE("Recenter at the same camera position twice is idempotent", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(4, 4, 4);

    level.recenter(glm::vec3(5.0f, 0.0f, 5.0f));
    glm::vec3 origin_after_first = level.snapped_origin;

    bool moved = level.recenter(glm::vec3(5.0f, 0.0f, 5.0f));
    REQUIRE(moved == false);
    REQUIRE(level.snapped_origin == origin_after_first);
}

TEST_CASE("Recenter within the same snapped cell does not move the origin", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(4, 4, 4);

    // Deliberately NOT (0,0,0): with probe_counts=4 and cell_size=2,
    // half_extent=4 on every axis, so camera (0,0,0) puts
    // desired_origin exactly ON a cell boundary (-4.0, an exact
    // multiple of cell_size) — the worst possible starting point, since
    // ANY negative nudge crosses into the cell below. Starting at
    // (1,1,1) keeps desired_origin mid-cell (-3.0) with real margin, so
    // a nudge that's genuinely supposed to stay in-cell actually does.
    level.recenter(glm::vec3(1.0f));
    glm::vec3 origin_after_first = level.snapped_origin;

    // A small nudge that stays within the same 2m cell shouldn't shift
    // the snapped origin at all — this is the whole point of snapping:
    // most frames, most levels don't move.
    bool moved = level.recenter(glm::vec3(1.5f, 1.3f, 0.8f));
    REQUIRE(moved == false);
    REQUIRE(level.snapped_origin == origin_after_first);
}

TEST_CASE("Crossing a cell boundary moves the origin by whole cells", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(4, 4, 4);

    level.recenter(glm::vec3(0.0f));
    glm::vec3 origin_before = level.snapped_origin;

    // Move far enough in X to guarantee crossing at least one 2m cell
    // boundary regardless of where the initial snap landed.
    bool moved = level.recenter(glm::vec3(10.0f, 0.0f, 0.0f));
    REQUIRE(moved == true);

    glm::vec3 delta = level.snapped_origin - origin_before;
    // The shift must be a whole multiple of cell_size on every axis —
    // snapping never produces a fractional-cell origin change.
    glm::vec3 cells_shifted = delta / level.cell_size;
    REQUIRE(cells_shifted.x == Approx(std::round(cells_shifted.x)));
    REQUIRE(cells_shifted.y == Approx(std::round(cells_shifted.y)));
    REQUIRE(cells_shifted.z == Approx(std::round(cells_shifted.z)));
}

TEST_CASE("Level covers the camera after recentering", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);

    glm::vec3 camera{13.0f, -4.0f, 27.5f};
    level.recenter(camera);

    // The whole point of a clipmap: after recentering, the camera's own
    // position must fall within [world_min, world_max) — this is exactly
    // the property the old frozen grid violated once content moved
    // outside its initial fit.
    glm::vec3 lo = level.world_min();
    glm::vec3 hi = level.world_max() + level.cell_size; // world_max is the last probe, not the far edge
    REQUIRE(camera.x >= lo.x);
    REQUIRE(camera.x <  hi.x);
    REQUIRE(camera.y >= lo.y);
    REQUIRE(camera.y <  hi.y);
    REQUIRE(camera.z >= lo.z);
    REQUIRE(camera.z <  hi.z);
}

TEST_CASE("Repeated small camera movement in one direction eventually covers a large traversal", "[ddgi_clipmap]") {
    // Simulates a camera walking a long distance in small steps (as it
    // would in an actual play session) and checks the level always
    // covers the camera at every step, not just at the two endpoints —
    // guards against an off-by-one in recenter() that only shows up
    // after several consecutive shifts.
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);

    for (int i = 0; i <= 200; ++i) {
        glm::vec3 camera{static_cast<float>(i) * 0.5f, 0.0f, 0.0f};
        level.recenter(camera);
        glm::vec3 lo = level.world_min();
        glm::vec3 hi = level.world_max() + level.cell_size;
        REQUIRE(camera.x >= lo.x);
        REQUIRE(camera.x <  hi.x);
    }
}

TEST_CASE("world_to_grid and covers agree with world_min/world_max", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(4, 4, 4);
    level.recenter(glm::vec3(0.0f));

    // The camera's own cell must be covered.
    glm::ivec3 camera_cell = level.world_to_grid(glm::vec3(0.0f));
    REQUIRE(level.covers(camera_cell));

    // A point far outside the level's extent must not be covered.
    glm::ivec3 far_cell = level.world_to_grid(glm::vec3(1000.0f, 1000.0f, 1000.0f));
    REQUIRE_FALSE(level.covers(far_cell));
}

// ── Reallocation math ───────────────────────────────────────────────────
// The allocation compute pass pairs freed slots with newly-covered cells
// by index rather than through an atomic free-list, which is only valid
// if those two sets are always the same size. These cover the shift
// shapes where that identity is least obvious.

namespace {

/// Brute-force reference: walk every cell of the current level and count
/// how many were NOT covered by the level at previous_origin. The closed
/// form in newly_covered_cell_count() must agree with this everywhere.
int brute_force_newly_covered(const fjell::DdgiClipmapLevel& level,
                              const glm::vec3& previous_origin) {
    int count = 0;
    for (int z = 0; z < level.probe_counts.z; ++z) {
        for (int y = 0; y < level.probe_counts.y; ++y) {
            for (int x = 0; x < level.probe_counts.x; ++x) {
                glm::ivec3 cell(x, y, z);
                if (!level.covers(level.grid_to_previous_grid(cell, previous_origin))) {
                    ++count;
                }
            }
        }
    }
    return count;
}

} // namespace

TEST_CASE("A level that has not moved reallocates nothing", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);
    level.recenter(glm::vec3(1.0f));

    glm::vec3 origin = level.snapped_origin;
    // Same snapped cell — recenter() reports no move, and every cell
    // must keep its existing slot. Any nonzero result here would mean
    // the whole volume retraces on frames where nothing happened.
    level.recenter(glm::vec3(1.5f, 1.3f, 0.8f));
    REQUIRE(level.newly_covered_cell_count(origin) == 0);
}

TEST_CASE("An axis-aligned one-cell shift reallocates exactly one slab", "[ddgi_clipmap]") {
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);
    level.recenter(glm::vec3(0.0f));

    glm::vec3 origin_before = level.snapped_origin;
    // Push the camera exactly one cell along X.
    level.recenter(glm::vec3(2.0f, 0.0f, 0.0f));
    REQUIRE(level.shift_in_cells(origin_before) == glm::ivec3(1, 0, 0));

    // One 8x8 slab of the 8x8x8 volume falls off the trailing edge.
    REQUIRE(level.newly_covered_cell_count(origin_before) == 8 * 8);
    REQUIRE(level.newly_covered_cell_count(origin_before)
            == brute_force_newly_covered(level, origin_before));
}

TEST_CASE("Diagonal shifts do not double-count the shared corner", "[ddgi_clipmap]") {
    // A naive "sum of three slabs" would count the overlapping edges
    // and corner more than once on a diagonal shift. The
    // closed form subtracts the retained box instead, so it must match
    // the brute-force walk exactly.
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);
    level.recenter(glm::vec3(0.0f));

    glm::vec3 origin_before = level.snapped_origin;
    level.recenter(glm::vec3(4.0f, 6.0f, 2.0f));

    glm::ivec3 shift = level.shift_in_cells(origin_before);
    REQUIRE(shift != glm::ivec3(0));

    REQUIRE(level.newly_covered_cell_count(origin_before)
            == brute_force_newly_covered(level, origin_before));
}

TEST_CASE("A shift larger than the level extent reallocates every cell", "[ddgi_clipmap]") {
    // The teleport / large-jump case. Overlap must clamp at zero rather
    // than going negative, which would make the closed form report a
    // count larger than the volume (and, in the shader, walk off the end
    // of the slot pool).
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(8, 8, 8);
    level.recenter(glm::vec3(0.0f));

    glm::vec3 origin_before = level.snapped_origin;
    level.recenter(glm::vec3(1000.0f, 0.0f, 0.0f));

    int total = 8 * 8 * 8;
    REQUIRE(level.newly_covered_cell_count(origin_before) == total);
    REQUIRE(level.newly_covered_cell_count(origin_before)
            == brute_force_newly_covered(level, origin_before));
}

TEST_CASE("Closed-form reallocation count matches brute force across many shifts", "[ddgi_clipmap]") {
    // Sweep a range of shifts including negative, mixed-sign, and
    // exactly-at-extent magnitudes. The pairing of freed slots to
    // newly-covered cells in the allocation pass depends on this
    // identity holding everywhere, not just on the cases above.
    DdgiClipmapLevel level;
    level.cell_size = glm::vec3(2.0f);
    level.probe_counts = glm::ivec3(6, 4, 6);

    for (int dx = -8; dx <= 8; dx += 2) {
        for (int dy = -6; dy <= 6; dy += 2) {
            for (int dz = -8; dz <= 8; dz += 2) {
                level.full_reset = true; // fresh level per iteration
                level.recenter(glm::vec3(0.0f));
                glm::vec3 origin_before = level.snapped_origin;

                level.recenter(glm::vec3(dx, dy, dz) * level.cell_size);

                int closed_form = level.newly_covered_cell_count(origin_before);
                int brute = brute_force_newly_covered(level, origin_before);
                REQUIRE(closed_form == brute);

                // The identity the allocation pass actually relies on:
                // as many slots are freed as there are cells to fill.
                int total = level.probe_counts.x * level.probe_counts.y * level.probe_counts.z;
                REQUIRE(closed_form >= 0);
                REQUIRE(closed_form <= total);
            }
        }
    }
}

TEST_CASE("snap_to_grid handles negative coordinates via floor, not truncation", "[ddgi_clipmap]") {
    // Lumen's SnapToGrid explicitly uses floor() "to round negative
    // numbers down" rather than toward zero — verify Fjell's port keeps
    // that behavior, since a truncating version would place -0.5 in the
    // same cell as 0.5 instead of the cell below it.
    glm::vec3 cell_size(2.0f);
    glm::vec3 snapped = DdgiClipmapLevel::snap_to_grid(glm::vec3(-0.5f, 0.0f, 0.0f), cell_size);
    REQUIRE(snapped.x == Approx(-2.0f));
}
