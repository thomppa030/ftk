#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/tree.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <optional>
#include <string>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

namespace {

// A folder with one child, as the hierarchy draws it.
struct SmallTree {
    ImGuiHarness h;
    ui::RenameBox rename;
    bool force_open = false;
    int folder_clicks = 0;
    bool child_drawn = false;
    std::optional<std::string> renamed;

    SmallTree() {
        h.set_ui([this] {
            if (auto tree = ui::Tree("##tree")) {
                const auto folder = ui::tree_row({.id = "1", .name = "Docks", .depth = 0, .has_children = true,
                                                  .force_open = force_open, .rename = &rename, .key = 1});
                h.mark("folder");
                if (folder.clicked) ++folder_clicks;
                if (folder.renamed) renamed = folder.renamed;
                child_drawn = false;
                if (folder.open) {
                    (void)ui::tree_row({.id = "2", .name = "Crate", .depth = 1});
                    h.mark("child");
                    child_drawn = true;
                }
            }
            ImGui::Button("elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    void click_at(ImVec2 p) { h.drag(p, p); }
};

} // namespace

TEST_CASE("A tree row is 24 px, selects on a click and folds from its chevron", "[ui][kit]") {
    SmallTree t;
    REQUIRE(t.child_drawn);
    CHECK(t.h.rect_max("folder").y - t.h.rect_min("folder").y == theme::TREE_ROW);
    // The child row starts where the folder row ends: rows sit edge to edge.
    CHECK(t.h.rect_min("child").y == t.h.rect_max("folder").y);

    const ImVec2 min = t.h.rect_min("folder");
    const float mid_y = min.y + theme::TREE_ROW * 0.5f;
    t.click_at({min.x + 60.0f, mid_y});
    CHECK(t.folder_clicks == 1);
    CHECK(t.child_drawn);

    // The chevron sits at the start of the row: it folds and selects nothing.
    t.click_at({min.x + 4.0f + 7.0f, mid_y});
    CHECK(t.folder_clicks == 1);
    CHECK_FALSE(t.child_drawn);
}

TEST_CASE("A search opens a folded row without unfolding it for good", "[ui][kit]") {
    SmallTree t;
    const ImVec2 min = t.h.rect_min("folder");
    t.click_at({min.x + 11.0f, min.y + theme::TREE_ROW * 0.5f});
    REQUIRE_FALSE(t.child_drawn);
    t.force_open = true;
    t.h.step(2);
    CHECK(t.child_drawn);
    t.force_open = false;
    t.h.step(2);
    CHECK_FALSE(t.child_drawn);
}

TEST_CASE("A rename commits on Enter or a click away, and Esc keeps the old name", "[ui][kit]") {
    SmallTree t;
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ImGuiKey_Backspace);
    t.h.type("Harbour");
    t.h.press(ImGuiKey_Enter);
    REQUIRE(t.renamed.has_value());
    CHECK(*t.renamed == "Harbour");
    CHECK_FALSE(t.rename.editing());

    t.renamed.reset();
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ImGuiKey_Backspace);
    t.h.type("Pier");
    t.h.click("elsewhere");
    REQUIRE(t.renamed.has_value());
    CHECK(*t.renamed == "Pier");

    t.renamed.reset();
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ImGuiKey_Backspace);
    t.h.type("Nope");
    t.h.press(ImGuiKey_Escape);
    CHECK_FALSE(t.renamed.has_value());
    CHECK_FALSE(t.rename.editing());
}

TEST_CASE("A drop over a row lands in front of it, into it, or after it, by height", "[ui][kit]") {
    ImGuiHarness h;
    ui::DropPlace place = ui::DropPlace::Into;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            (void)ui::tree_row({.id = "1", .name = "Docks"});
            h.mark("row");
            place = ui::drop_place();
        }
    });
    h.step(2);
    const ImVec2 min = h.rect_min("row");
    auto hover = [&](float fraction) {
        ImGui::GetIO().AddMousePosEvent(min.x + 40.0f, min.y + theme::TREE_ROW * fraction);
        h.step(1);
        return place;
    };
    CHECK(hover(0.1f) == ui::DropPlace::Before);
    CHECK(hover(0.5f) == ui::DropPlace::Into);
    CHECK(hover(0.9f) == ui::DropPlace::After);
}

TEST_CASE("A rename started from the keyboard draws no navigation ring", "[ui][kit]") {
    // The editor navigates with the keyboard, which is what turns the ring on.
    ImGuiHarness h;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ui::RenameBox rename;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            (void)ui::tree_row({.id = "1", .name = "Crate", .rename = &rename, .key = 1});
            h.mark("row");
        }
    });
    h.step(2);
    h.click("row");
    h.press(ImGuiKey_F2);
    rename.start(1, "Crate");
    h.step(3);
    REQUIRE(ImGui::GetCurrentContext()->ActiveId != 0);
    CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
}

TEST_CASE("A tree given a text size scales its rows with it", "[ui][tree]") {
    ImGuiHarness h;
    float plain = 0.0f;
    float sized = 0.0f;
    float after = 0.0f;
    h.set_ui([&] {
        {
            ui::Tree tree("##plain");
            (void)ui::tree_row({.id = "a", .name = "Plain"});
            plain = ImGui::GetItemRectSize().y;
        }
        {
            // Twice the text size around it: twice the row.
            ui::Tree tree("##sized", ImGui::GetFontSize() * 2.0f);
            (void)ui::tree_row({.id = "b", .name = "Sized"});
            sized = ImGui::GetItemRectSize().y;
        }
        {
            // The size goes with the tree that asked for it.
            ui::Tree tree("##after");
            (void)ui::tree_row({.id = "c", .name = "After"});
            after = ImGui::GetItemRectSize().y;
        }
    });
    h.step(2);
    CHECK(plain == ftk::theme::TREE_ROW);
    CHECK(sized == ftk::theme::TREE_ROW * 2.0f);
    CHECK(after == ftk::theme::TREE_ROW);
}

TEST_CASE("A row can start folded; rows start open by default", "[ui][tree]") {
    ImGuiHarness h;
    bool folded_open = true;
    bool default_open = false;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##starts")) {
            folded_open = ui::tree_row({.id = "folded", .name = "folded", .has_children = true, .starts_open = false}).open;
            default_open = ui::tree_row({.id = "plain", .name = "plain", .has_children = true}).open;
        }
    });
    h.step(2);
    CHECK_FALSE(folded_open);
    CHECK(default_open);
}
