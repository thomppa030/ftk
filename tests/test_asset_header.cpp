#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

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
