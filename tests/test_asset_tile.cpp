#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_tile.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("An asset tile is 96 px wide, reports clicks, and renames in place", "[ui][kit]") {
    KitHarness h;
    ui::RenameBox rename;
    int clicks = 0;
    int double_clicks = 0;
    std::optional<std::string> renamed;
    h.set_ui([&] {
        const auto tile = ui::asset_tile({.id = "1", .name = "crate_large_weathered.fjmesh",
                                          .icon = "*", .rename = &rename, .key = 1});
        h.mark("tile");
        if (tile.clicked) ++clicks;
        if (tile.double_clicked) ++double_clicks;
        if (tile.renamed) renamed = tile.renamed;
        ui::button("elsewhere");
        h.mark("elsewhere");
    });
    h.step(2);
    CHECK(h.rect("tile").width() == ui::asset_tile_size().x);
    CHECK(ui::asset_tile_size().x == 96.0f);

    h.click("tile");
    CHECK(clicks == 1);

    rename.start(1, "crate.fjmesh");
    h.step(2);
    for (int i = 0; i < 16; ++i) h.press(ui::Key::Backspace);
    h.type("barrel.fjmesh");
    // A click away keeps the new name.
    h.click("elsewhere");
    REQUIRE(renamed.has_value());
    CHECK(*renamed == "barrel.fjmesh");
}

TEST_CASE("A tile keeps its clicks from a background drawn behind the grid after it", "[ui][kit]") {
    // The content browser draws one button over the whole grid afterwards,
    // for right-clicks on empty space and drops onto the folder.
    KitHarness h;
    int tile_clicks = 0;
    int background_clicks = 0;
    h.set_ui([&] {
        const glm::vec2 origin = ui::cursor();
        if (ui::asset_tile({.id = "1", .name = "crate.fjmesh", .icon = "*"}).clicked) ++tile_clicks;
        h.mark("tile");
        ui::set_cursor(origin);
        if (ui::hit_area("##background", {300.0f, 300.0f})) ++background_clicks;
    });
    h.step(2);
    h.click("tile");
    CHECK(tile_clicks == 1);
    CHECK(background_clicks == 0);
}

TEST_CASE("A tile's badge marks work going on or failed, and its note joins the tooltip", "[ui][kit]") {
    KitHarness h;
    ui::TileBadge badge = ui::TileBadge::busy;
    std::string_view note;
    h.set_ui([&] {
        (void)ui::asset_tile({.id = "1", .name = "brick.fjtex", .icon = "*", .badge = badge, .note = note});
        h.mark("tile");
    });
    h.step(2);
    const auto drawn_in = [&](ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        const ui::core::Rect tile = h.rect("tile");
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                const glm::vec2 centre = (p.min + p.max) * 0.5f;
                if (p.colour == packed && tile.contains(centre) && centre.x > tile.min.x + 60.0f && centre.y < tile.min.y + 30.0f) {
                    return true;
                }
            }
        }
        return false;
    };
    // Busy: the spinner's arc in the accent, at the picture's top right.
    CHECK(drawn_in(ftk::theme::accent()));
    CHECK_FALSE(drawn_in(ftk::theme::error()));

    badge = ui::TileBadge::failed;
    h.step(2);
    CHECK(drawn_in(ftk::theme::error()));

    h.hover("tile");
    h.step(60);
    REQUIRE(h.tooltip().has_value());
    const float one_line = h.tooltip()->height();
    note = "the image is missing";
    h.step(2);
    REQUIRE(h.tooltip().has_value());
    CHECK(h.tooltip()->height() > one_line);
}
