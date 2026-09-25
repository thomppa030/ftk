#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "physics/physics_subsystem.hpp"
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

namespace {

// A character on a floor, stepped through the engine's physics subsystem the
// way the engine's frames step it: intents cleared before the frame, what the
// frame set kept before its steps, each step steered.
struct IntentFrames {
    Scene scene;
    Input input;
    Clock clock;
    PhysicsSubsystem physics;
    uint32_t character{0};
    uint64_t index{0};

    IntentFrames() {
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
        physics.notify_play_start(TickContext{.scene = scene, .input = input, .clock = clock, .dt = 0.0f});
        for (int i = 0; i < 60; ++i) frame(1, [] {}, [](int) {});
    }

    [[nodiscard]] CharacterComponent& cc() {
        return *scene.find_spatial(character)->components.get<CharacterComponent>();
    }
    [[nodiscard]] float x() { return scene.find_spatial(character)->transform.position.x; }

    /// One frame: `tick` sets what the frame sets, then `steps` steps, each
    /// running `in_step(i)` where a script's fixed_tick would.
    template <typename Tick, typename InStep>
    void frame(int steps, Tick tick, InStep in_step) {
        physics.clear_move_intents(scene);
        tick();
        physics.keep_frame_intents(scene);
        for (int i = 0; i < steps; ++i) {
            scene.begin_fixed_step(index);
            in_step(i);
            const FixedTickContext ctx{
                .scene = scene,
                .dt = STEP_DT,
                .time = static_cast<double>(index) * STEP_DT,
                .index = index,
                .frame_steps_left = steps - i,
                .physics = physics.world(),
                .project = nullptr,
                .resources = nullptr,
                .workers = nullptr,
            };
            physics.run_fixed(ctx);
            ++index;
        }
    }
};

} // namespace

TEST_CASE("An intent a step sets steers that step and no other", "[character_intent]") {
    // A script steering in fixed_tick for one step and then letting go moves
    // the character one step's worth, whether the next step falls in the same
    // frame or in the next one.
    IntentFrames one_frame;
    REQUIRE(one_frame.cc().ground_state == CharacterGroundState::OnGround);
    const float start_one = one_frame.x();
    one_frame.frame(2, [] {}, [&](int i) {
        if (i == 0) one_frame.cc().move_intent = {1.0f, 0.0f, 0.0f};
    });

    IntentFrames two_frames;
    const float start_two = two_frames.x();
    two_frames.frame(1, [] {}, [&](int) { two_frames.cc().move_intent = {1.0f, 0.0f, 0.0f}; });
    two_frames.frame(1, [] {}, [](int) {});

    const float one_step = two_frames.cc().max_speed * STEP_DT;
    CHECK(two_frames.x() - start_two == Approx(one_step).margin(0.01f));
    CHECK(one_frame.x() - start_one == two_frames.x() - start_two);
}

TEST_CASE("The frame's intent steers every step of the frame", "[character_intent]") {
    // Input and tick() set intent once a frame; at 30 fps that frame runs
    // two steps and the character walks on both.
    IntentFrames world;
    const float start = world.x();
    world.frame(2, [&] { world.cc().move_intent = {1.0f, 0.0f, 0.0f}; }, [](int) {});
    CHECK(world.x() - start == Approx(2.0f * world.cc().max_speed * STEP_DT).margin(0.01f));
}
