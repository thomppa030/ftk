#include "core/clock.hpp"
#include "core/fixed_timestep.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr double STEP = 1.0 / 60.0;

[[nodiscard]] FixedTimestep started(double start_time = 0.0) {
    FixedTimestep timestep;
    timestep.reset(STEP, start_time);
    return timestep;
}

/// Run `frames` frames of `frame_seconds` each and return the steps each one
/// ran, taking every step the frame asked for.
[[nodiscard]] std::vector<int> run(FixedTimestep& timestep, double frame_seconds, int frames) {
    std::vector<int> steps;
    for (int i = 0; i < frames; ++i) {
        const int n = timestep.advance(frame_seconds);
        for (int s = 0; s < n; ++s) (void)timestep.next_step();
        steps.push_back(n);
    }
    return steps;
}

} // namespace

TEST_CASE("At the tick rate every frame runs one step", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    for (int n : run(timestep, STEP, 120)) REQUIRE(n == 1);
    // Nothing is left over between steps, so rendering blends nothing.
    REQUIRE(timestep.alpha() == Approx(0.0).margin(1e-9));
    REQUIRE(timestep.index() == 120);
}

TEST_CASE("At half the tick rate every frame runs two steps", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    for (int n : run(timestep, 2.0 * STEP, 60)) REQUIRE(n == 2);
    REQUIRE(timestep.index() == 120);
}

TEST_CASE("Faster than the tick rate most frames run no step", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    const std::vector<int> steps = run(timestep, 1.0 / 144.0, 144);

    int total = 0;
    int idle = 0;
    for (int n : steps) {
        REQUIRE(n <= 1);
        total += n;
        if (n == 0) ++idle;
    }
    // A second of frames is a second of simulation, whatever the frame rate.
    REQUIRE(total >= 59);
    REQUIRE(total <= 60);
    REQUIRE(idle > total);
}

TEST_CASE("Between steps alpha climbs with the time carried over", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    REQUIRE(timestep.advance(STEP * 0.25) == 0);
    REQUIRE(timestep.alpha() == Approx(0.25));
    REQUIRE(timestep.advance(STEP * 0.5) == 0);
    REQUIRE(timestep.alpha() == Approx(0.75));

    // Crossing a whole step runs it and keeps only what is past it.
    REQUIRE(timestep.advance(STEP * 0.5) == 1);
    (void)timestep.next_step();
    REQUIRE(timestep.alpha() == Approx(0.25));
}

TEST_CASE("Each step starts where the last one ended", "[fixed_timestep]") {
    FixedTimestep timestep = started(10.0);
    REQUIRE(timestep.advance(3.5 * STEP) == 3);

    const FixedStepTime first = timestep.next_step();
    const FixedStepTime second = timestep.next_step();
    const FixedStepTime third = timestep.next_step();

    REQUIRE(first.index == 0);
    REQUIRE(second.index == 1);
    REQUIRE(third.index == 2);
    REQUIRE(first.time == 10.0);
    REQUIRE(second.time == Approx(10.0 + STEP));
    REQUIRE(third.time == Approx(10.0 + 2.0 * STEP));
    REQUIRE(first.dt == STEP);
    REQUIRE(timestep.time() == Approx(10.0 + 3.0 * STEP));
    REQUIRE(timestep.alpha() == Approx(0.5));
}

TEST_CASE("Simulation time is counted, not summed", "[fixed_timestep]") {
    // A million steps is about four and a half hours. Adding the step up that
    // many times would drift; the time is the count times the step instead.
    FixedTimestep timestep = started();
    constexpr int STEPS = 1'000'000;
    for (int i = 0; i < STEPS; ++i) {
        REQUIRE(timestep.advance(STEP) == 1);
        (void)timestep.next_step();
    }
    REQUIRE(timestep.index() == STEPS);
    REQUIRE(timestep.time() == static_cast<double>(STEPS) * STEP);
    REQUIRE(timestep.time() == Approx(STEPS / 60.0).epsilon(1e-12));
}

TEST_CASE("A single step runs exactly one step, whatever the frame took", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    REQUIRE(timestep.advance(STEP * 0.5) == 0);
    const double carried = timestep.alpha();

    // A long frame and a short one alike move the simulation by one step,
    // and the frame's own time is not banked for later.
    timestep.request_single_step();
    REQUIRE(timestep.advance(0.25) == 1);
    (void)timestep.next_step();
    REQUIRE(timestep.alpha() == Approx(carried));

    timestep.request_single_step();
    REQUIRE(timestep.advance(0.0) == 1);
    (void)timestep.next_step();

    REQUIRE(timestep.advance(0.0) == 0);
    REQUIRE(timestep.index() == 2);
}

TEST_CASE("A frame never runs more than the most steps a frame may run", "[fixed_timestep]") {
    // The clock limits frame time before it gets here; this is the bound
    // behind that, and what it holds back runs on the frames after.
    FixedTimestep timestep = started();
    REQUIRE(timestep.advance(10.5 * STEP) == FixedTimestep::MAX_STEPS_PER_FRAME);
    for (int s = 0; s < FixedTimestep::MAX_STEPS_PER_FRAME; ++s) (void)timestep.next_step();
    REQUIRE(timestep.advance(0.0) == FixedTimestep::MAX_STEPS_PER_FRAME);
    for (int s = 0; s < FixedTimestep::MAX_STEPS_PER_FRAME; ++s) (void)timestep.next_step();
    REQUIRE(timestep.advance(0.0) == 2);
}

TEST_CASE("Reset starts a session over", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    (void)run(timestep, 1.5 * STEP, 10);
    timestep.request_single_step();

    timestep.reset(1.0 / 30.0, 5.0);
    REQUIRE(timestep.index() == 0);
    REQUIRE(timestep.time() == 5.0);
    REQUIRE(timestep.alpha() == 0.0);
    REQUIRE(timestep.step_seconds() == 1.0 / 30.0);
    // The request made before the reset belongs to the old session.
    REQUIRE(timestep.advance(0.0) == 0);
}

TEST_CASE("The clock limits how much game time a frame counts for", "[fixed_timestep]") {
    FixedTimestep timestep = started();
    const auto limit = static_cast<float>(timestep.max_frame_seconds());

    Clock clock;
    clock.set_max_game_delta(limit);

    clock.advance(0.010f);
    REQUIRE(clock.game_delta_time() == Approx(0.010f));
    REQUIRE_FALSE(clock.game_delta_limited());

    // A hitch plays as the most a frame may count for, and the whole game
    // time moves by that much, not by the hitch.
    clock.advance(0.5f);
    REQUIRE(clock.game_delta_time() == limit);
    REQUIRE(clock.game_delta_limited());
    REQUIRE(clock.game_delta_dropped() == Approx(0.5f - limit));
    REQUIRE(clock.delta_time() == 0.5f);
    REQUIRE(clock.game_elapsed() == Approx(0.010f + limit));

    // Which is never more steps than a frame may run.
    REQUIRE(timestep.advance(clock.game_delta_time()) <= FixedTimestep::MAX_STEPS_PER_FRAME);

    clock.advance(0.010f);
    REQUIRE_FALSE(clock.game_delta_limited());
    REQUIRE(clock.game_delta_dropped() == 0.0f);
}

TEST_CASE("The limit applies to scaled game time", "[fixed_timestep]") {
    Clock clock;
    clock.set_max_game_delta(0.05f);

    // Slow motion lets a long frame through at the time it stands for.
    clock.set_time_scale(0.25f);
    clock.advance(0.1f);
    REQUIRE(clock.game_delta_time() == Approx(0.025f));
    REQUIRE_FALSE(clock.game_delta_limited());

    // Fast forward is held to the limit like anything else.
    clock.set_time_scale(4.0f);
    clock.advance(0.02f);
    REQUIRE(clock.game_delta_time() == 0.05f);
    REQUIRE(clock.game_delta_limited());

    // Paused is no time at all, and not a limited frame.
    clock.set_paused(true);
    clock.advance(0.5f);
    REQUIRE(clock.game_delta_time() == 0.0f);
    REQUIRE_FALSE(clock.game_delta_limited());

    // And without a limit nothing is cut.
    clock.set_paused(false);
    clock.set_time_scale(1.0f);
    clock.clear_max_game_delta();
    clock.advance(0.5f);
    REQUIRE(clock.game_delta_time() == 0.5f);
    REQUIRE_FALSE(clock.game_delta_limited());
}
