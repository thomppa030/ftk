#include "ftk/base/log.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

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

TEST_CASE("A panel drawn into another window among one's own lands there and takes its pointers", "[ui-core]") {
    core::Context ui;
    core::set_current(&ui);
    constexpr ftk::ui::Color MARK{0.9f, 0.1f, 0.1f, 1.0f};
    bool drew = false;
    bool hovered = false;
    glm::vec2 after{0.0f};
    const auto frame = [&](double time) -> const core::FrameOutput& {
        ui.begin_frame(time);
        ui.begin_root(1, {400.0f, 300.0f}, 1.0f);
        (void)ui.layout().next({10.0f, 10.0f});
        drew = ui.begin_other_root(2);
        if (drew) {
            ui.begin_panel("torn", {.min = {0.0f, 0.0f}, .max = {200.0f, 100.0f}});
            ui.draw_list().rect_filled({0.0f, 0.0f}, {200.0f, 100.0f}, MARK);
            hovered = ui.interact(ui.id("button"), ui.layout().next({100.0f, 24.0f})).hovered;
            ui.end_panel();
            ui.end_other_root();
        }
        // The first window goes on where it was.
        CHECK(ui.root() == 1);
        after = ui.layout().next({10.0f, 10.0f}).min;
        ui.end_root();
        ui.begin_root(2, {200.0f, 100.0f}, 2.0f);
        ui.end_root();
        return ui.end_frame();
    };
    const auto marked = [&](const core::RootOutput& root) {
        return std::ranges::any_of(root.draw->primitives,
                                   [&](const core::Primitive& p) { return p.colour == core::pack_colour(MARK); });
    };

    // Never drawn yet, it has nothing to draw into.
    (void)frame(0.0);
    CHECK_FALSE(drew);
    const core::FrameOutput& out = frame(0.1);
    CHECK(drew);
    CHECK(after.y > 10.0f);
    REQUIRE(out.roots.size() == 2);
    CHECK_FALSE(marked(out.roots[0]));
    CHECK(marked(out.roots[1]));

    ui.add_pointer_event({.kind = core::PointerEvent::Kind::Move, .root = 2, .sample = {.position = {20.0f, 20.0f}}});
    (void)frame(0.2);
    (void)frame(0.3);
    CHECK(hovered);
    core::set_current(nullptr);
}

TEST_CASE("A frame asks for windows to open and close, and hears of those the program closed", "[ui-core]") {
    core::Context ui;
    ui.begin_frame(0.0);
    ui.open_root({.root = 3, .size = {300.0f, 200.0f}, .near = 1, .at = {50.0f, 60.0f}, .title = "Inspector"});
    ui.close_root(2);
    const core::FrameOutput& out = ui.end_frame();
    REQUIRE(out.open.size() == 1);
    CHECK(out.open[0].root == 3);
    CHECK(out.open[0].size == glm::vec2{300.0f, 200.0f});
    CHECK(out.open[0].title == "Inspector");
    CHECK(out.close == std::vector<core::RootId>{2});

    ui.begin_frame(0.1);
    CHECK(ui.end_frame().open.empty());
    CHECK_FALSE(ui.root_closed(3));
    ui.closed_root(3);
    CHECK(ui.root_closed(3));
    // Asked for again, it is no longer closed.
    ui.begin_frame(0.2);
    ui.open_root({.root = 3});
    (void)ui.end_frame();
    CHECK_FALSE(ui.root_closed(3));
}
