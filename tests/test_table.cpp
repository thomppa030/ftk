#include "kit_harness.hpp"

#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/table.hpp"
#include "ftk/ui/kit/text.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A table's rows go under its headers, and a click on a header sorts by it", "[ui][kit]") {
    KitHarness h;
    static constexpr ui::Column COLUMNS[] = {
        {.name = "Name", .default_sort = true},
        {.name = "Size", .sizing = ui::ColumnWidth::Fixed, .width = 80.0f},
    };
    std::vector<ui::TableSort> sorts;
    h.set_ui([&] {
        if (auto table = ui::Table("##files", COLUMNS, {.sortable = true, .scroll = true})) {
            ui::table_headers();
            if (auto sort = ui::table_sort_changed()) sorts.push_back(*sort);
            for (int i = 0; i < 3; ++i) {
                ui::IdScope row(i);
                ui::table_next_row();
                ui::table_next_column();
                ui::text("terrain.fjmesh");
                h.mark("name" + std::to_string(i));
                ui::table_next_column();
                ui::text("12 KB");
                h.mark("size" + std::to_string(i));
            }
        }
    });
    h.step(2);
    REQUIRE(sorts.size() == 1);
    CHECK(sorts[0].column == 0);
    CHECK(sorts[0].ascending);
    CHECK(h.rect("size0").min.x > h.rect("name0").max.x);
    CHECK(h.rect("name1").min.y > h.rect("name0").min.y);
    CHECK(h.rect("name1").min.x == h.rect("name0").min.x);

    // The headers are a row above the first; the size column's is clicked.
    const glm::vec2 size_header{h.rect("size0").min.x + 10.0f, h.rect("size0").min.y - 8.0f};
    h.click_at(size_header);
    REQUIRE(sorts.size() == 2);
    CHECK(sorts[1].column == 1);
    CHECK(sorts[1].ascending);
    h.click_at(size_header);
    REQUIRE(sorts.size() == 3);
    CHECK(sorts[2].column == 1);
    CHECK_FALSE(sorts[2].ascending);
}

TEST_CASE("A table knows which of its rows the mouse is on", "[ui][kit]") {
    KitHarness h;
    static constexpr ui::Column COLUMNS[] = {{.name = "Property"}, {.name = "Value"}};
    std::array<bool, 2> hovered{};
    h.set_ui([&] {
        if (auto table = ui::Table("##rows", COLUMNS)) {
            for (int i = 0; i < 2; ++i) {
                ui::IdScope row(i);
                ui::table_next_row();
                ui::table_next_column();
                ui::text("width");
                h.mark("label" + std::to_string(i));
                ui::table_next_column();
                ui::text("120 px");
                hovered[static_cast<std::size_t>(i)] = ui::table_row_hovered();
            }
        }
    });
    h.step(2);
    h.hover("label1");
    CHECK_FALSE(hovered[0]);
    CHECK(hovered[1]);
}

TEST_CASE("A table's headers are told where each is", "[ui][kit]") {
    KitHarness h;
    static constexpr ui::Column COLUMNS[] = {
        {.name = "Name", .default_sort = true},
        {.name = "Size", .sizing = ui::ColumnWidth::Fixed, .width = 80.0f},
    };
    h.set_ui([&] {
        if (auto table = ui::Table("##files", COLUMNS, {.sortable = true, .scroll = true})) {
            ui::table_headers();
            (void)ui::table_sort_changed();
            ui::table_next_row();
            ui::table_next_column();
            ui::text("terrain.fjmesh");
            h.mark("name");
            ui::table_next_column();
            ui::text("12 KB");
            h.mark("size");
        }
    });
    h.step(2);
    const ui::core::SemanticNode* name = h.find("Name", ui::core::Role::Button);
    const ui::core::SemanticNode* size = h.find("Size", ui::core::Role::Button);
    REQUIRE(name != nullptr);
    REQUIRE(size != nullptr);
    // Each over its own column, in a row above the first.
    CHECK(name->rect.max.y <= h.rect("name").min.y);
    CHECK(size->rect.max.y <= h.rect("size").min.y);
    CHECK(name->rect.max.x <= size->rect.min.x + 1.0f);
    CHECK(size->rect.width() == 80.0f);
}

TEST_CASE("A table brings a row out of sight into view when asked", "[ui][kit]") {
    KitHarness h{{400.0f, 300.0f}};
    static constexpr ui::Column COLUMNS[] = {{.name = "Name"}};
    int reveal = -1;
    h.set_ui([&] {
        if (auto table = ui::Table("##files", COLUMNS, {.scroll = true})) {
            ui::table_headers();
            for (int i = 0; i < 100; ++i) {
                ui::IdScope row(i);
                ui::table_next_row();
                if (i == reveal) ui::table_reveal_row();
                ui::table_next_column();
                ui::text("row " + std::to_string(i));
                h.mark("row" + std::to_string(i));
            }
        }
    });
    h.step(2);
    const float top = h.rect("row0").min.y;
    CHECK(h.rect("row80").min.y > 300.0f);

    // Down to row 80: wholly in the window.
    reveal = 80;
    h.step(3);
    CHECK(h.rect("row80").min.y >= top);
    CHECK(h.rect("row80").max.y <= 300.0f);
    // A row in sight stays where it is.
    const float at = h.rect("row80").min.y;
    reveal = 79;
    h.step(3);
    CHECK(h.rect("row80").min.y == at);
    // Back up to row 2: under the headers, not behind them.
    reveal = 2;
    h.step(3);
    CHECK(h.rect("row2").min.y >= top);
    CHECK(h.rect("row2").max.y <= 300.0f);
}

