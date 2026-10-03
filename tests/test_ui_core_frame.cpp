#include "ftk/base/log.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_test_macros.hpp>

namespace core = ftk::ui::core;

namespace {

// Silences the core's log while it lives, for a test of a mistake the core
// reports, so a passing run stays quiet.
class QuietLog {
public:
    QuietLog() { ftk::log::core()->set_level(spdlog::level::off); }
    ~QuietLog() { ftk::log::core()->set_level(level_); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
    QuietLog(QuietLog&&) = delete;
    QuietLog& operator=(QuietLog&&) = delete;

private:
    spdlog::level::level_enum level_{ftk::log::core()->level()};
};

} // namespace

TEST_CASE("A frame lists the windows drawn and keeps the clock", "[ui-core]") {
    core::Context ui;
    ui.begin_frame(10.0);
    CHECK(ui.in_frame());
    CHECK(ui.frame() == 1);
    CHECK(ui.delta() == 0.0f);
    ui.begin_root(7, {800.0f, 600.0f}, 1.0f);
    ui.end_root();
    ui.begin_root(9, {400.0f, 300.0f}, 2.0f);
    ui.end_root();
    const core::FrameOutput& out = ui.end_frame();
    CHECK_FALSE(ui.in_frame());
    REQUIRE(out.roots.size() == 2);
    CHECK(out.roots[0].root == 7);
    CHECK(out.roots[1].root == 9);
    CHECK(out.roots[1].size.x == 400.0f);
    CHECK(out.roots[1].scale == 2.0f);

    ui.begin_frame(10.25);
    CHECK(ui.frame() == 2);
    CHECK(ui.delta() == 0.25f);
    CHECK(ui.end_frame().roots.empty());
}

TEST_CASE("A frame says when the next is wanted", "[ui-core]") {
    core::Context ui;

    SECTION("Not until there is input, when nothing asked") {
        ui.begin_frame(1.0);
        CHECK(ui.end_frame().next.waits_for_input());
    }
    SECTION("At once") {
        ui.begin_frame(1.0);
        ui.request_frame_at(3.0);
        ui.request_frame();
        const core::NextFrame next = ui.end_frame().next;
        CHECK(next.now);
        CHECK_FALSE(next.waits_for_input());
    }
    SECTION("By the earliest deadline asked for") {
        ui.begin_frame(1.0);
        ui.request_frame_at(3.0);
        ui.request_frame_at(1.15);
        ui.request_frame_at(2.0);
        const core::NextFrame next = ui.end_frame().next;
        CHECK_FALSE(next.now);
        REQUIRE(next.at.has_value());
        CHECK(*next.at == 1.15);
    }
    SECTION("At once for a deadline already reached") {
        ui.begin_frame(1.0);
        ui.request_frame_at(0.5);
        CHECK(ui.end_frame().next.now);
    }
    SECTION("Each frame asks again for what it still waits on") {
        ui.begin_frame(1.0);
        ui.request_frame();
        ui.request_frame_at(2.0);
        CHECK(ui.end_frame().next.now);
        ui.begin_frame(1.1);
        CHECK(ui.end_frame().next.waits_for_input());
    }
    SECTION("Once more after a frame that took input, to show what it changed") {
        ui.add_pointer_event({.kind = core::PointerEvent::Kind::Down});
        ui.begin_frame(1.0);
        CHECK(ui.end_frame().next.now);
        ui.begin_frame(1.1);
        CHECK(ui.end_frame().next.waits_for_input());
        ui.add_key_event({.kind = core::KeyEvent::Kind::Down, .key = ftk::ui::Key::A});
        ui.begin_frame(1.2);
        CHECK(ui.end_frame().next.now);
        ui.add_text("a");
        ui.begin_frame(1.3);
        CHECK(ui.end_frame().next.now);
        ui.begin_frame(1.4);
        CHECK(ui.end_frame().next.waits_for_input());
    }
}

TEST_CASE("Ids are one space across windows, and a window can't leak pushed ids", "[ui-core]") {
    core::Context ui;
    core::set_current(&ui);
    CHECK(&core::current() == &ui);

    ui.begin_frame(0.0);
    ui.begin_root(1, {100.0f, 100.0f}, 1.0f);
    const ftk::ui::Id in_first = ui.id("Brush size");
    // Pushed and never popped: the window's end drops it, so the next
    // window's ids don't shift.
    {
        const QuietLog quiet;
        ui.ids().push("forgotten");
        ui.end_root();
    }
    ui.begin_root(2, {100.0f, 100.0f}, 1.0f);
    CHECK(ui.id("Brush size") == in_first);
    ui.end_root();
    (void)ui.end_frame();
    core::set_current(nullptr);
}
