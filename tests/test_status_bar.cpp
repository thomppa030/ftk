#include "kit_harness.hpp"

#include "ftk/ui/kit/status_bar.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("The status bar sits along the bottom and its button reports a click", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            if (ui::status_button("C", "Content", "Ctrl Space", false)) ++clicks;
            h.mark("content");
        }
    });
    h.step(2);
    CHECK_THAT(h.rect("content").min.y, Catch::Matchers::WithinAbs(h.size().y - ftk::theme::STATUS_BAR, 0.5));
    CHECK_THAT(h.rect("content").max.y, Catch::Matchers::WithinAbs(h.size().y, 0.5));
    h.click("content");
    CHECK(clicks == 1);
}

TEST_CASE("Items after right() end against the status bar's right edge", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_item(nullptr, "harbour");
            h.mark("left");
            bar.right();
            ui::status_item(nullptr, "6.94 ms");
            h.mark("right");
        }
    });
    // The right-hand group is placed by the width it measured the frame before.
    h.step(3);
    CHECK(h.rect("left").min.x < 20.0f);
    CHECK_THAT(h.rect("right").max.x, Catch::Matchers::WithinAbs(h.size().x - ftk::theme::GAP_S, 1.0));
}
