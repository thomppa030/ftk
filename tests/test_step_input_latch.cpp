#include "core/step_input_latch.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

TEST_CASE("A press on a frame that runs no step reaches the next step", "[step_input]") {
    StepInputLatch input;
    // Frame 1: pressed, but the frame runs no step (a fast frame rate).
    input.record("Jump", true, false);
    // Frame 2: held, no new edge, and this frame runs a step.
    input.record("Jump", false, false);
    input.begin_step();
    CHECK(input.pressed("Jump"));
}

TEST_CASE("A press on a frame that runs two steps reaches only the first", "[step_input]") {
    StepInputLatch input;
    input.record("Jump", true, false);
    input.begin_step();
    CHECK(input.pressed("Jump"));
    input.begin_step();
    CHECK_FALSE(input.pressed("Jump"));
}

TEST_CASE("A release and a press between two steps are both seen", "[step_input]") {
    // Tapped again before the simulation stepped: the step sees the release
    // and the new press, not one cancelling the other.
    StepInputLatch input;
    input.record("Fire", false, true);
    input.record("Fire", true, false);
    input.begin_step();
    CHECK(input.released("Fire"));
    CHECK(input.pressed("Fire"));
}

TEST_CASE("Each action's edges are its own", "[step_input]") {
    StepInputLatch input;
    input.record("Jump", true, false);
    input.record("Crouch", false, true);
    input.begin_step();
    CHECK(input.pressed("Jump"));
    CHECK_FALSE(input.released("Jump"));
    CHECK(input.released("Crouch"));
    CHECK_FALSE(input.pressed("Crouch"));
    CHECK_FALSE(input.pressed("Fire"));
}

TEST_CASE("Edges recorded during a step wait for the next one", "[step_input]") {
    // A frame's input is recorded while the previous frame's last step still
    // stands as the current one; it belongs to the next step, not that one.
    StepInputLatch input;
    input.begin_step();
    input.record("Jump", true, false);
    CHECK_FALSE(input.pressed("Jump"));
    input.begin_step();
    CHECK(input.pressed("Jump"));
}

TEST_CASE("A new session forgets the last one's input", "[step_input]") {
    StepInputLatch input;
    input.record("Jump", true, false);
    input.begin_step();
    input.record("Fire", true, false);

    input.reset();
    CHECK_FALSE(input.pressed("Jump"));
    input.begin_step();
    CHECK_FALSE(input.pressed("Fire"));
}
