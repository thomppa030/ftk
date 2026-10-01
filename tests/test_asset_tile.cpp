#include "ftk/ui/kit/asset_tile.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

TEST_CASE("An asset tile is 96 px wide, reports clicks, and renames in place", "[ui][kit]") {
    ImGuiHarness h;
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
        ImGui::Button("elsewhere");
        h.mark("elsewhere");
    });
    h.step(2);
    CHECK(h.rect_max("tile").x - h.rect_min("tile").x == ui::asset_tile_size().x);
    CHECK(ui::asset_tile_size().x == 96.0f);

    h.click("tile");
    CHECK(clicks == 1);

    rename.start(1, "crate.fjmesh");
    h.step(2);
    for (int i = 0; i < 16; ++i) h.press(ImGuiKey_Backspace);
    h.type("barrel.fjmesh");
    // A click away keeps the new name.
    h.click("elsewhere");
    REQUIRE(renamed.has_value());
    CHECK(*renamed == "barrel.fjmesh");
}

TEST_CASE("A tile keeps its clicks from a background drawn behind the grid after it", "[ui][kit]") {
    // The content browser draws one button over the whole grid afterwards,
    // for right-clicks on empty space and drops onto the folder.
    ImGuiHarness h;
    int tile_clicks = 0;
    int background_clicks = 0;
    h.set_ui([&] {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (ui::asset_tile({.id = "1", .name = "crate.fjmesh", .icon = "*"}).clicked) ++tile_clicks;
        h.mark("tile");
        ImGui::SetCursorScreenPos(origin);
        if (ImGui::InvisibleButton("##background", {300.0f, 300.0f})) ++background_clicks;
    });
    h.step(2);
    h.click("tile");
    CHECK(tile_clicks == 1);
    CHECK(background_clicks == 0);
}
