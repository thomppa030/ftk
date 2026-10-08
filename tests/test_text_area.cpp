#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text_field.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A description field `width` wide over `text`, with a button beside it to
// click away to. Every commit is applied to `text` and counted.
struct Description {
    KitHarness h;
    std::string text;
    int commits{0};

    explicit Description(std::string start = {}, float width = 300.0f) : text(std::move(start)) {
        h.set_ui([this, width] {
            ui::next_width(width);
            std::string shown = text;
            if (ui::text_area("##Description", shown).committed) {
                text = shown;
                ++commits;
            }
            h.mark("area");
            ui::button("Elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    // Into the area, the caret at the end of its text.
    void enter_at_end() {
        h.click("area");
        h.press(ui::Key::End, {.ctrl = true});
    }

    void finish() { h.press(ui::Key::Enter, {.ctrl = true}); }
};

} // namespace

TEST_CASE("A text area starts a new line on Enter and commits once, on leaving", "[ui][text_area]") {
    Description d;
    d.h.click("area");
    d.h.type("First");
    d.h.press(ui::Key::Enter);
    d.h.type("Second");
    CHECK(d.commits == 0);
    CHECK(d.text.empty());

    d.h.click("elsewhere");
    CHECK(d.commits == 1);
    CHECK(d.text == "First\nSecond");
}

TEST_CASE("Ctrl Enter finishes a text area, and Escape puts its text back", "[ui][text_area]") {
    Description d("Old");
    d.enter_at_end();
    d.h.type(" and new");
    d.finish();
    CHECK(d.commits == 1);
    CHECK(d.text == "Old and new");

    d.enter_at_end();
    d.h.type(" and more");
    d.h.press(ui::Key::Escape);
    CHECK(d.commits == 1);
    CHECK(d.text == "Old and new");
}

TEST_CASE("Up and Down move a text area's caret from line to line", "[ui][text_area]") {
    Description d("abc\ndef");
    d.enter_at_end();
    // Up from the end of "def" lands after "abc"; Down from after "abcX",
    // further along than "def" goes, lands at its end.
    d.h.press(ui::Key::Up);
    d.h.type("X");
    d.h.press(ui::Key::Down);
    d.h.type("Y");
    // Up from the first line goes to the start, Down from the last to the end.
    d.h.press(ui::Key::Up);
    d.h.press(ui::Key::Up);
    d.h.type("<");
    d.h.press(ui::Key::Down);
    d.h.press(ui::Key::Down);
    d.h.type(">");
    d.finish();
    CHECK(d.text == "<abcX\ndefY>");
}

TEST_CASE("Home and End go to a line's ends, and with Ctrl to the text's", "[ui][text_area]") {
    Description d("one\ntwo\nthree");
    d.enter_at_end();
    d.h.press(ui::Key::Up);
    d.h.press(ui::Key::Home);
    d.h.type("[");
    d.h.press(ui::Key::End);
    d.h.type("]");
    d.h.press(ui::Key::Home, {.ctrl = true});
    d.h.type("^");
    d.finish();
    CHECK(d.text == "^one\n[two]\nthree");
}

TEST_CASE("A text area wraps its text, and Down follows the wrapped lines", "[ui][text_area]") {
    const std::string sentence = "the quick brown fox jumps over the lazy dog and keeps on running";
    Description d(sentence, 120.0f);
    d.h.click("area");
    d.h.press(ui::Key::Home, {.ctrl = true});
    d.h.press(ui::Key::Down);
    d.h.type("|");
    d.finish();
    // Not at the end, as Down on one long line would go, but where the
    // second wrapped line begins.
    const std::size_t bar = d.text.find('|');
    REQUIRE(bar != std::string::npos);
    CHECK(bar > 0);
    CHECK(bar < sentence.size() - 1);
    CHECK(d.text[bar - 1] == ' ');
}

TEST_CASE("A click puts a text area's caret on the line it lands on", "[ui][text_area]") {
    Description d("first line\nsecond line");
    const auto rect = d.h.rect("area");
    // At the left edge, halfway down the three lines: the second line's start.
    d.h.click_at({rect.min.x + 2.0f, (rect.min.y + rect.max.y) * 0.5f});
    d.h.type("X");
    d.finish();
    CHECK(d.text == "first line\nXsecond line");
}

TEST_CASE("Pasting into a text area keeps the lines", "[ui][text_area]") {
    Description d("one\ntwo");
    d.h.click("area");
    d.h.press(ui::Key::A, {.ctrl = true});
    d.h.press(ui::Key::C, {.ctrl = true});
    d.h.press(ui::Key::End, {.ctrl = true});
    d.h.press(ui::Key::V, {.ctrl = true});
    d.finish();
    CHECK(d.text == "one\ntwoone\ntwo");
}
