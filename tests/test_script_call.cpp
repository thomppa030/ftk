#include "scripting/script_call.hpp"
#include "scripting/timer_manager.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace fjell;

TEST_CASE("a call into the game library is caught however it ends", "[scripts][call]") {
    const ScriptCall call{.type_name = "Probe", .hook = "tick"};
    CHECK(current_script_call() == nullptr);

    const ScriptCall* seen = nullptr;
    CHECK(call_script(call, [&] { seen = current_script_call(); }));
    CHECK(seen == &call);
    CHECK(current_script_call() == nullptr);

    CHECK_FALSE(call_script(call, [] { throw std::runtime_error("broken"); }));
    CHECK_FALSE(call_script(call, [] { throw 7; }));
    CHECK(current_script_call() == nullptr);
    CHECK(describe(call) == "Probe.tick");
    CHECK(describe({.type_name = "A timer's callback"}) == "A timer's callback");
}

TEST_CASE("a Ref used with nothing there halts the game library until resumed", "[scripts][call]") {
    halt_script_calls({.message = "Probe.tick used `target` with nothing there", .node_id = 4});
    halt_script_calls({.message = "what followed"});
    CHECK(script_calls_halted());

    bool ran = false;
    CHECK_FALSE(call_script({.type_name = "Other"}, [&] { ran = true; }));
    CHECK_FALSE(ran);

    const auto report = take_missing_reference_report();
    REQUIRE(report.has_value());
    CHECK(report->message == "Probe.tick used `target` with nothing there");
    CHECK(report->node_id == 4);
    CHECK_FALSE(take_missing_reference_report().has_value());
    CHECK(script_calls_halted());

    resume_script_calls();
    CHECK(call_script({.type_name = "Other"}, [&] { ran = true; }));
    CHECK(ran);
}

TEST_CASE("a timer whose callback throws keeps its place", "[scripts][call]") {
    TimerManager timers;
    int repeats = 0;
    int after = 0;
    (void)timers.set_repeating(1.0f, [&] {
        ++repeats;
        throw std::runtime_error("callback failed");
    });
    (void)timers.set(1.0f, [&] { ++after; });

    timers.tick(1.0f);
    CHECK(repeats == 1);
    CHECK(after == 1);  // the timer after it still fired
    timers.tick(1.0f);
    CHECK(repeats == 2);  // and the repeating one is still there
}
