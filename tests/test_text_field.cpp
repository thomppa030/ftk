#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text_field.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

using ftk::test::KitHarness;
using ftk::ui::TextField;
namespace ui = ftk::ui;

namespace {

// A name field over `name` with a button beside it to click away to, the
// way the Properties panel draws a node's name. Every commit is applied to
// `name` and counted.
struct NameField {
    KitHarness h;
    TextField field;
    std::string name{"Crate"};
    int commits{0};
    int changed_frames{0};

    NameField() {
        h.set_ui([this] {
            auto edit = field.draw("##Name", name);
            h.mark("field");
            if (edit.changed) ++changed_frames;
            if (edit.committed) {
                name = field.text();
                ++commits;
            }
            ui::button("Elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    // Clears the field's text: select all, then delete.
    void clear() {
        h.press(ui::Key::A, {.ctrl = true});
        h.step();
        h.press(ui::Key::Delete);
    }
};

} // namespace

TEST_CASE("TextField shows the value it is drawn with", "[ui][text_field]") {
    NameField f;
    CHECK(f.field.text() == "Crate");
    f.name = "Barrel";
    f.h.step();
    CHECK(f.field.text() == "Barrel");
}

TEST_CASE("TextField commits once, on Enter, not per keystroke", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Crate2");
    CHECK(f.changed_frames > 0);
    CHECK(f.commits == 0);
    CHECK(f.name == "Crate");

    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 1);
    CHECK(f.name == "Crate2");
}

TEST_CASE("TextField commits when it loses focus", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Lamp");
    f.h.click("elsewhere");
    CHECK(f.commits == 1);
    CHECK(f.name == "Lamp");
}

TEST_CASE("TextField restores the old text on Escape and commits nothing", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Oops");
    f.h.press(ui::Key::Escape);
    CHECK(f.commits == 0);
    CHECK(f.name == "Crate");
    f.h.step();
    CHECK(f.field.text() == "Crate");
}

TEST_CASE("TextField commits nothing when the text ends where it started", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 0);
}

TEST_CASE("TextField keeps typed text when the value changes underneath it", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Crate2");
    f.name = "Renamed elsewhere";
    f.h.step();
    CHECK(f.commits == 0);
    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 1);
    CHECK(f.name == "Crate2");
}

namespace {

// The stateless form over a value the caller owns, the way a component
// inspector draws a field straight onto a component.
struct PlainField {
    KitHarness h;
    std::string value{"idle"};
    int commits{0};

    PlainField() {
        h.set_ui([this] {
            if (ftk::ui::text_field("##Default", value).committed) ++commits;
            h.mark("field");
            ui::button("Elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    void clear() {
        h.press(ui::Key::A, {.ctrl = true});
        h.step();
        h.press(ui::Key::Delete);
    }
};

} // namespace

TEST_CASE("text_field shows its hint while empty and commits what is typed over it", "[ui][text_field]") {
    KitHarness h;
    std::string value;
    int commits = 0;
    h.set_ui([&] {
        if (ftk::ui::text_field("##Output", value, {.hint = "Beside each source"}).committed) ++commits;
        h.mark("field");
        ui::button("Elsewhere");
        h.mark("elsewhere");
    });
    h.step(2);
    const auto drew_hint = [&] {
        const uint32_t hint = ftk::ui::core::pack_colour(ftk::theme::text_disabled());
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.colour == hint && h.rect("field").contains((p.min + p.max) * 0.5f)) return true;
            }
        }
        return false;
    };
    CHECK(drew_hint());
    // The hint is never the value: leaving the field untouched commits nothing.
    h.click("field");
    h.click("elsewhere");
    CHECK(commits == 0);
    CHECK(value.empty());
    h.click("field");
    h.type("models");
    h.press(ui::Key::Enter);
    CHECK(commits == 1);
    CHECK(value == "models");
    h.step();
    CHECK_FALSE(drew_hint());
}

TEST_CASE("text_field leaves the value alone while typing and writes it on Enter", "[ui][text_field]") {
    PlainField f;
    f.h.click("field");
    f.clear();
    f.h.type("walk");
    CHECK(f.value == "idle");
    f.h.press(ui::Key::Enter);
    CHECK(f.commits == 1);
    CHECK(f.value == "walk");
}

TEST_CASE("text_field writes the value when it loses focus", "[ui][text_field]") {
    PlainField f;
    f.h.click("field");
    f.clear();
    f.h.type("run");
    f.h.click("elsewhere");
    CHECK(f.commits == 1);
    CHECK(f.value == "run");
}

TEST_CASE("text_field keeps the value on Escape", "[ui][text_field]") {
    PlainField f;
    f.h.click("field");
    f.clear();
    f.h.type("oops");
    f.h.press(ui::Key::Escape);
    f.h.step();
    CHECK(f.commits == 0);
    CHECK(f.value == "idle");
}

TEST_CASE("A text entry's text is live, and Enter is told apart from leaving it", "[ui][kit]") {
    KitHarness h;
    std::string name;
    std::string seen;
    int entered = 0;
    int left = 0;
    bool focus = false;
    h.set_ui([&] {
        if (ui::text_entry("##name", name, {.hint = "Project name", .focus = std::exchange(focus, false)})) ++entered;
        if (ui::item_deactivated()) ++left;
        h.mark("name");
        seen = name;
        ui::button("Elsewhere");
        h.mark("elsewhere");
    });
    h.step(2);
    // Focus asked for on one frame takes the keyboard on the next.
    focus = true;
    h.step(2);
    h.type("Fjord");
    h.step();
    CHECK(seen == "Fjord");
    CHECK(entered == 0);
    h.press(ui::Key::Enter);
    h.step();
    CHECK(entered == 1);

    h.click("name");
    h.type("s");
    h.step();
    h.click("elsewhere");
    CHECK(name == "Fjords");
    CHECK(entered == 1);
    CHECK(left >= 1);
}

TEST_CASE("A text entry's prompt comes before the typing and isn't part of it", "[ui][kit]") {
    KitHarness h;
    std::string line;
    bool entered = false;
    h.set_ui([&] {
        ui::next_width(200.0f);
        entered = ui::text_entry("##line", line, {.prompt = ":"}) || entered;
        h.mark("line");
    });
    h.step(2);
    h.click("line");
    h.type("18");
    h.press(ui::Key::Enter);
    CHECK(line == "18");
    CHECK(entered);
}

TEST_CASE("stop_typing drops what a field held, and the value set stays", "[ui][text_field]") {
    KitHarness h;
    std::string query;
    bool clear = false;
    h.set_ui([&] {
        (void)ui::text_entry("##search", query);
        h.mark("search");
        if (ui::button("Clear")) clear = true;
        h.mark("clear");
        if (std::exchange(clear, false)) {
            ui::stop_typing();
            query.clear();
        }
    });
    h.step(2);
    h.click("search");
    h.type("kit");
    REQUIRE(query == "kit");
    h.click("clear");
    h.step(3);
    CHECK(query.empty());
    CHECK_FALSE(ui::typing());
    // Typed in again, it starts from what it now holds.
    h.click("search");
    h.type("props");
    CHECK(query == "props");
}

