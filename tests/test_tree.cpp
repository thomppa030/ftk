#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/drag_drop.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tree.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("A rename's field is told as a text field named Name, with the keyboard", "[ui][kit]") {
    SmallTree t;
    t.rename.start(1, "Docks");
    t.h.step(2);
    const ui::core::SemanticNode* field = t.h.find("Name", ui::core::Role::TextField);
    REQUIRE(field != nullptr);
    CHECK(field->value == "Docks");
    CHECK(field->state.focused);
    t.h.press(ui::Key::Escape);
    t.h.step();
    CHECK(t.h.find("Name", ui::core::Role::TextField) == nullptr);
}

TEST_CASE("A rename finished by its owner gives the name as typed", "[ui][kit]") {
    SmallTree t;
    t.rename.start(1, "Docks");
    t.h.step(2);
    t.h.press(ui::Key::Backspace);
    t.h.type("Harbour");
    REQUIRE(t.rename.editing());
    const std::optional<std::string> finished = t.rename.finish();
    REQUIRE(finished.has_value());
    CHECK(*finished == "Harbour");
    CHECK_FALSE(t.rename.editing());
    CHECK_FALSE(t.rename.finish().has_value());
    t.h.step();
    CHECK_FALSE(ui::typing());

    // Unchanged, or emptied: nothing.
    t.rename.start(1, "Docks");
    t.h.step(2);
    CHECK_FALSE(t.rename.finish().has_value());
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

TEST_CASE("A row's description stands in a column after its name, in the text colour on the row shown",
          "[ui][tree]") {
    KitHarness h;
    float width = 400.0f;
    h.set_ui([&] {
        if (auto region = ui::Region("##rows", {width, 100.0f})) {
            ui::Tree tree("##names");
            (void)ui::tree_row({.id = "w", .name = "w", .description = "Save", .description_at = 80.0f});
            h.mark("short");
            (void)ui::tree_row({.id = "wq", .name = "wq", .description = "Save and close", .description_at = 80.0f,
                                .selection = ui::RowSelection::Primary});
            h.mark("shown");
        }
    });
    h.step(2);
    // Where the last frame drew in `colour` inside the row marked `row`,
    // left to right, and how far right it reached.
    const auto lefts = [&](const char* row, ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        std::vector<float> out;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect(row).contains((p.min + p.max) * 0.5f)) out.push_back(p.min.x);
            }
        }
        std::ranges::sort(out);
        return out;
    };
    const std::vector<float> plain = lefts("short", theme::text_secondary());
    REQUIRE_FALSE(plain.empty());
    // The descriptions begin together, past the names.
    const std::vector<float> shown = lefts("shown", theme::text());
    REQUIRE(shown.size() > 2);
    const auto description_left = [&](const std::vector<float>& xs) {
        return *std::ranges::find_if(xs, [&](float x) { return x >= h.rect("short").min.x + 60.0f; });
    };
    CHECK(plain.front() >= h.rect("short").min.x + 80.0f);
    CHECK(description_left(shown) == plain.front());

    // Too narrow for it all: cut short of the row's end.
    width = 120.0f;
    h.step(2);
    float right = 0.0f;
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (h.rect("shown").contains((p.min + p.max) * 0.5f)) right = std::max(right, p.max.x);
        }
    }
    CHECK(right <= h.rect("shown").max.x + 0.5f);
}

TEST_CASE("A row's tag that is code is drawn in the mono face", "[ui][tree]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto region = ui::Region("##rows", {300.0f, 100.0f})) {
            ui::Tree tree("##elements");
            (void)ui::tree_row({.id = "plain", .name = "Box", .tag = "div"});
            h.mark("plain");
            (void)ui::tree_row({.id = "code", .name = "Box", .tag = "div", .tag_is_code = true});
            h.mark("code");
        }
        // The size a row's tag is drawn at, glyphs being drawn at their size.
        const ui::FontScope mono(ui::Font::Mono, ui::font_size() * theme::SMALL_TEXT / theme::BODY_TEXT);
        ui::text("div");
        h.mark("mono");
    });
    h.step(2);
    // How far the glyphs drawn inside `mark` reach, from the first one's
    // left to the last one's right: "div" is wider in the mono face, whose
    // "i" takes as much room as any letter.
    const auto reach = [&](const char* mark, bool tag_only) {
        const uint32_t tag_colour = ftk::ui::core::pack_colour(theme::text_disabled());
        float left = 1e9f;
        float right = -1e9f;
        int count = 0;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.kind != ftk::ui::core::PrimitiveKind::Glyph) continue;
                if (tag_only && p.colour != tag_colour) continue;
                if (!h.rect(mark).contains((p.min + p.max) * 0.5f)) continue;
                left = std::min(left, p.min.x);
                right = std::max(right, p.max.x);
                ++count;
            }
        }
        REQUIRE(count == 3);
        return right - left;
    };
    const float mono = reach("mono", false);
    // Placed on whole pixels, a glyph may shift by one.
    CHECK(std::abs(reach("code", true) - mono) <= 1.0f);
    CHECK(std::abs(reach("plain", true) - mono) > 1.0f);
}

TEST_CASE("A row named in its hue draws its name in its icon's colour, and dimmed in the disabled one",
          "[ui][tree]") {
    KitHarness h;
    h.set_ui([&] {
        ui::Tree tree("##placed");
        (void)ui::tree_row({.id = "a", .name = "Lamp", .icon = "*", .hue = theme::Hue::green});
        h.mark("plain");
        (void)ui::tree_row({.id = "b", .name = "Lamp", .icon = "*", .hue = theme::Hue::green, .name_in_hue = true});
        h.mark("named");
        (void)ui::tree_row(
            {.id = "c", .name = "Lamp", .icon = "*", .hue = theme::Hue::green, .name_in_hue = true, .dimmed = true});
        h.mark("dimmed");
    });
    h.step(2);
    // How many shapes the last frame drew in `colour` inside the row marked `row`.
    const auto drawn_in = [&](const char* row, ftk::ui::Color colour) {
        const uint32_t packed = ftk::ui::core::pack_colour(colour);
        int count = 0;
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == packed && h.rect(row).contains((p.min + p.max) * 0.5f)) ++count;
            }
        }
        return count;
    };
    const ftk::ui::Color green = theme::hue(theme::Hue::green);
    const int icon = drawn_in("plain", green);
    const int name = drawn_in("plain", theme::text());
    REQUIRE(icon > 0);
    REQUIRE(name > 0);
    CHECK(drawn_in("named", theme::text()) == 0);
    CHECK(drawn_in("named", green) == icon + name);
    CHECK(drawn_in("dimmed", green) == icon);
    CHECK(drawn_in("dimmed", theme::text_disabled()) == name);
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

TEST_CASE("A row's fold reads before the row is drawn as the row draws it", "[ui][kit]") {
    KitHarness h;
    bool listed_open = false;
    bool drawn_open = false;
    bool listed_folded = true;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            listed_open = ui::tree_row_open("1");
            listed_folded = ui::tree_row_open("2", false);
            drawn_open = ui::tree_row({.id = "1", .name = "Docks", .has_children = true}).open;
            h.mark("folder");
            (void)ui::tree_row({.id = "2", .name = "Yard", .has_children = true, .starts_open = false});
        }
    });
    h.step(2);
    CHECK(listed_open);
    CHECK(drawn_open);
    CHECK_FALSE(listed_folded);

    // Folded from its chevron, it reads folded from the next frame on.
    const glm::vec2 min = h.rect("folder").min;
    const glm::vec2 chevron{min.x + 4.0f + 7.0f, min.y + theme::TREE_ROW * 0.5f};
    h.drag(chevron, chevron);
    h.step();
    CHECK_FALSE(listed_open);
    CHECK_FALSE(drawn_open);
}

TEST_CASE("An owner opens a folded row, as unfolding it from its chevron would", "[ui][kit]") {
    KitHarness h;
    bool open_it = false;
    bool listed_open = true;
    bool drawn_open = true;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            if (std::exchange(open_it, false)) ui::open_tree_row("2");
            listed_open = ui::tree_row_open("2", false);
            drawn_open = ui::tree_row({.id = "2", .name = "Yard", .has_children = true, .starts_open = false}).open;
        }
    });
    h.step(2);
    CHECK_FALSE(listed_open);
    CHECK_FALSE(drawn_open);

    open_it = true;
    h.step();
    CHECK(listed_open);
    CHECK(drawn_open);
    // It stays open, as a row unfolded by hand does.
    h.step(2);
    CHECK(drawn_open);
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

TEST_CASE("An open row's inset holds its rows at the window's spacing, and the tree goes on under it", "[ui][kit]") {
    KitHarness h;
    float spacing = 0.0f;
    float frame = 0.0f;
    float row_bottom = 0.0f;
    std::vector<float> tops;
    float after = 0.0f;
    bool open = false;
    h.set_ui([&] {
        spacing = ui::item_spacing().y;
        frame = ui::frame_height();
        tops.clear();
        if (auto tree = ui::Tree("##kept")) {
            const auto row = ui::tree_row({.id = "rack", .name = "Net Rack", .has_children = true});
            row_bottom = ui::cursor().y;
            open = row.open;
            if (row.open) {
                if (auto inset = ui::TreeInset("##rack")) {
                    for (const char* name : {"Position", "Left"}) {
                        tops.push_back(ui::cursor().y);
                        ui::button(name);
                    }
                }
            }
            after = ui::cursor().y;
            (void)ui::tree_row({.id = "gull", .name = "Gull"});
        }
    });
    h.step(3);
    REQUIRE(open);
    REQUIRE(tops.size() == 2);
    // Inside its padding, under the row.
    CHECK(tops[0] > row_bottom);
    // Its rows apart as the window's are, though the tree's rows touch.
    CHECK(tops[1] - tops[0] == frame + spacing);
    // The next row of the tree goes under it.
    CHECK(after > tops[1] + frame);
    const auto* gull = h.find("Gull", ftk::ui::core::Role::TreeItem);
    REQUIRE(gull != nullptr);
    CHECK(gull->rect.min.y >= after);

    // Folded, it holds nothing and the next row follows the row.
    h.click_at({h.find("Net Rack", ftk::ui::core::Role::TreeItem)->rect.min.x + 10.0f,
                h.find("Net Rack", ftk::ui::core::Role::TreeItem)->rect.min.y + 6.0f});
    h.step(2);
    CHECK_FALSE(open);
    CHECK(after == row_bottom);
}

TEST_CASE("A drag resting on a folded row opens it after a while, and it stays open", "[ui][kit]") {
    KitHarness h;
    bool child_drawn = false;
    h.set_ui([&] {
        ui::button("carry");
        h.mark("carry");
        if (auto drag = ui::DragSource()) drag.carry("THING", "crate");
        if (auto tree = ui::Tree("##tree")) {
            const auto yard = ui::tree_row({.id = "1", .name = "Yard", .has_children = true, .starts_open = false});
            h.mark("yard");
            child_drawn = yard.open;
            if (yard.open) (void)ui::tree_row({.id = "2", .name = "Shed", .depth = 1});
        }
    });
    h.step(2);
    const glm::vec2 from = h.rect("carry").centre();
    const glm::vec2 over = h.rect("yard").centre();
    h.move_to(from);
    h.button_down();
    h.move_to(from + glm::vec2(20.0f, 0.0f));
    h.move_to(over);
    // A moment passing over it opens nothing.
    h.step(10);
    CHECK_FALSE(child_drawn);
    // Resting there past 0.6 s does.
    h.step(40);
    CHECK(child_drawn);
    h.button_up();
    h.step(3);
    CHECK(child_drawn);
}

TEST_CASE("A folded row the mouse rests on without a drag stays folded", "[ui][kit]") {
    KitHarness h;
    bool child_drawn = false;
    h.set_ui([&] {
        if (auto tree = ui::Tree("##tree")) {
            const auto yard = ui::tree_row({.id = "1", .name = "Yard", .has_children = true, .starts_open = false});
            h.mark("yard");
            child_drawn = yard.open;
        }
    });
    h.step(2);
    h.move_to(h.rect("yard").centre());
    h.step(60);
    CHECK_FALSE(child_drawn);
}
