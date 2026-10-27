#include "kit_harness.hpp"

#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/search.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A search matches anywhere in the text, ignoring case", "[ui][kit]") {
    CHECK(ui::matches("Crate_01", "cra"));
    CHECK(ui::matches("Crate_01", "TE_0"));
    CHECK(ui::matches("Crate_01", ""));
    CHECK_FALSE(ui::matches("Crate_01", "box"));
    CHECK(ui::detail::find_match("Old crate", "CRATE") == 4);
}

namespace {

// The runs a fuzzy match found, as the text they cover.
std::vector<std::string> found_runs(std::string_view text, std::string_view query) {
    std::vector<std::string> runs;
    if (const auto match = ui::fuzzy_match(text, query)) {
        for (const auto& run : match->runs) runs.emplace_back(text.substr(run.begin, run.end - run.begin));
    }
    return runs;
}

int score(std::string_view text, std::string_view query) {
    return ui::fuzzy_match(text, query).value().score;
}

} // namespace

TEST_CASE("A fuzzy search finds the query's characters in order, any distance apart, ignoring case", "[ui][kit]") {
    CHECK(found_runs("Vector warp", "wp") == std::vector<std::string>{"w", "p"});
    CHECK(found_runs("Gradient map", "GRAD") == std::vector<std::string>{"Grad"});
    CHECK(found_runs("Bevel", "bl") == std::vector<std::string>{"B", "l"});
    CHECK_FALSE(ui::fuzzy_match("Warp", "pw").has_value());
    CHECK_FALSE(ui::fuzzy_match("Blur", "blurry").has_value());
    const auto everything = ui::fuzzy_match("Noise", "");
    REQUIRE(everything.has_value());
    CHECK(everything->score == 0);
    CHECK(everything->runs.empty());
}

TEST_CASE("A fuzzy search ranks starts of words and characters together first", "[ui][kit]") {
    // The text's start, then a word's, beats the middle of one.
    CHECK(score("Warp", "wp") > score("Vector warp", "wp"));
    CHECK(score("Vector warp", "wp") > score("Bowl of soup", "wp"));
    // Together beats apart.
    CHECK(score("Blur", "bl") > score("Bevel", "bl"));
    // Found at the starts of words beats found in a row in the middle.
    CHECK(score("Tile sampler", "ts") > score("Toasts", "ts"));
    // A capital after a small letter starts a word too.
    CHECK(found_runs("histogramScan", "hs") == std::vector<std::string>{"h", "S"});
}

TEST_CASE("A fuzzy search takes whole characters of UTF-8 text", "[ui][kit]") {
    CHECK(found_runs("Größe", "öß") == std::vector<std::string>{"öß"});
    CHECK(found_runs("Große Öse", "gö") == std::vector<std::string>{"G", "Ö"});
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

TEST_CASE("A search button drawn as the field opens its search when clicked", "[ui][kit]") {
    KitHarness h;
    int opened = 0;
    h.set_ui([&] {
        ui::next_width(240.0f);
        if (ui::search_button("##find_in_files", "Find in files", "Ctrl Shift F")) ++opened;
    });
    h.step(2);
    const auto* button = h.find("Find in files", ui::core::Role::Button);
    REQUIRE(button != nullptr);
    CHECK(button->rect.width() == 240.0f);
    h.click_at(button->rect.centre());
    CHECK(opened == 1);
}

TEST_CASE("Ctrl Shift F leaves the search field alone, for a shortcut of its own", "[ui][kit]") {
    KitHarness h;
    std::string query;
    h.set_ui([&] {
        ui::next_width(200.0f);
        (void)ui::search_field("##search", query);
    });
    h.step(2);
    h.press(ui::Key::F, {.ctrl = true, .shift = true});
    h.step(1);
    h.type("x");
    CHECK(query.empty());
}
