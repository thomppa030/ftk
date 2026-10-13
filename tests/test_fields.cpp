#include "kit_harness.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// Every frame's Edit added up over a run of frames.
struct Tally {
    int changed{0};
    int committed{0};

    void add(const ui::Edit& edit) {
        if (edit.changed) ++changed;
        if (edit.committed) ++committed;
    }
};

} // namespace

TEST_CASE("A dragged number changes live and commits once, on release", "[ui][kit]") {
    KitHarness h;
    float mass = 10.0f;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(200.0f);
        tally.add(ui::drag("##mass", mass, {.speed = 0.1f, .unit = ui::Unit::Kilograms}));
        h.mark("mass");
    });
    h.step(2);
    const glm::vec2 from = h.rect("mass").centre();
    h.drag(from, {from.x + 80.0f, from.y});
    CHECK(mass > 10.0f);
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("A dragged number stays inside its range", "[ui][kit]") {
    KitHarness h;
    float strength = 0.9f;
    h.set_ui([&] {
        ui::next_width(200.0f);
        (void)ui::drag("##strength", strength, {.speed = 0.1f, .min = 0.0f, .max = 1.0f});
        h.mark("strength");
    });
    h.step(2);
    const glm::vec2 from = h.rect("strength").centre();
    h.drag(from, {from.x + 300.0f, from.y});
    CHECK(strength == 1.0f);
}

TEST_CASE("Dragging one axis of a vector moves only that axis, as one commit", "[ui][kit]") {
    KitHarness h;
    glm::vec3 position{1.0f, 2.0f, 3.0f};
    Tally tally;
    h.set_ui([&] {
        ui::next_width(300.0f);
        tally.add(ui::vec3("##position", position));
        h.mark("vector");
    });
    h.step(2);
    // The middle third of the group is the Y field.
    const glm::vec2 at = h.rect("vector").centre();
    h.drag(at, {at.x + 60.0f, at.y});
    CHECK(position.x == 1.0f);
    CHECK(position.y > 2.0f);
    CHECK(position.z == 3.0f);
    CHECK(tally.committed == 1);
}

TEST_CASE("next_width sets the next field's width", "[ui][kit]") {
    KitHarness h;
    float value = 0.0f;
    float field_width = 0.0f;
    h.set_ui([&] {
        ui::next_width(120.0f);
        (void)ui::slider("##value", value, 0.0f, 1.0f);
        field_width = ui::item_max().x - ui::item_min().x;
    });
    h.step(2);
    CHECK(field_width == 120.0f);
}

TEST_CASE("A vector fills the width it is given", "[ui][kit]") {
    KitHarness h;
    glm::vec2 tiling{1.0f, 1.0f};
    h.set_ui([&] {
        ui::next_width(240.0f);
        (void)ui::vec2("##tiling", tiling);
        h.mark("vector");
    });
    h.step(2);
    CHECK(h.rect("vector").width() == 240.0f);
}

TEST_CASE("A slider commits once and keeps to its ends", "[ui][kit]") {
    KitHarness h;
    float slope = 45.0f;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(200.0f);
        tally.add(ui::slider("##slope", slope, 0.0f, 90.0f, ui::Unit::Degrees));
        h.mark("slope");
    });
    h.step(2);
    const glm::vec2 from = h.rect("slope").centre();
    h.drag(from, {from.x + 40.0f, from.y});
    CHECK(slope > 45.0f);
    CHECK(slope < 90.0f);
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
    h.drag(from, {from.x + 400.0f, from.y});
    CHECK(slope == 90.0f);
    CHECK(tally.committed == 2);
}

TEST_CASE("A whole number drags in whole steps and commits once", "[ui][kit]") {
    KitHarness h;
    int count = 4;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(200.0f);
        tally.add(ui::drag_int("##count", count, 0.5f, 0, 100));
        h.mark("count");
    });
    h.step(2);
    const glm::vec2 from = h.rect("count").centre();
    h.drag(from, {from.x + 40.0f, from.y});
    CHECK(count > 4);
    CHECK(tally.committed == 1);
}

TEST_CASE("A checkbox commits on the click that changes it", "[ui][kit]") {
    KitHarness h;
    bool shadows = false;
    Tally tally;
    h.set_ui([&] {
        tally.add(ui::checkbox("##shadows", shadows));
        h.mark("box");
    });
    h.step(2);
    h.click("box");
    CHECK(shadows);
    CHECK(tally.changed == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("Every unit but none has a symbol", "[ui][kit]") {
    CHECK(std::string(ui::unit_symbol(ui::Unit::None)).empty());
    for (auto unit : {ui::Unit::Centimetres, ui::Unit::Metres, ui::Unit::SquareMetres, ui::Unit::MetresPerSecond,
                      ui::Unit::MetresPerSecondSquared,
                      ui::Unit::Degrees, ui::Unit::Radians, ui::Unit::Milliseconds, ui::Unit::Seconds, ui::Unit::Minutes,
                      ui::Unit::Hertz,
                      ui::Unit::Kilograms, ui::Unit::Pixels, ui::Unit::Texels, ui::Unit::Percent, ui::Unit::ExposureValue,
                      ui::Unit::Times}) {
        CHECK_FALSE(std::string(ui::unit_symbol(unit)).empty());
    }
}

TEST_CASE("A row puts its field in the value column and fills it", "[ui][kit]") {
    KitHarness h;
    float mass = 1.0f;
    float window_width = 0.0f;
    h.set_ui([&] {
        window_width = ui::space_left().x;
        if (auto t = ui::PropertyTable("##rows")) {
            ui::row("Mass", [&] {
                (void)ui::drag("##mass", mass);
                h.mark("field");
            });
            ui::hint("A hint under the row");
        }
    });
    h.step(2);
    const float label = ftk::theme::label_column(window_width);
    const float field_width = h.rect("field").width();
    CHECK(h.rect("field").min.x >= label);
    CHECK(field_width > window_width - label - 40.0f);
}

TEST_CASE("A heading with no row above it in a table keeps the rows under it", "[ui][kit]") {
    KitHarness h;
    float a = 0.0f;
    float b = 0.0f;
    h.set_ui([&] {
        if (auto t = ui::PropertyTable("##rows")) {
            // First in the table.
            if (ui::subheading_foldable("All", true)) {
                h.mark("all");
                ui::row("A", [&] {
                    (void)ui::drag("##a", a);
                    h.mark("a");
                });
            }
            // Folded, then another heading straight under it.
            (void)ui::subheading_foldable("Red", false);
            h.mark("red");
            if (ui::subheading_foldable("Green", true)) {
                h.mark("green");
                ui::row("B", [&] {
                    (void)ui::drag("##b", b);
                    h.mark("b");
                });
            }
        }
    });
    h.step(2);
    CHECK(h.rect("a").min.y >= h.rect("all").max.y);
    CHECK(h.rect("red").min.y >= h.rect("a").max.y);
    CHECK(h.rect("green").min.y >= h.rect("red").max.y);
    CHECK(h.rect("b").min.y >= h.rect("green").max.y);
}

TEST_CASE("A stepper steps one at a time and stops at its end", "[ui][kit]") {
    KitHarness h;
    int size = 19;
    int commits = 0;
    h.set_ui([&] {
        if (ui::stepper("##size", size, 11, 20, ui::Unit::Pixels).committed) ++commits;
        h.mark("plus");  // the last item drawn: the plus button
    });
    h.step(2);
    h.click("plus");
    CHECK(size == 20);
    CHECK(commits == 1);
    h.click("plus");
    CHECK(size == 20);
    CHECK(commits == 1);
}

TEST_CASE("Mixed, a checkbox shows a dash and a click turns every one on", "[ui][kit]") {
    KitHarness h;
    bool on = true;
    bool mixed = true;
    Tally tally;
    h.set_ui([&] {
        if (mixed) ui::next_field_mixed();
        const ui::Edit edit = ui::checkbox("Cutout", on);
        tally.add(edit);
        if (edit.changed) mixed = false;
        h.mark("box");
    });
    h.step(2);
    const auto* box = h.find("Cutout", ftk::ui::core::Role::Toggle);
    REQUIRE(box != nullptr);
    CHECK(box->value == ui::MIXED);
    CHECK_FALSE(box->state.checked);

    // On already, and still on: a mixed click does not toggle.
    h.click("box");
    CHECK(on);
    CHECK(tally.committed == 1);
    h.step();
    CHECK(h.find("Cutout", ftk::ui::core::Role::Toggle)->state.checked);
}

TEST_CASE("Mixed, a number reads the word until it is dragged", "[ui][kit]") {
    KitHarness h;
    float cutoff = 0.5f;
    bool mixed = true;
    h.set_ui([&] {
        ui::next_width(200.0f);
        if (mixed) ui::next_field_mixed();
        if (ui::drag("Cutoff", cutoff, {.speed = 0.01f, .min = 0.0f, .max = 1.0f}).committed) mixed = false;
        h.mark("cutoff");
    });
    h.step(2);
    CHECK(h.find("Cutoff", ftk::ui::core::Role::Slider)->value == ui::MIXED);
    const glm::vec2 from = h.rect("cutoff").centre();
    h.drag(from, {from.x + 20.0f, from.y});
    h.step();
    CHECK(cutoff > 0.5f);
    CHECK(h.find("Cutoff", ftk::ui::core::Role::Slider)->value != ui::MIXED);
}
