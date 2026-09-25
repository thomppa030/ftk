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

TEST_CASE("A timer callback may set more timers", "[timers]") {
    // Setting a timer can grow the list the tick is walking; the callback
    // that did it is still running while it grows.
    TimerManager timers;
    int chained = 0;
    int first = 0;
    (void)timers.set(0.1f, [&] {
        ++first;
        for (int i = 0; i < 64; ++i) (void)timers.set(0.1f, [&] { ++chained; });
    });

    timers.tick(0.2f);
    CHECK(first == 1);
    // Set during this tick, they count down from the next one.
    CHECK(chained == 0);
    timers.tick(0.2f);
    CHECK(chained == 64);
}

TEST_CASE("A repeating timer may cancel itself from its own callback", "[timers]") {
    TimerManager timers;
    int fired = 0;
    TimerHandle handle;
    handle = timers.set_repeating(0.1f, [&] {
        ++fired;
        if (fired == 3) timers.cancel(handle);
    });

    for (int i = 0; i < 10; ++i) timers.tick(0.1f);
    CHECK(fired == 3);
}

TEST_CASE("A timer cancelled and its slot reused in a callback is left alone", "[timers]") {
    // The callback frees its own slot and a new timer takes it; the tick
    // that ran the callback must not treat the newcomer as the old timer.
    TimerManager timers;
    int old_fired = 0;
    int new_fired = 0;
    TimerHandle handle;
    handle = timers.set_repeating(0.1f, [&] {
        ++old_fired;
        timers.cancel(handle);
        (void)timers.set(5.0f, [&] { ++new_fired; });
    });

    timers.tick(0.1f);
    CHECK(old_fired == 1);
    timers.tick(0.1f);
    CHECK(old_fired == 1);
    CHECK(new_fired == 0);
    timers.tick(5.0f);
    CHECK(new_fired == 1);
}
