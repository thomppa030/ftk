#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/popup.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A menu entry reports its click, and a disabled one doesn't", "[ui][kit]") {
    KitHarness h;
    int fits = 0;
    int creates = 0;
    h.set_ui([&] {
        if (ui::menu_item({.label = "Fit to mesh"})) ++fits;
        h.mark("fit");
        if (ui::menu_item({.label = "Create material", .enabled = false,
                           .disabled_reason = "Needs a terrain asset first"})) {
            ++creates;
        }
        h.mark("create");
    });
    h.step(2);
    h.click("fit");
    h.click("create");
    CHECK(fits == 1);
    CHECK(creates == 0);
}

TEST_CASE("A popup opens by name and closes when an entry is chosen", "[ui][kit]") {
    KitHarness h;
    int chosen = 0;
    bool open = false;
    h.set_ui([&] {
        if (ui::button("Menu")) ui::open_popup("##menu");
        h.mark("menu");
        if (auto menu = ui::Popup("##menu")) {
            if (ui::menu_item({.label = "Rename"})) ++chosen;
            h.mark("rename");
        }
        open = ui::popup_open("##menu");
    });
    h.step(2);
    CHECK_FALSE(open);
    h.click("menu");
    h.step();
    CHECK(open);
    h.click("rename");
    h.step();
    CHECK(chosen == 1);
    CHECK_FALSE(open);
}

TEST_CASE("A context menu opens on a right click on its item", "[ui][kit]") {
    KitHarness h;
    bool shown = false;
    h.set_ui([&] {
        ui::button("Track");
        h.mark("track");
        shown = false;
        if (auto menu = ui::ContextMenu("##track")) {
            shown = true;
            ui::menu_item({.label = "Delete track", .destructive = true});
        }
    });
    h.step(2);
    CHECK_FALSE(shown);
    h.click("track", ui::MouseButton::Right);
    h.step();
    CHECK(shown);
}

TEST_CASE("A menu on the bar opens on a click and runs its entries", "[ui][kit]") {
    KitHarness h;
    int saved = 0;
    h.set_ui([&] {
        if (auto bar = ui::MenuBar()) {
            const bool open = ui::begin_bar_menu("File");
            if (!open) h.mark("file");
            if (open) {
                if (ui::menu_item({.label = "Save scene", .shortcut = "Ctrl S"})) ++saved;
                h.mark("save");
                ui::end_menu();
            }
        }
    });
    h.step(2);
    h.click("file");
    h.step();
    h.click("save");
    h.step();
    CHECK(saved == 1);
}
