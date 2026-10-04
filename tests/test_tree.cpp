#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

namespace {

// A folder with one child, as the hierarchy draws it.
struct SmallTree {
    KitHarness h;
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
            ui::button("elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    void click_at(glm::vec2 p) { h.drag(p, p); }
};

} // namespace

TEST_CASE("A tree row is 24 px, selects on a click and folds from its chevron", "[ui][kit]") {
    SmallTree t;
    REQUIRE(t.child_drawn);
    CHECK(t.h.rect("folder").height() == theme::TREE_ROW);
    // The child row starts where the folder row ends: rows sit edge to edge.
    CHECK(t.h.rect("child").min.y == t.h.rect("folder").max.y);

    const glm::vec2 min = t.h.rect("folder").min;
    const float mid_y = min.y + theme::TREE_ROW * 0.5f;
    t.click_at({min.x + 60.0f, mid_y});
    CHECK(t.folder_clicks == 1);
    CHECK(t.child_drawn);

    // The chevron sits at the start of the row: it folds and selects nothing.
    t.click_at({min.x + 4.0f + 7.0f, mid_y});
    CHECK(t.folder_clicks == 1);
    CHECK_FALSE(t.child_drawn);
}

TEST_CASE("A row that folds on a click folds from anywhere on it, and is clicked", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    bool child_drawn = false;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##files")) {
            const auto folder = ui::tree_row({.id = "scripts", .name = "scripts", .has_children = true,
                                              .starts_open = false, .fold_on_click = true});
            h.mark("folder");
            if (folder.clicked) ++clicks;
            child_drawn = folder.open;
            if (folder.open) (void)ui::tree_row({.id = "coin", .name = "coin.cpp", .depth = 1});
        }
    });
    h.step(2);
    REQUIRE_FALSE(child_drawn);
    const auto folder = h.rect("folder");
    h.drag({folder.min.x + 80.0f, folder.centre().y}, {folder.min.x + 80.0f, folder.centre().y});
    h.step();
    CHECK(child_drawn);
    CHECK(clicks == 1);
    h.drag({folder.min.x + 80.0f, folder.centre().y}, {folder.min.x + 80.0f, folder.centre().y});
    h.step();
    CHECK_FALSE(child_drawn);
}

TEST_CASE("A search opens a folded row without unfolding it for good", "[ui][kit]") {
    SmallTree t;
    const glm::vec2 min = t.h.rect("folder").min;
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
    t.h.press(ui::Key::Backspace);
    t.h.type("Harbour");
    t.h.press(ui::Key::Enter);
    REQUIRE(t.renamed.has_value());
    CHECK(*t.renamed == "Harbour");
    CHECK_FALSE(t.rename.editing());

    t.renamed.reset();
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ui::Key::Backspace);
    t.h.type("Pier");
    t.h.click("elsewhere");
    REQUIRE(t.renamed.has_value());
    CHECK(*t.renamed == "Pier");

    t.renamed.reset();
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ui::Key::Backspace);
    t.h.type("Nope");
    t.h.press(ui::Key::Escape);
    CHECK_FALSE(t.renamed.has_value());
    CHECK_FALSE(t.rename.editing());
}

TEST_CASE("A drop over a row lands in front of it, into it, or after it, by height", "[ui][kit]") {
    KitHarness h;
    ui::DropPlace place = ui::DropPlace::Into;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            (void)ui::tree_row({.id = "1", .name = "Docks"});
            h.mark("row");
            place = ui::drop_place();
        }
    });
    h.step(2);
    const glm::vec2 min = h.rect("row").min;
    auto hover = [&](float fraction) {
        h.move_to({min.x + 40.0f, min.y + theme::TREE_ROW * fraction});
        return place;
    };
    CHECK(hover(0.1f) == ui::DropPlace::Before);
    CHECK(hover(0.5f) == ui::DropPlace::Into);
    CHECK(hover(0.9f) == ui::DropPlace::After);
}

TEST_CASE("A tree given a text size scales its rows with it", "[ui][tree]") {
    KitHarness h;
    float plain = 0.0f;
    float sized = 0.0f;
    float after = 0.0f;
    h.set_ui([&] {
        {
            ui::Tree tree("##plain");
            (void)ui::tree_row({.id = "a", .name = "Plain"});
            plain = ui::item_max().y - ui::item_min().y;
        }
        {
            // Twice the text size around it: twice the row.
            ui::Tree tree("##sized", ui::font_size() * 2.0f);
            (void)ui::tree_row({.id = "b", .name = "Sized"});
            sized = ui::item_max().y - ui::item_min().y;
        }
        {
            // The size goes with the tree that asked for it.
            ui::Tree tree("##after");
            (void)ui::tree_row({.id = "c", .name = "After"});
            after = ui::item_max().y - ui::item_min().y;
        }
    });
    h.step(2);
    CHECK(plain == ftk::theme::TREE_ROW);
    CHECK(sized == ftk::theme::TREE_ROW * 2.0f);
    CHECK(after == ftk::theme::TREE_ROW);
}

TEST_CASE("A row can start folded; rows start open by default", "[ui][tree]") {
    KitHarness h;
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

TEST_CASE("Rows in view draws only the rows its region shows, and the region scrolls over them all", "[ui][kit]") {
    KitHarness h;
    constexpr std::size_t COUNT = 1000;
    constexpr float SHOWN = 5.0f;
    std::optional<std::size_t> reveal;
    std::size_t first = 0;
    std::size_t drawn = 0;
    float scroll_max = 0.0f;
    h.set_ui([&] {
        if (auto list = ui::Region("##list", {200.0f, SHOWN * theme::TREE_ROW})) {
            if (auto tree = ui::Tree("##rows")) {
                const ui::RowsInView rows(COUNT, std::exchange(reveal, std::nullopt));
                first = rows.first();
                drawn = 0;
                for (std::size_t i = rows.first(); i < rows.end(); ++i) {
                    const std::string id = std::to_string(i);
                    (void)ui::tree_row({.id = id.c_str(), .name = id});
                    ++drawn;
                }
            }
            scroll_max = ui::scroll_max_y();
        }
        // The region, where it is on screen.
        h.mark("list");
    });
    h.step(2);
    CHECK(first == 0);
    CHECK(drawn <= static_cast<std::size_t>(SHOWN) + 1);
    // The room of every row, less the screenful shown.
    CHECK(scroll_max >= (static_cast<float>(COUNT) - SHOWN) * theme::TREE_ROW);

    // A row far down, revealed, is drawn wholly inside the region.
    reveal = 500;
    h.step(3);
    REQUIRE(first <= 500);
    REQUIRE(500 < first + drawn);
    const auto* row = h.find("500", ftk::ui::core::Role::TreeItem);
    REQUIRE(row != nullptr);
    CHECK(row->rect.min.y >= h.rect("list").min.y);
    CHECK(row->rect.max.y <= h.rect("list").max.y);
}
