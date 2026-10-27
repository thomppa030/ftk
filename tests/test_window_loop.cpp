#include "ftk/app/window_loop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_CASE("A close asked for while the window loop sleeps reaches the program before the loop ends", "[app][loop]") {
    // The user closes the window while the loop sleeps. The program cancels
    // the first close to ask about its unsaved work, and lets the next go.
    bool asked = false;
    int cancelled = 0;
    std::vector<std::string> turns;
    const ftk::Result<> ran = ftk::run_window_loop({
        .take_events = [&] { turns.emplace_back("events"); },
        .update =
            [&] {
                turns.emplace_back("update");
                if (asked && cancelled == 0) {
                    asked = false;
                    ++cancelled;
                }
            },
        .closing = [&] { return asked; },
        .draw =
            [&] {
                turns.emplace_back("draw");
                return ftk::Result<>{};
            },
        .wait =
            [&] {
                turns.emplace_back("wait");
                asked = true;
            },
    });
    REQUIRE(ran);
    CHECK(cancelled == 1);
    // The frame after the first close shows the question.
    CHECK(turns == std::vector<std::string>{"events", "update", "draw", "wait", "events", "update", "draw", "wait",
                                            "events", "update"});
}

TEST_CASE("A frame that cannot be shown ends the window loop with why", "[app][loop]") {
    int drawn = 0;
    const ftk::Result<> ran = ftk::run_window_loop({
        .take_events = [] {},
        .update = {},
        .closing = [] { return false; },
        .draw = [&]() -> ftk::Result<> {
            if (++drawn == 2) return ftk::make_error("The device was lost");
            return {};
        },
        .wait = [] {},
    });
    REQUIRE_FALSE(ran);
    CHECK(ran.error() == "The device was lost");
    CHECK(drawn == 2);
}
