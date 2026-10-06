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

TEST_CASE("A state shows as a dot in its tone's colour, and a note says where it is from", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_note("use of undeclared identifier", "coin.cpp:26");
            h.mark("note");
            bar.right();
            ui::status_state(ui::StatusTone::Error, "clangd", "clangd stopped");
            h.mark("state");
        }
    });
    h.step(3);
    // The dot, in the error colour, at the state's left.
    bool dot = false;
    const uint32_t error = ftk::ui::core::pack_colour(ftk::theme::error());
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == error && h.rect("state").contains((p.min + p.max) * 0.5f)) dot = true;
        }
    }
    CHECK(dot);
    // The note's place is drawn after its text, in the disabled colour.
    bool where = false;
    const uint32_t disabled = ftk::ui::core::pack_colour(ftk::theme::text_disabled());
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == disabled && h.rect("note").contains((p.min + p.max) * 0.5f)) where = true;
        }
    }
    CHECK(where);
    // Hovered, the state says more.
    h.hover("state");
    h.step(60);
    CHECK(h.tooltip().has_value());
}

TEST_CASE("Work in the background shows its spinner before what it is", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            bar.right();
            ui::status_busy("Baking 37 textures");
            h.mark("busy");
        }
    });
    h.step(3);
    // The spinner's arc, in the accent, at the item's left; the text after it.
    const uint32_t accent = ftk::ui::core::pack_colour(ftk::theme::accent());
    bool spinner = false;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            const glm::vec2 centre = (p.min + p.max) * 0.5f;
            if (p.colour == accent && h.rect("busy").contains(centre) && centre.x < h.rect("busy").min.x + 30.0f) {
                spinner = true;
            }
        }
    }
    CHECK(spinner);
    CHECK(h.rect("busy").max.x - h.rect("busy").min.x > 100.0f);
}
