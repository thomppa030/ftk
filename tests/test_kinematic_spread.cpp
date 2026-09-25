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

TEST_CASE("A step longer than a sixtieth of a second takes more collision passes",
          "[kinematic_spread]") {
    CHECK(collision_steps_for(1.0f / 60.0f) == 1);
    CHECK(collision_steps_for(1.0f / 120.0f) == 1);
    CHECK(collision_steps_for(1.0f / 240.0f) == 1);
    CHECK(collision_steps_for(1.0f / 50.0f) == 2);
    CHECK(collision_steps_for(1.0f / 30.0f) == 2);
    CHECK(collision_steps_for(1.0f / 10.0f) == 6);
}
