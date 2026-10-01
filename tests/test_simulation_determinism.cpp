#include <catch2/catch_test_macros.hpp>

#include "core/clock.hpp"
#include "core/fixed_timestep.hpp"
#include "core/frame_context.hpp"
#include "core/input.hpp"
#include "ftk/base/thread_pool.hpp"
#include "physics/physics_subsystem.hpp"
#include "physics/physics_types.hpp"
#include "physics/physics_world.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace fjell;

// The simulation advances in steps of one length whatever the frames are
// doing, so the same steps must come out of any frame rate, bit for bit.
// These tests play one scene under different frame times and compare the
// whole simulated state after every step. Equality is exact: there is no
// tolerance to widen until a test passes.

namespace {

constexpr double STEP_SECONDS = 1.0 / FixedTimestep::DEFAULT_TICK_RATE;

// Everything the simulation holds after one step, as the bits of each value:
// two runs agree on a step only if every value is the same float.
using StepState = std::vector<std::uint32_t>;

void put(StepState& state, float value) {
    state.push_back(std::bit_cast<std::uint32_t>(value));
}
void put(StepState& state, glm::vec3 v) {
    put(state, v.x);
    put(state, v.y);
    put(state, v.z);
}
void put(StepState& state, glm::quat q) {
    put(state, q.x);
    put(state, q.y);
    put(state, q.z);
    put(state, q.w);
}
void put(StepState& state, const std::vector<std::pair<uint32_t, CollisionEvent>>& events) {
    state.push_back(static_cast<std::uint32_t>(events.size()));
    for (const auto& [id, event] : events) {
        state.push_back(id);
        state.push_back(event.other_id);
        put(state, event.contact_point);
        put(state, event.contact_normal);
        state.push_back(event.surface);
    }
}

// Every node's placement, every body's and character's motion, and the step's
// contact events in the order they are dispatched.
[[nodiscard]] StepState state_of(Scene& scene, const PhysicsWorld& world) {
    StepState state;
    for (const auto& obj : scene.objects()) {
        const Node3D* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;
        put(state, spatial->transform.position);
        put(state, spatial->transform.rotation);
        if (obj->components.get<RigidbodyComponent>() != nullptr) {
            put(state, world.get_linear_velocity(obj->id));
            put(state, world.get_angular_velocity(obj->id));
        }
        if (const auto* cc = obj->components.get<CharacterComponent>()) {
            put(state, cc->velocity);
            state.push_back(static_cast<std::uint32_t>(cc->ground_state));
        }
    }
    put(state, world.collision_enter_events());
    put(state, world.collision_exit_events());
    put(state, world.trigger_enter_events());
    put(state, world.trigger_exit_events());
    return state;
}

// Where two runs part: "none" when they agree on every step, otherwise the
// first step they disagree on.
[[nodiscard]] std::string first_difference(const std::vector<StepState>& a,
                                           const std::vector<StepState>& b) {
    const std::size_t shared = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (a[i] != b[i]) return "step " + std::to_string(i);
    }
    if (a.size() != b.size()) return "step " + std::to_string(shared);
    return "none";
}

uint32_t add_box(Scene& scene, glm::vec3 position, glm::vec3 half_extents,
                 std::optional<BodyType> body, float mass = 1.0f) {
    Transform at;
    at.position = position;
    const uint32_t id = scene.add_empty(at, "box");
    Node3D& node = *scene.find_spatial(id);
    auto& collider = node.components.add<ColliderComponent>();
    collider.shape = ColliderShape::Box;
    collider.half_extents = half_extents;
    if (body) {
        auto& rb = node.components.add<RigidbodyComponent>();
        rb.body_type = *body;
        rb.mass = mass;
    }
    return id;
}

// A swell of two trains crossing, as buoyancy reads it.
[[nodiscard]] FjWaterData swell() {
    WaterWave long_wave;
    long_wave.direction = glm::normalize(glm::vec2{1.0f, 0.3f});
    long_wave.wavelength = 9.0f;
    long_wave.amplitude = 0.35f;
    WaterWave short_wave;
    short_wave.direction = glm::normalize(glm::vec2{-0.2f, 1.0f});
    short_wave.wavelength = 4.0f;
    short_wave.amplitude = 0.12f;

    GerstnerContributor trains;
    trains.waves = {long_wave, short_wave};
    FjWaterData field;
    field.contributors.emplace_back(std::move(trains));
    return water::apply_wind(field, {0.0f, 0.0f});
}

// The scene a determinism run plays, built from nothing in the same order
// every time so Jolt is handed the same calls: a floor; a lift going up and
// down with a character standing on it; a crate in the character's way; a
// leaning stack of boxes that topples; and, off the floor's edge, a box
// floating on a swell. Physics runs through the engine's PhysicsSubsystem,
// buoyancy and all.
class DeterminismWorld {
public:
    DeterminismWorld() {
        (void)add_box(scene_, {0.0f, -0.5f, 0.0f}, {8.0f, 0.5f, 8.0f}, std::nullopt);
        lift_ = add_box(scene_, {0.0f, LIFT_LOW, 0.0f}, {1.5f, 0.1f, 1.5f}, BodyType::Kinematic);

        Transform standing;
        standing.position = {0.0f, LIFT_LOW + 0.15f, 0.0f};
        character_ = scene_.add_empty(standing, "character");
        scene_.find_spatial(character_)->components.add<CharacterComponent>();

        crate_ = add_box(scene_, {0.0f, 0.4f, 4.0f}, glm::vec3{0.4f}, BodyType::Dynamic, 10.0f);
        for (int i = 0; i < 6; ++i) {
            const auto level = static_cast<float>(i);
            (void)add_box(scene_, {-4.0f + (0.08f * level), 0.25f + (0.52f * level), -3.0f},
                          glm::vec3{0.25f}, BodyType::Dynamic);
        }

        Transform sea;
        sea.position = {22.0f, 0.0f, 0.0f};
        const uint32_t water_id = scene_.add_empty(sea, "water");
        auto& water = scene_.find_spatial(water_id)->components.add<WaterComponent>();
        water.size = 20.0f;
        water.effective_water = swell();
        floater_ = add_box(scene_, {22.0f, 0.6f, 0.0f}, {0.5f, 0.3f, 0.8f}, BodyType::Dynamic, 200.0f);
        scene_.find_spatial(floater_)->components.add<BuoyancyComponent>().density = 0.5f;

        scene_.compute_world_matrices();
        const TickContext start{.scene = scene_, .input = input_, .clock = clock_, .dt = 0.0f};
        physics_.notify_play_start(start);
    }

    /// One step as the engine runs it: every node's transform kept, the
    /// game's part (what a script's fixed_tick would do, from the step
    /// alone), then physics with its forces.
    void step(const FixedStepTime& step, int frame_steps_left) {
        scene_.begin_fixed_step(step.index);
        play(step);
        const FixedTickContext ctx{
            .scene = scene_,
            .dt = static_cast<float>(step.dt),
            .time = step.time,
            .index = step.index,
            .frame_steps_left = frame_steps_left,
            .physics = physics_.world(),
            .project = nullptr,
            .resources = nullptr,
            .workers = &workers_,
        };
        physics_.run_fixed(ctx);
    }

    /// Before a frame's steps, as the engine does: intents cleared, then
    /// what the frame set kept for each step to start from.
    void begin_frame_steps() {
        physics_.clear_move_intents(scene_);
        physics_.keep_frame_intents(scene_);
    }

    [[nodiscard]] StepState state() { return state_of(scene_, *physics_.world()); }
    [[nodiscard]] Scene& scene() { return scene_; }
    [[nodiscard]] const PhysicsWorld& physics() { return *physics_.world(); }
    [[nodiscard]] Node3D& node(uint32_t id) { return *scene_.find_spatial(id); }

    uint32_t lift_{0};
    uint32_t character_{0};
    uint32_t crate_{0};
    uint32_t floater_{0};

private:
    static constexpr float LIFT_LOW = 0.3f;

    // The game: the lift rises and falls on the step's time; the character
    // stands, walks off the lift into the crate, jumps, turns and stops, by
    // step number, the way a recorded input replays.
    void play(const FixedStepTime& step) {
        const auto t = static_cast<float>(step.time);
        node(lift_).transform.position.y = LIFT_LOW + (0.3f * (1.0f - std::cos(1.2f * t)));

        auto& cc = *node(character_).components.get<CharacterComponent>();
        cc.move_intent = glm::vec3{0.0f};
        if (step.index >= 90 && step.index < 160) cc.move_intent = {0.0f, 0.0f, 1.0f};
        if (step.index >= 200 && step.index < 240) cc.move_intent = {-1.0f, 0.0f, 0.0f};
        if (step.index == 130) cc.jump();
    }

    Scene scene_;
    Input input_;
    Clock clock_;
    ThreadPool workers_{2};
    PhysicsSubsystem physics_;
};

/// Play a world through frames whose lengths `next_frame` gives, as the
/// engine does, until it has taken `steps` steps: the clock limits each
/// frame, the timestep turns it into steps, and after them the scene is
/// blended and its world matrices built as for drawing. Returns the state
/// after every step, in order.
std::vector<StepState> play_frames(DeterminismWorld& world, std::size_t steps,
                                   const std::function<double()>& next_frame) {
    FixedTimestep timestep;
    timestep.reset(STEP_SECONDS, 0.0);
    Clock clock;
    clock.set_max_game_delta(static_cast<float>(timestep.max_frame_seconds()));

    std::vector<StepState> states;
    while (states.size() < steps) {
        clock.advance(static_cast<float>(next_frame()));
        const int frame_steps = timestep.advance(clock.game_delta_time());
        if (frame_steps > 0) world.begin_frame_steps();
        for (int i = 0; i < frame_steps; ++i) {
            world.step(timestep.next_step(), frame_steps - i);
            states.push_back(world.state());
        }
        if (frame_steps > 0) world.scene().end_fixed_steps(timestep.index() - 1);
        world.scene().interpolate(static_cast<float>(timestep.alpha()));
        world.scene().compute_world_matrices();
    }
    states.resize(steps);
    return states;
}

[[nodiscard]] std::function<double()> every(double seconds) {
    return [seconds] { return seconds; };
}

// Five seconds: the stack falls, the character rides the lift, walks off
// into the crate and jumps, and the floater rides several swells.
constexpr std::size_t STEPS = 300;

} // namespace

TEST_CASE("The determinism scene exercises what it is there to test", "[determinism]") {
    // A run that did nothing would agree with every other one, so first what
    // happens in it: the lift carries the character, the character walks off
    // it, things collide, and the sea lifts and drops the floater.
    DeterminismWorld world;
    const float lift_start = world.node(world.lift_).transform.position.y;
    float character_highest = world.node(world.character_).transform.position.y;
    float floater_lowest = 1e9f;
    float floater_highest = -1e9f;
    std::size_t contacts = 0;

    FixedTimestep timestep;
    timestep.reset(STEP_SECONDS, 0.0);
    for (std::size_t taken = 0; taken < STEPS;) {
        const int frame_steps = timestep.advance(STEP_SECONDS);
        for (int i = 0; i < frame_steps; ++i, ++taken) {
            world.step(timestep.next_step(), frame_steps - i);
            character_highest =
                std::max(character_highest, world.node(world.character_).transform.position.y);
            // Once it has fallen in and settled.
            if (taken >= 60) {
                const float floater_y = world.node(world.floater_).transform.position.y;
                floater_lowest = std::min(floater_lowest, floater_y);
                floater_highest = std::max(floater_highest, floater_y);
            }
            contacts += world.physics().collision_enter_events().size();
        }
    }

    CHECK(world.node(world.lift_).transform.position.y != lift_start);
    CHECK(character_highest > lift_start + 0.5f);
    CHECK(world.node(world.character_).transform.position.z > 3.0f);
    CHECK(contacts > 5);
    CHECK(floater_highest - floater_lowest > 0.1f);
}

TEST_CASE("The simulation takes the same steps at any frame rate", "[determinism]") {
    DeterminismWorld at_60;
    const std::vector<StepState> reference = play_frames(at_60, STEPS, every(1.0 / 60.0));

    for (const double fps : {30.0, 144.0, 47.0}) {
        DeterminismWorld world;
        const std::vector<StepState> run = play_frames(world, STEPS, every(1.0 / fps));
        INFO("at " << fps << " fps against 60");
        CHECK(first_difference(reference, run) == "none");
    }

    // Frames anywhere from 2 to 45 ms, and a quarter-second hitch every
    // fiftieth, which the clock cuts to four steps' worth.
    std::mt19937 rng(20260925);
    std::uniform_real_distribution<double> frame(0.002, 0.045);
    int frames = 0;
    DeterminismWorld uneven;
    const std::vector<StepState> run = play_frames(uneven, STEPS, [&] {
        return (++frames % 50 == 0) ? 0.25 : frame(rng);
    });
    INFO("under uneven frames with hitches against 60 fps");
    CHECK(first_difference(reference, run) == "none");
}

namespace {

// Fifty boxes dropped onto a floor in two layers, so dozens of contacts start
// in the same step and Jolt reports them from whichever threads it has.
[[nodiscard]] std::vector<StepState> drop_boxes(std::optional<int> worker_threads,
                                                std::size_t& most_events_in_a_step) {
    Scene scene;
    (void)add_box(scene, {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f}, std::nullopt);
    for (int layer = 0; layer < 2; ++layer) {
        for (int x = 0; x < 5; ++x) {
            for (int z = 0; z < 5; ++z) {
                const glm::vec3 at{(static_cast<float>(x) * 1.2f) - 2.4f,
                                   0.3f + (static_cast<float>(layer) * 0.7f),
                                   (static_cast<float>(z) * 1.2f) - 2.4f};
                (void)add_box(scene, at, glm::vec3{0.25f}, BodyType::Dynamic);
            }
        }
    }
    scene.compute_world_matrices();

    PhysicsWorld world(worker_threads);
    world.create_bodies(scene);

    std::vector<StepState> states;
    most_events_in_a_step = 0;
    for (std::uint64_t i = 0; i < 120; ++i) {
        scene.begin_fixed_step(i);
        world.step(static_cast<float>(STEP_SECONDS), scene);
        most_events_in_a_step = std::max(most_events_in_a_step, world.collision_enter_events().size());
        states.push_back(state_of(scene, world));
    }
    return states;
}

} // namespace

TEST_CASE("Contacts come in the same order on one thread as on many", "[determinism]") {
    // Jolt reports contacts from its worker threads as they find them; the
    // world sorts each step's events before they are dispatched, so gameplay
    // answering them runs in one order whatever the scheduling was.
    std::size_t most_single = 0;
    std::size_t most_many = 0;
    const std::vector<StepState> single = drop_boxes(0, most_single);
    const std::vector<StepState> many = drop_boxes(4, most_many);

    // The first layer lands in one step: the order has something to decide.
    REQUIRE(most_single >= 10);
    CHECK(first_difference(single, many) == "none");
}
