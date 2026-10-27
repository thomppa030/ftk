#include "kit_harness.hpp"

#include "ftk/ui/kit/other_window.hpp"
#include "ftk/ui/kit/status_bar.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>
#include <vector>

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

TEST_CASE("A window of the program's own has a bar of its own along its bottom", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_item(nullptr, "harbour");
            h.mark("main");
            bar.right();
            ui::status_item(nullptr, "6.94 ms");
            h.mark("main right");
        }
        const ui::OtherWindow other("##other", {.title = "bark.fjmat", .size = {400.0f, 300.0f}});
        if (!other) return;
        if (auto bar = ui::StatusBar("##StatusBar other")) {
            ui::status_item(nullptr, "bark");
            h.mark("other");
            bar.right();
            ui::status_item(nullptr, "12.50 ms");
            h.mark("other right");
        }
    });
    // Opened a frame after it is asked for; then each bar's right-hand group
    // is placed by the width it measured.
    h.step(5);
    CHECK_THAT(h.rect("main").min.y, Catch::Matchers::WithinAbs(h.size().y - ftk::theme::STATUS_BAR, 0.5));
    CHECK_THAT(h.rect("other").min.y, Catch::Matchers::WithinAbs(300.0f - ftk::theme::STATUS_BAR, 0.5));
    CHECK_THAT(h.rect("main right").max.x, Catch::Matchers::WithinAbs(h.size().x - ftk::theme::GAP_S, 1.0));
    CHECK_THAT(h.rect("other right").max.x, Catch::Matchers::WithinAbs(400.0f - ftk::theme::GAP_S, 1.0));
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

TEST_CASE("A pill needs no icon, and a mode's pill reads in the toggle or the accent colour", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_pill(nullptr, "Insert", ui::StatusTone::On);
            h.mark("on");
            ui::status_pill(nullptr, "Visual", ui::StatusTone::Selected);
            h.mark("selected");
            ui::status_pill(nullptr, "Normal", ui::StatusTone::Plain);
            h.mark("plain");
        }
    });
    h.step(2);
    // Whether the last frame drew something in `colour` inside `item`.
    const auto drawn_in = [&](const char* item, ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect(item).contains((p.min + p.max) * 0.5f)) return true;
            }
        }
        return false;
    };
    ftk::ui::Color toggle = ftk::theme::toggle_on();
    toggle.a = 1.0f;
    CHECK(drawn_in("on", toggle));
    CHECK(drawn_in("selected", ftk::theme::accent()));
    CHECK(drawn_in("plain", ftk::theme::text_secondary()));
    // Without an icon the text starts at the pill's own padding: no room is
    // kept for one. A plain pill has no wash, so what it draws is its text.
    const uint32_t secondary = ftk::ui::core::pack_colour(ftk::theme::text_secondary());
    float text_left = h.rect("plain").max.x;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == secondary && h.rect("plain").contains((p.min + p.max) * 0.5f)) {
                text_left = std::min(text_left, p.min.x);
            }
        }
    }
    CHECK(text_left - h.rect("plain").min.x < ftk::theme::GAP_M + 7.0f + 3.0f);
}

TEST_CASE("A message reads in its tone's colour, an error after its icon", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_message(ui::StatusTone::Error, "E486: Pattern not found: spin");
            h.mark("error");
            ui::status_message(ui::StatusTone::Plain, "search hit BOTTOM, continuing at TOP");
            h.mark("note");
        }
    });
    h.step(2);
    // What the last frame drew in `colour` inside `item`, left to right.
    const auto drawn_in = [&](const char* item, ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        std::vector<float> lefts;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect(item).contains((p.min + p.max) * 0.5f)) lefts.push_back(p.min.x);
            }
        }
        return lefts;
    };
    CHECK_FALSE(drawn_in("error", ftk::theme::error()).empty());
    CHECK(drawn_in("error", ftk::theme::text_secondary()).empty());
    CHECK_FALSE(drawn_in("note", ftk::theme::text_secondary()).empty());
    // The error's icon comes first, from the item's own padding.
    const std::vector<float> error = drawn_in("error", ftk::theme::error());
    CHECK(*std::ranges::min_element(error) - h.rect("error").min.x < ftk::theme::GAP_M + 3.0f);
}

TEST_CASE("A note in the warning tone leads with its icon in the warning colour", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_note(ui::StatusTone::Warning, "No command called files.recent", "keys.vim:12");
            h.mark("warned");
            ui::status_note("use of undeclared identifier", "coin.cpp:26");
            h.mark("plain");
        }
    });
    h.step(2);
    const auto drawn_in = [&](const char* item, ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        std::vector<float> lefts;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect(item).contains((p.min + p.max) * 0.5f)) lefts.push_back(p.min.x);
            }
        }
        return lefts;
    };
    const std::vector<float> icon = drawn_in("warned", ftk::theme::warning());
    const std::vector<float> text = drawn_in("warned", ftk::theme::text_secondary());
    REQUIRE_FALSE(icon.empty());
    REQUIRE_FALSE(text.empty());
    CHECK(*std::ranges::max_element(icon) < *std::ranges::min_element(text));
    CHECK(drawn_in("plain", ftk::theme::warning()).empty());
}

TEST_CASE("A note in the success tone leads with its icon in the success colour", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            ui::status_note(ui::StatusTone::Success, "fjedit is in your app launcher",
                            "/home/someone/.local/share/applications/fjedit.desktop");
            h.mark("done");
        }
    });
    h.step(2);
    const auto drawn_in = [&](ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        std::vector<float> lefts;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect("done").contains((p.min + p.max) * 0.5f)) lefts.push_back(p.min.x);
            }
        }
        return lefts;
    };
    const std::vector<float> icon = drawn_in(ftk::theme::success());
    const std::vector<float> text = drawn_in(ftk::theme::text_secondary());
    REQUIRE_FALSE(icon.empty());
    REQUIRE_FALSE(text.empty());
    CHECK(*std::ranges::max_element(icon) < *std::ranges::min_element(text));
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

TEST_CASE("A figure keeps its width as it changes, so the items beside it hold still", "[ui][kit]") {
    KitHarness h;
    std::string time = "6.94 ms";
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            bar.right();
            ui::status_item(nullptr, "3 errors");
            h.mark("before");
            ui::status_figure(time, "000.00 ms");
            h.mark("figure");
        }
    });
    h.step(3);
    const float before = h.rect("before").min.x;
    const float width = h.rect("figure").max.x - h.rect("figure").min.x;

    time = "116.94 ms";
    h.step(3);
    CHECK_THAT(h.rect("before").min.x, Catch::Matchers::WithinAbs(before, 0.5));
    CHECK_THAT(h.rect("figure").max.x - h.rect("figure").min.x, Catch::Matchers::WithinAbs(width, 0.5));

    // Wider than its widest, it widens.
    time = "10116.94 ms";
    h.step(3);
    CHECK(h.rect("figure").max.x - h.rect("figure").min.x > width + 1.0f);
}
