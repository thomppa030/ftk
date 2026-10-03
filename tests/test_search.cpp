#include "kit_harness.hpp"

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/search.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A search matches anywhere in the text, ignoring case", "[ui][kit]") {
    CHECK(ui::matches("Crate_01", "cra"));
    CHECK(ui::matches("Crate_01", "TE_0"));
    CHECK(ui::matches("Crate_01", ""));
    CHECK_FALSE(ui::matches("Crate_01", "box"));
    CHECK(ui::detail::find_match("Old crate", "CRATE") == 4);
}

TEST_CASE("The search field filters as you type, and its clear button empties it", "[ui][kit]") {
    KitHarness h;
    std::string query;
    int changes = 0;
    h.set_ui([&] {
        ui::next_width(200.0f);
        if (ui::search_field("##search", query)) ++changes;
        h.mark("field");
    });
    h.step(2);
    h.click("field");
    h.type("cra");
    // While typing, with no Enter.
    CHECK(query == "cra");
    CHECK(changes > 0);

    // The clear button sits over the right end of the field.
    const auto field = h.rect("field");
    const glm::vec2 clear{field.max.x - field.height() * 0.5f, field.centre().y};
    h.drag(clear, clear);
    CHECK(query.empty());
}

TEST_CASE("Esc empties the search field, not only the typing since it was entered", "[ui][kit]") {
    KitHarness h;
    std::string query = "lamp";
    h.set_ui([&] {
        ui::next_width(200.0f);
        (void)ui::search_field("##search", query);
        h.mark("field");
    });
    h.step(2);
    h.click("field");
    h.type("s");
    REQUIRE(query == "lamps");
    h.press(ui::Key::Escape);
    CHECK(query.empty());
}

TEST_CASE("Ctrl F puts the cursor in the focused window's search field", "[ui][kit]") {
    KitHarness h;
    std::string query;
    h.set_ui([&] {
        ui::next_width(200.0f);
        (void)ui::search_field("##search", query);
    });
    h.step(2);
    h.press(ui::Key::F, {.ctrl = true});
    h.step(1);
    h.type("x");
    CHECK(query == "x");
}
