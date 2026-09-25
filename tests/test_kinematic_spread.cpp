#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "scene/scene.hpp"

#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float STEP_DT = 1.0f / 60.0f;

// A kinematic platform in a physics world, stepped the way the engine steps
// it: each step begins by keeping every node's transform, and the physics
// step is told how many of the frame's steps are left.
struct PlatformWorld {
    Scene scene;
    PhysicsWorld physics;
    uint64_t index{0};
    uint32_t platform{0};

    PlatformWorld() {
        platform = scene.add_empty({}, "platform");
        auto* node = scene.find_spatial(platform);
        auto& collider = node->components.add<ColliderComponent>();
        collider.shape = ColliderShape::Box;
        collider.half_extents = glm::vec3{2.0f, 0.1f, 2.0f};
        node->components.add<RigidbodyComponent>().body_type = BodyType::Kinematic;
        scene.compute_world_matrices();
        physics.create_bodies(scene);
    }

    [[nodiscard]] glm::vec3& position() { return scene.find_spatial(platform)->transform.position; }

    /// One frame of `steps` steps. `in_step(i)` runs inside step i, where a
    /// script's fixed_tick would. Returns the platform's velocity after each.
    template <typename InStep>
    std::vector<float> frame(int steps, InStep in_step) {
        std::vector<float> velocities;
        for (int i = 0; i < steps; ++i) {
            scene.begin_fixed_step(index++);
            in_step(i);
            physics.step(STEP_DT, scene, steps - i);
            velocities.push_back(physics.get_linear_velocity(platform).x);
        }
        return velocities;
    }
};

} // namespace

TEST_CASE("A kinematic body the frame moves once moves on every step of the frame",
          "[kinematic_spread]") {
    // Moved a metre in tick, before a frame of two steps: half a metre each,
    // not the whole metre on the first and a standstill on the second, which
    // would jerk whatever rides on it.
    PlatformWorld world;
    world.position().x = 1.0f;
    const std::vector<float> v = world.frame(2, [](int) {});
    CHECK(v[0] == Approx(0.5f / STEP_DT));
    CHECK(v[1] == Approx(0.5f / STEP_DT));

    // Three steps next frame: a third of the next metre each.
    world.position().x = 2.0f;
    const std::vector<float> w = world.frame(3, [](int) {});
    for (float speed : w) CHECK(speed == Approx(1.0f / 3.0f / STEP_DT).epsilon(1e-3));
}

TEST_CASE("A kinematic body moved inside a step reaches its target that step",
          "[kinematic_spread]") {
    // Moved by a script's fixed_tick, the target is the step's own: shared
    // out over the frame, the body would lag the script and catch up late.
    PlatformWorld world;
    const std::vector<float> v =
        world.frame(2, [&](int) { world.position().x += 0.5f; });
    CHECK(v[0] == Approx(0.5f / STEP_DT));
    CHECK(v[1] == Approx(0.5f / STEP_DT));
}

TEST_CASE("A frame of one step reaches the target on it", "[kinematic_spread]") {
    PlatformWorld world;
    world.position().x = 1.0f;
    const std::vector<float> v = world.frame(1, [](int) {});
    CHECK(v[0] == Approx(1.0f / STEP_DT));
}

TEST_CASE("A character on a lift the frame moves rises on every step of the frame",
          "[kinematic_spread]") {
    // A lift moved once a frame, in tick, carries whoever stands on it. At
    // two steps a frame the lift covers half of the frame's rise on each, and
    // the character has to rise with it on both: all of it on the first and
    // none on the second would jerk the character up and down at the frame
    // rate. Nothing in test_project has a moving platform, so this is the one
    // place a character riding one is checked.
    Scene scene;
    const uint32_t lift = scene.add_empty({}, "lift");
    {
        auto* node = scene.find_spatial(lift);
        auto& collider = node->components.add<ColliderComponent>();
        collider.shape = ColliderShape::Box;
        collider.half_extents = glm::vec3{2.0f, 0.1f, 2.0f};
        node->components.add<RigidbodyComponent>().body_type = BodyType::Kinematic;
    }
    Transform standing;
    standing.position = {0.0f, 0.15f, 0.0f};
    const uint32_t character = scene.add_empty(standing, "character");
    scene.find_spatial(character)->components.add<CharacterComponent>();
    scene.compute_world_matrices();

    PhysicsWorld physics;
    physics.create_bodies(scene);
    uint64_t index = 0;
    auto step = [&](int steps_left) {
        scene.begin_fixed_step(index++);
        physics.step(STEP_DT, scene, steps_left);
    };
    auto height = [&](uint32_t id) { return scene.find_spatial(id)->transform.position.y; };

    // Settle on the lift while it stands still.
    for (int i = 0; i < 60; ++i) step(1);
    REQUIRE(scene.find_spatial(character)->components.get<CharacterComponent>()->ground_state ==
            CharacterGroundState::OnGround);

    // Then half a metre a second up, set once a frame for a frame of two
    // steps. The first frame starts it from rest; after that every step.
    constexpr float SPEED = 0.5f;
    for (int frame = 0; frame < 30; ++frame) {
        scene.find_spatial(lift)->transform.position.y += SPEED * 2.0f * STEP_DT;
        for (int i = 0; i < 2; ++i) {
            const float character_before = height(character);
            step(2 - i);
            if (frame == 0) continue;
            INFO("frame " << frame << ", step " << i);
            // The node holds the frame's target; the body is what moves.
            CHECK(physics.get_linear_velocity(lift).y == Approx(SPEED).epsilon(0.01));
            CHECK(height(character) - character_before == Approx(SPEED * STEP_DT).epsilon(0.1));
        }
    }
}

TEST_CASE("A step longer than a sixtieth of a second takes more collision passes",
          "[kinematic_spread]") {
    CHECK(collision_steps_for(1.0f / 60.0f) == 1);
    CHECK(collision_steps_for(1.0f / 120.0f) == 1);
    CHECK(collision_steps_for(1.0f / 240.0f) == 1);
    CHECK(collision_steps_for(1.0f / 50.0f) == 2);
    CHECK(collision_steps_for(1.0f / 30.0f) == 2);
    CHECK(collision_steps_for(1.0f / 10.0f) == 6);
}
