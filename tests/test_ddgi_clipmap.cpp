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

TEST_CASE("snap_to_grid handles negative coordinates via floor, not truncation", "[ddgi_clipmap]") {
    // Lumen's SnapToGrid explicitly uses floor() "to round negative
    // numbers down" rather than toward zero — verify Fjell's port keeps
    // that behavior, since a truncating version would place -0.5 in the
    // same cell as 0.5 instead of the cell below it.
    glm::vec3 cell_size(2.0f);
    glm::vec3 snapped = DdgiClipmapLevel::snap_to_grid(glm::vec3(-0.5f, 0.0f, 0.0f), cell_size);
    REQUIRE(snapped.x == Approx(-2.0f));
}
