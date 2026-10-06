#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/problem_row.hpp"
#include "ftk/ui/kit/status_bar.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace core = ftk::ui::core;

namespace {

// Whether the last frame drew anything in `colour` inside `rect`.
bool drew_in(const KitHarness& h, core::Rect rect, ui::Color colour) {
    const uint32_t packed = core::pack_colour(colour);
    for (const auto& root : h.output().roots) {
        for (const auto& p : root.draw->primitives) {
            if (p.colour == packed && rect.contains((p.min + p.max) * 0.5f)) return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("A problem row reports clicks and double-clicks, and opens its menu", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    int opens = 0;
    bool menu = false;
    h.set_ui([&] {
        const auto row = ui::problem_row({.id = "##sea", .severity = ui::Severity::Error,
                                          .message = "'foam_bias' : undeclared identifier", .where = "sea.fjsl:41"});
        h.mark("row");
        if (row.clicked) ++clicks;
        if (row.double_clicked) ++opens;
        if (auto context = ui::ContextMenu("##problem")) menu = true;
    });
    h.step(2);
    h.click("row");
    CHECK(clicks == 1);
    CHECK(opens == 0);
    h.double_click("row");
    CHECK(opens == 1);
    h.click("row", ui::MouseButton::Right);
    h.step();
    CHECK(menu);
}

TEST_CASE("A problem row takes a second line for what happens meanwhile", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        (void)ui::problem_row({.id = "##one", .message = "'albedo' : wrong operand types", .where = "sea.fjsl:58"});
        h.mark("one");
        (void)ui::problem_row({.id = "##two", .message = "'foam_bias' : undeclared identifier",
                               .meanwhile = "It draws with the last version that compiled", .where = "sea.fjsl:41"});
        h.mark("two");
    });
    h.step(2);
    const float one = h.rect("one").max.y - h.rect("one").min.y;
    const float two = h.rect("two").max.y - h.rect("two").min.y;
    CHECK(one == ftk::theme::TREE_ROW);
    CHECK(two > one + ftk::theme::SMALL_TEXT * 0.5f);
    // The rows sit one under the other, the second line inside its own row.
    CHECK(h.rect("two").min.y >= h.rect("one").max.y);
    CHECK(drew_in(h, h.rect("two"), ftk::theme::text_secondary()));
    CHECK_FALSE(drew_in(h, h.rect("one"), ftk::theme::text_secondary()));
}

TEST_CASE("A problem row's icon is in its severity's colour, and where it is on the right", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        (void)ui::problem_row({.id = "##error", .severity = ui::Severity::Error, .message = "Font didn't bake",
                               .where = "Title.fjfont"});
        h.mark("error");
        (void)ui::problem_row({.id = "##warning", .severity = ui::Severity::Warning,
                               .message = "Unknown property 'text-shadow'", .where = "hud.fjss:12:5"});
        h.mark("warning");
    });
    h.step(2);
    const core::Rect error = h.rect("error");
    const core::Rect icon_column{error.min, {error.min.x + 30.0f, error.max.y}};
    CHECK(drew_in(h, icon_column, ftk::theme::error()));
    CHECK_FALSE(drew_in(h, error, ftk::theme::warning()));
    const core::Rect warning = h.rect("warning");
    CHECK(drew_in(h, {warning.min, {warning.min.x + 30.0f, warning.max.y}}, ftk::theme::warning()));
    // Where: in the disabled colour, in the row's right half.
    const core::Rect right_half{{(error.min.x + error.max.x) * 0.5f, error.min.y}, error.max};
    CHECK(drew_in(h, right_half, ftk::theme::text_disabled()));
}

TEST_CASE("A message too long for its row is cut and whole in the tooltip", "[ui][kit]") {
    KitHarness h({300.0f, 200.0f});
    h.set_ui([&] {
        (void)ui::problem_row({.id = "##long", .message = "A sentence far too long to fit in a row this narrow, "
                                                          "which goes on and on past the edge",
                               .where = "sea.fjsl:41"});
        h.mark("long");
        (void)ui::problem_row({.id = "##short", .message = "Short", .where = "sea.fjsl:42"});
        h.mark("short");
    });
    h.step(2);
    h.hover("short");
    h.step(30);
    CHECK_FALSE(h.tooltip().has_value());
    h.hover("long");
    h.step(30);
    CHECK(h.tooltip().has_value());
}

TEST_CASE("A severity toggle shows its state and reports a click", "[ui][kit]") {
    KitHarness h;
    bool errors = true;
    bool warnings = false;
    h.set_ui([&] {
        if (ui::severity_toggle(ui::Severity::Error, 3, errors)) errors = !errors;
        h.mark("errors");
        if (ui::severity_toggle(ui::Severity::Warning, 2, warnings)) warnings = !warnings;
        h.mark("warnings");
    });
    h.step(2);
    REQUIRE(h.find("Errors", core::Role::Toggle));
    CHECK(h.find("Errors", core::Role::Toggle)->state.checked);
    CHECK_FALSE(h.find("Warnings", core::Role::Toggle)->state.checked);
    CHECK(drew_in(h, h.rect("errors"), ftk::theme::error()));
    CHECK(drew_in(h, h.rect("warnings"), ftk::theme::warning()));
    // On, it is filled in the toggle colour.
    CHECK(drew_in(h, h.rect("errors"), ftk::theme::toggle_on()));
    CHECK_FALSE(drew_in(h, h.rect("warnings"), ftk::theme::toggle_on()));

    h.click("errors");
    CHECK_FALSE(errors);
    h.click("warnings");
    CHECK(warnings);
}

TEST_CASE("The status bar's problems show only while there are some, and open the list", "[ui][kit]") {
    KitHarness h;
    int errors = 0;
    int warnings = 0;
    int opened = 0;
    h.set_ui([&] {
        if (auto bar = ui::StatusBar()) {
            bar.right();
            if (ui::status_problems(errors, warnings, "'foam_bias' : undeclared identifier", "sea.fjsl:41")) ++opened;
            ui::status_item(nullptr, "6.94 ms");
        }
    });
    h.step(3);
    CHECK_FALSE(h.find("Problems", core::Role::Button));

    errors = 3;
    warnings = 2;
    h.step(3);
    REQUIRE(h.find("Problems", core::Role::Button));
    const core::Rect rect = h.rect("Problems");
    CHECK(drew_in(h, rect, ftk::theme::error()));
    CHECK(drew_in(h, rect, ftk::theme::warning()));
    // Where the newest is, small in the disabled colour after its sentence.
    CHECK(drew_in(h, rect, ftk::theme::text_disabled()));
    h.click("Problems");
    CHECK(opened == 1);
}
