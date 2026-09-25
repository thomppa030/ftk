#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float STEP_DT = 1.0f / 60.0f;

[[nodiscard]] CharacterComponent on_ground() {
    CharacterComponent c;
    c.ground_state = CharacterGroundState::OnGround;
    return c;
}

[[nodiscard]] CharacterComponent in_air() {
    CharacterComponent c;
    c.ground_state = CharacterGroundState::InAir;
    return c;
}

[[nodiscard]] float horizontal_speed(const CharacterComponent& c) {
    return std::hypot(c.velocity.x, c.velocity.z);
}

} // namespace

TEST_CASE("On the ground, full intent is full speed at once", "[character_intent]") {
    CharacterComponent c = on_ground();
    c.max_speed = 6.0f;
    c.move_intent = {1.0f, 0.0f, 0.0f};
    steer_by_intent(c, STEP_DT);
    CHECK(c.velocity.x == Approx(6.0f));
    CHECK(c.velocity.z == Approx(0.0f));

    // An intent longer than a stick can push is a full stick, not faster.
    c.move_intent = {3.0f, 0.0f, 4.0f};
    steer_by_intent(c, STEP_DT);
    CHECK(horizontal_speed(c) == Approx(6.0f));
}

TEST_CASE("On the ground, no intent stops the character", "[character_intent]") {
    // Intent is cleared each frame, so a released stick is zero intent and
    // must stop the character rather than let it coast.
    CharacterComponent c = on_ground();
    c.velocity = {4.0f, 0.0f, 0.0f};
    steer_by_intent(c, STEP_DT);
    CHECK(horizontal_speed(c) == Approx(0.0f));
}

TEST_CASE("With a ground acceleration, speed builds at that rate", "[character_intent]") {
    CharacterComponent c = on_ground();
    c.max_speed = 5.0f;
    c.ground_acceleration = 10.0f;
    c.move_intent = {0.0f, 0.0f, 1.0f};

    steer_by_intent(c, STEP_DT);
    CHECK(horizontal_speed(c) == Approx(10.0f * STEP_DT));

    // Half a second at 10 m/s² is the full 5 m/s, and it goes no faster.
    for (int i = 1; i < 60; ++i) steer_by_intent(c, STEP_DT);
    CHECK(horizontal_speed(c) == Approx(5.0f));
}

TEST_CASE("In the air, no intent keeps the momentum", "[character_intent]") {
    CharacterComponent c = in_air();
    c.velocity = {3.0f, -2.0f, 1.0f};
    steer_by_intent(c, STEP_DT);
    CHECK(c.velocity.x == 3.0f);
    CHECK(c.velocity.z == 1.0f);
    // Vertical is gravity's, never the steering's.
    CHECK(c.velocity.y == -2.0f);
}

TEST_CASE("In the air, steering is limited by air control", "[character_intent]") {
    CharacterComponent c = in_air();
    c.max_speed = 5.0f;
    c.air_acceleration = 12.0f;
    c.air_control = 0.5f;
    c.move_intent = {1.0f, 0.0f, 0.0f};
    steer_by_intent(c, STEP_DT);
    CHECK(c.velocity.x == Approx(12.0f * 0.5f * STEP_DT));
}

TEST_CASE("A jump leaves the ground at jump speed, once", "[character_intent]") {
    CharacterComponent c = on_ground();
    c.jump_speed = 4.0f;
    c.jump();
    steer_by_intent(c, STEP_DT);
    CHECK(c.velocity.y == 4.0f);
    CHECK_FALSE(c.jump_requested);

    // Asked for in the air, it is dropped rather than kept for the landing.
    CharacterComponent airborne = in_air();
    airborne.velocity.y = -1.0f;
    airborne.jump();
    steer_by_intent(airborne, STEP_DT);
    CHECK(airborne.velocity.y == -1.0f);
    CHECK_FALSE(airborne.jump_requested);
}

namespace {

// A character on a floor in a headless physics world, stepped the way the
// engine steps it.
struct JumpWorld {
    Scene scene;
    PhysicsWorld physics;
    uint32_t character{0};

    JumpWorld() {
        Transform floor_at;
        floor_at.position = {0.0f, -0.5f, 0.0f};
        const uint32_t floor = scene.add_empty(floor_at, "floor");
        auto& collider = scene.find_spatial(floor)->components.add<ColliderComponent>();
        collider.shape = ColliderShape::Box;
        collider.half_extents = {10.0f, 0.5f, 10.0f};

        Transform start;
        start.position = {0.0f, 0.05f, 0.0f};
        character = scene.add_empty(start, "character");
        scene.find_spatial(character)->components.add<CharacterComponent>();

        scene.compute_world_matrices();
        physics.create_bodies(scene);
    }

    [[nodiscard]] CharacterComponent& cc() {
        return *scene.find_spatial(character)->components.get<CharacterComponent>();
    }
    [[nodiscard]] float height() { return scene.find_spatial(character)->transform.position.y; }

    void step() {
        physics.step(STEP_DT, scene);
        scene.compute_world_matrices();
    }
};

} // namespace

TEST_CASE("A jump rises as high as its speed and the world's gravity say", "[character_intent]") {
    JumpWorld world;
    for (int i = 0; i < 120 && world.cc().ground_state != CharacterGroundState::OnGround; ++i) {
        world.step();
    }
    REQUIRE(world.cc().ground_state == CharacterGroundState::OnGround);
    const float floor_height = world.height();

    world.cc().jump_speed = 5.0f;
    world.cc().jump();
    float apex = floor_height;
    for (int i = 0; i < 240; ++i) {
        world.step();
        apex = std::max(apex, world.height());
        if (world.cc().velocity.y < 0.0f && world.height() < apex) break;
    }

    // Launched at v against g, it rises v² / 2g. The steps are discrete, so
    // it may fall short of the peak by as much as one step's travel.
    const float g = std::abs(world.physics.gravity().y);
    const float expected = 5.0f * 5.0f / (2.0f * g);
    CHECK(apex - floor_height == Approx(expected).margin(5.0f * STEP_DT));
}

TEST_CASE("A character moved by velocity ignores its intent", "[character_intent]") {
    JumpWorld world;
    world.cc().movement = CharacterMovement::Velocity;
    for (int i = 0; i < 60; ++i) world.step();

    const float x0 = world.scene.find_spatial(world.character)->transform.position.x;
    for (int i = 0; i < 30; ++i) {
        world.cc().move_intent = {1.0f, 0.0f, 0.0f};
        world.cc().velocity.x = 2.0f;
        world.step();
    }
    // Half a second at the 2 m/s the script wrote, not at the intent's 5.
    const float x1 = world.scene.find_spatial(world.character)->transform.position.x;
    CHECK(x1 - x0 == Approx(1.0f).margin(0.05f));
}
