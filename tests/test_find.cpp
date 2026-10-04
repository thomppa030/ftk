#include "kit_harness.hpp"

#include "ftk/ui/kit/find.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A find field with what it did added up across frames.
struct FindSample {
    std::string query;
    ui::FindToggles toggles;
    int changes{0};
    int next{0};
    int previous{0};
    int closed{0};

    void draw(KitHarness& h, ui::FindFieldOptions options = {}) {
        ui::next_width(300.0f);
        const ui::FindField find = ui::find_field("##find", query, toggles, options);
        h.mark("field");
        changes += find.changed ? 1 : 0;
        next += find.next ? 1 : 0;
        previous += find.previous ? 1 : 0;
        closed += find.closed ? 1 : 0;
    }
};

} // namespace

TEST_CASE("A find field's query is live, and Enter goes on keeping the keyboard", "[ui][kit][find]") {
    KitHarness h;
    FindSample find;
    h.set_ui([&] { find.draw(h); });
    h.step(2);
    h.click("field");
    h.type("ban");
    CHECK(find.query == "ban");
    CHECK(find.changes > 0);

    h.press(ui::Key::Enter);
    CHECK(find.next == 1);
    h.press(ui::Key::Enter, {.shift = true});
    CHECK(find.previous == 1);
    // Still the field's keyboard.
    h.type("d");
    CHECK(find.query == "band");
}

TEST_CASE("Escape closes a find field and keeps its query", "[ui][kit][find]") {
    KitHarness h;
    FindSample find;
    h.set_ui([&] { find.draw(h); });
    h.step(2);
    h.click("field");
    h.type("band");
    h.press(ui::Key::Escape);
    CHECK(find.closed == 1);
    CHECK(find.query == "band");
    h.type("x");
    CHECK(find.query == "band");
}

TEST_CASE("A find field can be given the keyboard, its query selected", "[ui][kit][find]") {
    KitHarness h;
    FindSample find;
    find.query = "old";
    bool focus = true;
    h.set_ui([&] {
        find.draw(h, {.focus = focus});
        focus = false;
    });
    h.step(2);
    h.type("new");
    CHECK(find.query == "new");
}

TEST_CASE("The toggles inside a find field flip at a click", "[ui][kit][find]") {
    KitHarness h;
    FindSample find;
    h.set_ui([&] { find.draw(h); });
    h.step(2);
    for (const char* name : {"Match case", "Whole word", "Regular expression"}) {
        const ui::core::SemanticNode* toggle = h.find(name, ui::core::Role::Toggle);
        REQUIRE(toggle != nullptr);
        CHECK(h.rect("field").contains(toggle->rect.centre()));
        h.click_at(toggle->rect.centre());
    }
    CHECK(find.toggles == ui::FindToggles{.match_case = true, .whole_word = true, .regex = true});
    CHECK(find.changes == 3);
    h.step();
    CHECK(h.find("Regular expression", ui::core::Role::Toggle)->state.checked);
}

TEST_CASE("A replace field replaces on Enter, keeping the keyboard, and closes on Escape", "[ui][kit][find]") {
    KitHarness h;
    std::string replacement;
    int replaced = 0;
    int closed = 0;
    h.set_ui([&] {
        ui::next_width(300.0f);
        const ui::ReplaceField field = ui::replace_field("##replace", replacement);
        h.mark("field");
        replaced += field.replace ? 1 : 0;
        closed += field.closed ? 1 : 0;
    });
    h.step(2);
    h.click("field");
    h.type("strip");
    h.press(ui::Key::Enter);
    h.press(ui::Key::Enter);
    CHECK(replaced == 2);
    h.press(ui::Key::Escape);
    CHECK(closed == 1);
    CHECK(replacement == "strip");
}

TEST_CASE("A click on a find field's toggle leaves the keyboard in the field", "[ui][kit][find]") {
    KitHarness h;
    FindSample find;
    h.set_ui([&] { find.draw(h); });
    h.step(2);
    h.click("field");
    h.type("ba");
    h.click_at(h.find("Match case", ftk::ui::core::Role::Toggle)->rect.centre());
    h.type("nd");
    CHECK(find.query == "band");
    CHECK(find.toggles.match_case);
}
