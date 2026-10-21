#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("The asset header's Save, after the name, answers only while there are unsaved changes", "[ui][kit]") {
    KitHarness h;
    ui::AssetHeaderSpec spec{.icon = ui::icon::file, .name = "crate_wood.fjmat", .folder = "Props / Crates",
                             .unsaved = false, .saves = "crate_wood.fjmat"};
    ui::SaveAction last = ui::SaveAction::None;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            // Save is the last thing the header draws before the editor's own actions.
            h.mark("save");
            header.begin_actions();
            const auto action = header.finish();
            if (action != ui::SaveAction::None) last = action;
        }
    });
    h.step(3);
    h.click("save");
    CHECK(last == ui::SaveAction::None);

    spec.unsaved = true;
    h.step(2);
    h.click("save");
    CHECK(last == ui::SaveAction::Save);
}

TEST_CASE("A failed save shows under the header, and Try again saves", "[ui][kit]") {
    KitHarness h;
    ui::AssetHeaderSpec spec{.name = "hero.fjanimset", .unsaved = true, .saves = "hero.fjanimset",
                             .error = "the animset: cannot write"};
    ui::SaveAction last = ui::SaveAction::None;
    float bottom = 0.0f;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            header.begin_actions();
            const auto action = header.finish();
            if (action != ui::SaveAction::None) last = action;
            bottom = ui::window_pos().y + ui::window_size().y;
            h.mark("callout_end");
        }
    });
    h.step(3);
    // The strip grew to hold the callout.
    CHECK(bottom > 60.0f);
    // Try again is the callout's button, at its bottom left.
    h.click_at({h.rect("callout_end").min.x + 40.0f, h.rect("callout_end").max.y - 12.0f});
    CHECK(last == ui::SaveAction::Save);
}

TEST_CASE("A header naming several says which they are when its name is hovered", "[ui][kit]") {
    KitHarness h;
    const ui::AssetHeaderSpec spec{.name = "3 textures",
                                   .name_tooltip = "moss.fjtex\nbark.fjtex\nslate.fjtex",
                                   .saves = "moss.fjtex, bark.fjtex and slate.fjtex"};
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            h.mark("save");
            header.begin_actions();
            (void)header.finish();
        }
    });
    h.step(3);
    CHECK_FALSE(h.tooltip().has_value());
    // The name leads the strip, on Save's line.
    const ftk::ui::core::Rect save = h.rect("save");
    h.mark("name", {.min = {12.0f, save.min.y}, .max = {20.0f, save.max.y}});
    h.hover("name");
    CHECK(h.tooltip().has_value());
}

// An action button's label as the kit records it: its icon, then its text.
std::string action_label(const char* icon, const char* text) { return std::string(icon) + "  " + text; }

TEST_CASE("A header of a file only read says so where Revert and Save would be", "[ui][kit]") {
    KitHarness h;
    // Unsaved means nothing to a file that is never written.
    ui::AssetHeaderSpec spec{.name = "quick.fjsave", .folder = "Saved", .unsaved = true, .read_only = true};
    ui::SaveAction last = ui::SaveAction::None;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            header.begin_actions();
            const auto action = header.finish();
            if (action != ui::SaveAction::None) last = action;
        }
    });
    h.step(3);
    CHECK(h.find(action_label(ui::icon::save, "Save")) == nullptr);
    CHECK(h.find(action_label(ui::icon::revert, "Revert")) == nullptr);
    CHECK(last == ui::SaveAction::None);

    spec.read_only = false;
    h.step(2);
    CHECK(h.find(action_label(ui::icon::save, "Save")) != nullptr);
}

TEST_CASE("A notice shows under the header, and its button answers", "[ui][kit]") {
    KitHarness h;
    ui::AssetHeaderSpec spec{.name = "quick.fjsave", .read_only = true};
    spec.notice = ui::AssetHeaderNotice{.severity = ui::Severity::Error,
                                        .title = "The migration from save version 1 failed",
                                        .text = "best_from_count threw while carrying the save on to version 2.",
                                        .action_icon = ui::icon::file,
                                        .action = "Open best_from_count"};
    bool pressed = false;
    float bottom = 0.0f;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            header.begin_actions();
            (void)header.finish();
            pressed = pressed || header.notice_pressed();
            bottom = ui::window_pos().y + ui::window_size().y;
        }
    });
    h.step(3);
    // The strip grew to hold it.
    CHECK(bottom > 60.0f);
    const std::string open = action_label(ui::icon::file, "Open best_from_count");
    REQUIRE(h.find(open) != nullptr);
    h.click(open);
    CHECK(pressed);

    // Without an action it is a callout alone.
    spec.notice->action.clear();
    h.step(2);
    CHECK(h.find(open) == nullptr);
}
