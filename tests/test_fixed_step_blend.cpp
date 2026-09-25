#include "core/fixed_timestep.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>

using namespace fjell;
using Catch::Approx;

namespace {

[[nodiscard]] Transform at(glm::vec3 position) {
    Transform t;
    t.position = position;
    return t;
}

/// Run one fixed step on the scene in which `move` moves things, the way the
/// engine brackets each step.
template <typename Move>
void step(Scene& scene, std::uint64_t index, Move move) {
    scene.begin_fixed_step(index);
    move();
    scene.end_fixed_steps(index);
}

[[nodiscard]] glm::vec3 drawn_position(Scene& scene, uint32_t id) {
    scene.compute_world_matrices();
    return glm::vec3(scene.find_spatial(id)->world_matrix_[3]);
}

} // namespace

TEST_CASE("A node the last step moved is drawn blended by alpha", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Ball");
    Node3D& ball = *scene.find_spatial(id);

    step(scene, 0, [&] { ball.transform.position.x = 1.0f; });

    scene.interpolate(0.25f);
    CHECK(drawn_position(scene, id).x == Approx(0.25f));
    scene.interpolate(0.75f);
    CHECK(drawn_position(scene, id).x == Approx(0.75f));

    // The simulation's own state is untouched by what is drawn.
    CHECK(ball.transform.position.x == 1.0f);
}

TEST_CASE("A node the last step did not move is drawn where it is", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty(at({3.0f, 0.0f, 0.0f}), "Rock");

    step(scene, 0, [] {});
    scene.interpolate(0.5f);
    CHECK(drawn_position(scene, id).x == Approx(3.0f));
    CHECK_FALSE(scene.find_spatial(id)->interpolated_);
}

TEST_CASE("Only the last step's motion is blended", "[fixed_step_blend]") {
    // Two steps in one frame: the node is drawn between where the second
    // began and ended, not between where the frame began and ended.
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Ball");
    Node3D& ball = *scene.find_spatial(id);

    scene.begin_fixed_step(0);
    ball.transform.position.x = 1.0f;
    scene.begin_fixed_step(1);
    ball.transform.position.x = 2.0f;
    scene.end_fixed_steps(1);

    scene.interpolate(0.5f);
    CHECK(drawn_position(scene, id).x == Approx(1.5f));
}

TEST_CASE("A frame with no steps keeps blending the last step", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Ball");
    Node3D& ball = *scene.find_spatial(id);
    step(scene, 0, [&] { ball.transform.position.x = 1.0f; });

    for (float alpha : {0.2f, 0.4f, 0.6f}) {
        scene.interpolate(alpha);
        CHECK(drawn_position(scene, id).x == Approx(alpha));
    }
}

TEST_CASE("A node spawned during the last step appears where it was put", "[fixed_step_blend]") {
    Scene scene;
    uint32_t id = 0;
    step(scene, 0, [&] { id = scene.add_empty(at({5.0f, 0.0f, 0.0f}), "Spawned"); });

    scene.interpolate(0.5f);
    CHECK(drawn_position(scene, id).x == Approx(5.0f));
}

TEST_CASE("A teleport is not blended across", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Player");
    Node3D& player = *scene.find_spatial(id);
    step(scene, 0, [&] { player.transform.position.x = 1.0f; });
    scene.interpolate(0.5f);

    scene.teleport(player, {100.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    CHECK(drawn_position(scene, id).x == Approx(100.0f));

    // Teleported inside a step, the rest of that step blends from the arrival.
    step(scene, 1, [&] {
        scene.teleport(player, {200.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
        player.transform.position.x += 1.0f;
    });
    scene.interpolate(0.5f);
    CHECK(drawn_position(scene, id).x == Approx(200.5f));
}

TEST_CASE("A position written outside the simulation is drawn as written", "[fixed_step_blend]") {
    // The inspector, the network or a script's tick moving a node the
    // simulation also moves: a jump to a new place, never blended towards.
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Crate");
    Node3D& crate = *scene.find_spatial(id);
    step(scene, 0, [&] { crate.transform.position.x = 1.0f; });
    scene.interpolate(0.5f);

    crate.transform.position.x = 10.0f;
    CHECK(drawn_position(scene, id).x == Approx(10.0f));
}

TEST_CASE("Turning a node outside the simulation does not stop its walk blending",
          "[fixed_step_blend]") {
    // A character the simulation walks and a script turns every frame: the
    // turn is drawn as the script set it and the walk stays smooth.
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Character");
    Node3D& character = *scene.find_spatial(id);
    step(scene, 0, [&] { character.transform.position.x = 1.0f; });
    scene.interpolate(0.5f);

    const glm::quat facing = glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 1.0f, 0.0f});
    character.transform.rotation = facing;
    scene.compute_world_matrices();
    const Transform drawn = decompose_matrix(character.world_matrix_);
    CHECK(drawn.position.x == Approx(0.5f));
    CHECK(glm::abs(glm::dot(drawn.rotation, facing)) == Approx(1.0f));
}

TEST_CASE("Rotation blends along the short way round", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty({}, "Spinner");
    Node3D& spinner = *scene.find_spatial(id);
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    spinner.transform.rotation = glm::angleAxis(glm::radians(170.0f), up);

    // 170 to -170 degrees is 20 degrees through 180, not 340 back through 0.
    step(scene, 0, [&] { spinner.transform.rotation = glm::angleAxis(glm::radians(-170.0f), up); });
    scene.interpolate(0.5f);
    scene.compute_world_matrices();

    const glm::vec3 forward = glm::mat3(spinner.world_matrix_) * glm::vec3{0.0f, 0.0f, 1.0f};
    const glm::vec3 half_way = glm::angleAxis(glm::radians(180.0f), up) * glm::vec3{0.0f, 0.0f, 1.0f};
    CHECK(glm::dot(forward, half_way) == Approx(1.0f));
}

TEST_CASE("A child of a blended parent is drawn with it", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t boat = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Boat");
    const uint32_t mast = scene.add_empty(at({0.0f, 2.0f, 0.0f}), "Mast");
    scene.reparent(mast, static_cast<int32_t>(boat));
    scene.find_spatial(mast)->transform.position = {0.0f, 2.0f, 0.0f};
    scene.compute_world_matrices();

    Node3D& hull = *scene.find_spatial(boat);
    step(scene, 0, [&] { hull.transform.position.x = 4.0f; });
    scene.interpolate(0.5f);

    // What followers read before the matrix pass agrees with what it draws.
    const glm::vec3 expected{2.0f, 2.0f, 0.0f};
    const glm::vec3 before_pass{scene.interpolated_world_matrix(*scene.find_spatial(mast))[3]};
    CHECK(before_pass.x == Approx(expected.x));
    CHECK(before_pass.y == Approx(expected.y));
    const glm::vec3 drawn = drawn_position(scene, mast);
    CHECK(drawn.x == Approx(expected.x));
    CHECK(drawn.y == Approx(expected.y));
}

TEST_CASE("After play every node is drawn at its transform", "[fixed_step_blend]") {
    Scene scene;
    const uint32_t id = scene.add_empty(at({0.0f, 0.0f, 0.0f}), "Ball");
    Node3D& ball = *scene.find_spatial(id);
    step(scene, 0, [&] { ball.transform.position.x = 1.0f; });
    scene.interpolate(0.5f);

    scene.clear_interpolation();
    CHECK(drawn_position(scene, id).x == Approx(1.0f));
}

TEST_CASE("Rendering shows the time the blend shows", "[fixed_step_blend]") {
    constexpr double STEP = 1.0 / 60.0;
    FixedTimestep timestep;
    timestep.reset(STEP, 10.0);

    // Two steps and a half: drawn halfway through the second step.
    REQUIRE(timestep.advance(2.5 * STEP) == 2);
    (void)timestep.next_step();
    (void)timestep.next_step();
    CHECK(timestep.render_time() == Approx(10.0 + 1.5 * STEP));
}
