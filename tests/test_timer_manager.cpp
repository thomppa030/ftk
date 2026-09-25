#include "scripting/timer_manager.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

TEST_CASE("A one-shot timer fires once when its delay runs out", "[timers]") {
    TimerManager timers;
    int fired = 0;
    (void)timers.set(0.5f, [&] { ++fired; });

    timers.tick(0.4f);
    CHECK(fired == 0);
    timers.tick(0.2f);
    CHECK(fired == 1);
    timers.tick(1.0f);
    CHECK(fired == 1);
}

TEST_CASE("Cancelling everything an owner set leaves other owners' timers", "[timers]") {
    // A script destroyed mid-play takes its timers with it; the callbacks
    // point into it and must never run again.
    TimerManager timers;
    int script_a = 0;
    int script_b = 0;
    const int owner_a = 0;
    const int owner_b = 0;
    (void)timers.set(0.1f, [&] { ++script_a; }, &owner_a);
    (void)timers.set_repeating(0.1f, [&] { ++script_a; }, &owner_a);
    (void)timers.set_repeating(0.1f, [&] { ++script_b; }, &owner_b);

    timers.cancel_owned(&owner_a);
    timers.tick(0.25f);
    CHECK(script_a == 0);
    CHECK(script_b == 1);

    // Nothing owns a timer set without an owner, so no owner cancels it.
    int unowned = 0;
    (void)timers.set(0.1f, [&] { ++unowned; });
    timers.cancel_owned(nullptr);
    timers.tick(0.2f);
    CHECK(unowned == 1);
}
