#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/menu.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <utility>
#include <vector>

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

} // namespace

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

TEST_CASE("A menu as data runs the entry chosen, leaves a disabled one, and opens its submenus", "[ui][kit]") {
    KitHarness h;
    int saved = 0;
    int exported = 0;
    bool grid = false;
    // Drawn after an entry, it marks that entry for the harness to click.
    const auto marking = [&h](const char* name) { return ui::MenuEntry::custom([&h, name] { h.mark(name); }); };
    h.set_ui([&] {
        if (auto bar = ui::MenuBar()) {
            const bool open = ui::begin_bar_menu("File");
            if (!open) h.mark("file");
            if (open) {
                const std::vector<ui::MenuEntry> entries{
                    {.label = "Save scene", .shortcut = "Ctrl S", .chosen = [&] { ++saved; }},
                    marking("save"),
                    ui::MenuEntry::separator(),
                    {.label = "Show grid", .checked = grid, .chosen = [&] { grid = !grid; }},
                    marking("grid"),
                    {.label = "Bake", .enabled = false, .disabled_reason = "Nothing to bake", .chosen = [&] { ++saved; }},
                    marking("bake"),
                    ui::MenuEntry::note("No project open"),
                    ui::MenuEntry::submenu(nullptr, "Export",
                                           {{.label = "As glTF", .chosen = [&] { ++exported; }}, marking("gltf")}),
                    marking("export"),
                };
                ui::menu_entries(entries);
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

    h.click("file");
    h.step();
    h.click("grid");
    h.step();
    CHECK(grid);

    h.click("file");
    h.step();
    h.click("bake");
    h.step();
    CHECK(saved == 1);
    h.click("export");
    h.step(2);
    h.click("gltf");
    h.step();
    CHECK(exported == 1);
}

TEST_CASE("A menu's entry is found by its label", "[ui][kit]") {
    KitHarness h;
    int deleted = 0;
    h.set_ui([&] {
        ui::button("Track");
        h.mark("track");
        if (auto menu = ui::ContextMenu("##track")) {
            if (ui::menu_item({.label = "Delete track", .destructive = true})) ++deleted;
        }
    });
    h.step(2);
    h.click("track", ui::MouseButton::Right);
    h.step();
    REQUIRE(h.find("Delete track") != nullptr);
    h.click("Delete track");
    CHECK(deleted == 1);
}

TEST_CASE("A submenu entry is as tall as any other entry", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        ui::menu_item({.label = "Rename"});
        h.mark("rename");
        if (ui::begin_menu(nullptr, "Add")) ui::end_menu();
        h.mark("add");
        ui::menu_item({.label = "Delete"});
        h.mark("delete");
    });
    h.step(2);
    const auto rename = h.rect("rename");
    const auto add = h.rect("add");
    const auto del = h.rect("delete");
    CHECK(add.max.y - add.min.y == rename.max.y - rename.min.y);
    // It takes the room an entry takes: the one after starts as far below.
    CHECK(del.min.y - add.min.y == add.min.y - rename.min.y);
}

TEST_CASE("A menu title takes an entry's place", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        // Three entries, then an entry, a title and an entry.
        ui::menu_item({.label = "Add"});
        h.mark("first");
        ui::menu_item({.label = "Add preset"});
        ui::menu_item({.label = "Move here"});
        h.mark("third");
        ui::menu_item({.label = "Duplicate"});
        h.mark("before");
        ui::menu_title(nullptr, "Rock_03", "Mesh");
        ui::menu_item({.label = "Frame"});
        h.mark("after");
    });
    h.step(2);
    CHECK(h.rect("after").min.y - h.rect("before").min.y == h.rect("third").min.y - h.rect("first").min.y);
}

TEST_CASE("A popup opens where it is asked to, wherever the mouse is", "[ui][kit]") {
    KitHarness h;
    bool open_it = false;
    h.set_ui([&] {
        if (std::exchange(open_it, false)) ui::open_popup("##menu", {300.0f, 200.0f});
        if (auto menu = ui::Popup("##menu")) ui::menu_item({.label = "Add"});
    });
    h.move_to({40.0f, 30.0f});
    h.step(2);
    open_it = true;
    h.step(3);
    REQUIRE(h.popup());
    CHECK(h.popup()->min == glm::vec2{300.0f, 200.0f});
}


TEST_CASE("A menu entry's icon takes its kind's colour, and its detail follows the label dimmed", "[ui][kit]") {
    KitHarness h;
    const ui::Color hue{0.49f, 0.69f, 0.87f, 1.0f};
    h.set_ui([&] {
        (void)ui::menu_item({.icon = ui::icon::checked, .icon_colour = hue, .label = "crate.fjmesh", .detail = "Model"});
        h.mark("with");
        (void)ui::menu_item({.icon = ui::icon::checked, .label = "crate.fjmesh"});
        h.mark("without");
    });
    h.step(2);
    const core::Rect with = h.rect("with");
    const core::Rect without = h.rect("without");
    // The icon column, left of the label.
    const core::Rect with_icon{.min = with.min, .max = {with.min.x + 30.0f, with.max.y}};
    const core::Rect without_icon{.min = without.min, .max = {without.min.x + 30.0f, without.max.y}};
    CHECK(drew_in(h, with_icon, hue));
    CHECK_FALSE(drew_in(h, without_icon, hue));
    CHECK(drew_in(h, without_icon, ftk::theme::text()));
    // The detail after the label, in the secondary colour; none without it.
    CHECK(drew_in(h, with, ftk::theme::text_secondary()));
    CHECK_FALSE(drew_in(h, without, ftk::theme::text_secondary()));
}

TEST_CASE("A submenu entry's icon takes its kind's colour as an entry's does", "[ui][kit]") {
    KitHarness h;
    const ui::Color hue{0.49f, 0.69f, 0.87f, 1.0f};
    h.set_ui([&] {
        if (ui::begin_menu(ui::icon::checked, "Shader (.fjsl)", true, hue)) ui::end_menu();
        h.mark("with");
        if (ui::begin_menu(ui::icon::checked, "Create new")) ui::end_menu();
        h.mark("without");
    });
    h.step(2);
    const core::Rect with = h.rect("with");
    const core::Rect without = h.rect("without");
    const core::Rect with_icon{.min = with.min, .max = {with.min.x + 30.0f, with.max.y}};
    const core::Rect without_icon{.min = without.min, .max = {without.min.x + 30.0f, without.max.y}};
    CHECK(drew_in(h, with_icon, hue));
    CHECK_FALSE(drew_in(h, without_icon, hue));
    CHECK(drew_in(h, without_icon, ftk::theme::text()));
    // The label, after the icon column, keeps the text colour whatever the icon's.
    const core::Rect with_label{.min = {with.min.x + 30.0f, with.min.y}, .max = {with.min.x + 120.0f, with.max.y}};
    CHECK(drew_in(h, with_label, ftk::theme::text()));
    CHECK_FALSE(drew_in(h, with_label, hue));
}
