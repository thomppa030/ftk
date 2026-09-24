#include "imgui_harness.hpp"
#include "ui/kit/status_bar.hpp"
#include "ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

TEST_CASE("The status bar sits along the bottom and its button reports a click", "[ui][kit]") {
    ImGuiHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            if (ui::status_button("C", "Content", "Ctrl Space", false)) ++clicks;
            h.mark("content");
        }
    });
    h.step(2);
    const float display_h = ImGui::GetIO().DisplaySize.y;
    CHECK_THAT(h.rect_min("content").y, Catch::Matchers::WithinAbs(display_h - fjell::theme::STATUS_BAR, 0.5));
    CHECK_THAT(h.rect_max("content").y, Catch::Matchers::WithinAbs(display_h, 0.5));
    h.click("content");
    CHECK(clicks == 1);
}

TEST_CASE("Items after right() end against the status bar's right edge", "[ui][kit]") {
    ImGuiHarness h;
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
    const float display_w = ImGui::GetIO().DisplaySize.x;
    CHECK(h.rect_min("left").x < 20.0f);
    CHECK_THAT(h.rect_max("right").x, Catch::Matchers::WithinAbs(display_w - fjell::theme::GAP_S, 1.0));
}
