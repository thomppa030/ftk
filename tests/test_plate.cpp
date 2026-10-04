#include "kit_harness.hpp"

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/plate.hpp"
#include "ftk/ui/kit/text_field.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("A plate takes the pointer over what it floats on, and its items over it", "[ui][kit][plate]") {
    KitHarness h;
    int under = 0;
    int clicked = 0;
    const glm::vec2 view_min{20.0f, 20.0f};
    const glm::vec2 view_max{620.0f, 420.0f};
    h.set_ui([&] {
        ui::core::Context& context = ui::core::current();
        const auto in = context.interact(context.id("##view"), {.min = view_min, .max = view_max},
                                         {.beneath = true, .overlap = true});
        under += in.pressed ? 1 : 0;
        if (auto plate = ui::Plate("##plate", view_min, view_max, ui::PlatePlace::TopRight, 200.0f)) {
            if (ui::button("Go")) ++clicked;
            h.mark("go");
        }
    });
    h.step(2);

    // Its item, then its padding beside the item, then the view clear of it.
    h.click("go");
    CHECK(clicked == 1);
    const ui::core::Rect go = h.rect("go");
    h.click_at({go.max.x + 40.0f, go.centre().y});
    CHECK(under == 0);
    h.click_at({view_min.x + 40.0f, view_max.y - 40.0f});
    CHECK(under == 1);
    // At the top right, in from the view's edges.
    CHECK(go.min.x > view_max.x - ui::PLATE_INSET - 200.0f);
    CHECK(go.max.x < view_max.x - ui::PLATE_INSET);
    CHECK(go.min.y > view_min.y + ui::PLATE_INSET);
}

TEST_CASE("A press on a plate's button leaves the keyboard where it was", "[ui][kit][plate]") {
    KitHarness h;
    std::string text;
    int clicked = 0;
    h.set_ui([&] {
        (void)ui::text_entry("##text", text);
        h.mark("text");
        if (auto plate = ui::Plate("##plate", {0.0f, 100.0f}, {600.0f, 400.0f}, ui::PlatePlace::TopCenter, 200.0f)) {
            if (ui::button("Go")) ++clicked;
            h.mark("go");
        }
    });
    h.step(2);
    h.click("text");
    h.type("ab");
    h.click("go");
    h.type("c");
    CHECK(clicked == 1);
    CHECK(text == "abc");
}
