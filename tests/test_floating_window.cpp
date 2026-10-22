#include "kit_harness.hpp"

#include "ftk/ui/kit/floating_window.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace core = ftk::ui::core;

namespace {

// Whether the last frame drew anything in `colour` inside `rect`.
bool drew_in(const KitHarness& h, core::Rect rect, ui::Color colour) {
    const uint32_t packed = core::pack_colour(colour);
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == packed && rect.contains((p.min + p.max) * 0.5f)) return true;
        }
    }
    return false;
}

// A save's window: its title, a line of what it shows, and its bar.
struct SaveWindow {
    KitHarness h;
    bool open{true};
    bool can_play{true};
    bool titled{false};
    std::optional<ui::DialogAnswer> answered{};
    core::Rect title_bar{};

    SaveWindow() {
        h.set_ui([this] {
            if (!open) return;
            if (titled) ui::next_window_title("+", ftk::theme::accent(), "Saved");
            if (const auto shown = ui::window_named("quick.fjsave###save")) {
                title_bar = {.min = shown->pos, .max = shown->pos + glm::vec2{shown->size.x, ui::frame_height()}};
            }
            if (auto window = ui::FloatingWindow("quick.fjsave###save", &open, {400.0f, 300.0f})) {
                ui::text("Before the storm");
                h.mark("body");
                const ui::DialogAnswer said = window.bar({.confirm = "Play from this save", .cancel = "Close"},
                                                         {.can_confirm = can_play, .why_not = "Saved by a newer version"});
                if (said != ui::DialogAnswer::None) answered = said;
            }
        });
        h.step(3);
    }
};

} // namespace

TEST_CASE("A floating window's title can lead with an icon and say where its file is", "[ui][kit]") {
    SaveWindow plain;
    CHECK_FALSE(drew_in(plain.h, plain.title_bar, ftk::theme::accent()));
    CHECK_FALSE(drew_in(plain.h, plain.title_bar, ftk::theme::text_secondary()));

    SaveWindow titled;
    titled.titled = true;
    titled.h.step(2);
    CHECK(drew_in(titled.h, titled.title_bar, ftk::theme::accent()));
    CHECK(drew_in(titled.h, titled.title_bar, ftk::theme::text_secondary()));
}

TEST_CASE("A floating window's bar answers with its verb, and its cancel closes it", "[ui][kit]") {
    SaveWindow w;
    w.h.click("Play from this save");
    REQUIRE(w.answered.has_value());
    CHECK(*w.answered == ui::DialogAnswer::Confirm);
    CHECK(w.open);

    w.answered.reset();
    w.h.click("Close");
    REQUIRE(w.answered.has_value());
    CHECK(*w.answered == ui::DialogAnswer::Cancel);
    CHECK_FALSE(w.open);
}

TEST_CASE("A floating window's verb greyed out presses nothing", "[ui][kit]") {
    SaveWindow w;
    w.can_play = false;
    w.h.step(2);
    w.h.click("Play from this save");
    CHECK_FALSE(w.answered.has_value());
}

TEST_CASE("Esc closes a floating window only while it has the keyboard, and Enter presses nothing", "[ui][kit]") {
    SaveWindow w;
    // The keyboard elsewhere, in the window under it: neither key reaches it.
    w.h.click_at({10.0f, 580.0f});
    w.h.press(ui::Key::Escape);
    w.h.press(ui::Key::Enter);
    CHECK_FALSE(w.answered.has_value());
    CHECK(w.open);

    // Clicked into, it has the keyboard: Enter still presses nothing.
    w.h.click("body");
    w.h.press(ui::Key::Enter);
    CHECK_FALSE(w.answered.has_value());
    w.h.press(ui::Key::Escape);
    REQUIRE(w.answered.has_value());
    CHECK(*w.answered == ui::DialogAnswer::Cancel);
    CHECK_FALSE(w.open);
}
