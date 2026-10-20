#include "kit_harness.hpp"

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/layout.hpp"
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

TEST_CASE("A card hangs under its anchor inside its view, and takes the pointer over what it covers", "[ui][kit][plate]") {
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
        // A word near the view's right edge: the card is pushed back in.
        if (auto card = ui::Card("##card", {.view_min = view_min, .view_max = view_max,
                                            .anchor_min = {580.0f, 100.0f}, .anchor_max = {600.0f, 120.0f},
                                            .width = 200.0f, .padding = {12.0f, 10.0f}})) {
            if (ui::button("Go")) ++clicked;
            h.mark("go");
        }
    });
    h.step(2);

    const ui::core::Rect go = h.rect("go");
    CHECK(go.min.y > 120.0f);
    CHECK(go.min.x >= view_max.x - ui::PLATE_INSET - 200.0f + 12.0f - 0.5f);
    h.click("go");
    CHECK(clicked == 1);
    // Its padding beside the button, then the view clear of it.
    h.click_at({go.max.x + 30.0f, go.centre().y});
    CHECK(under == 0);
    h.click_at({view_min.x + 40.0f, view_max.y - 40.0f});
    CHECK(under == 1);
}

TEST_CASE("A card with no room under its anchor hangs over it", "[ui][kit][plate]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto card = ui::Card("##card", {.view_min = {0.0f, 0.0f}, .view_max = {600.0f, 420.0f},
                                            .anchor_min = {100.0f, 390.0f}, .anchor_max = {140.0f, 410.0f},
                                            .width = 200.0f, .padding = {12.0f, 10.0f}})) {
            (void)ui::button("Go");
            h.mark("go");
        }
    });
    h.step(3);
    CHECK(h.rect("go").max.y < 390.0f);
}

TEST_CASE("A plate at the bottom right sits its foot in from the view's corner", "[ui][kit][plate]") {
    KitHarness h;
    h.set_ui([&] {
        if (auto plate = ui::Plate("##plate", {0.0f, 0.0f}, {600.0f, 400.0f}, ui::PlatePlace::BottomRight, 200.0f)) {
            (void)ui::button("Go");
            h.mark("go");
        }
    });
    h.step(3);
    const ui::core::Rect go = h.rect("go");
    CHECK(go.max.x < 600.0f - ui::PLATE_INSET);
    CHECK(go.min.x > 600.0f - ui::PLATE_INSET - 200.0f);
    CHECK(go.max.y < 400.0f - ui::PLATE_INSET);
    CHECK(go.max.y > 400.0f - ui::PLATE_INSET - 20.0f);
}

TEST_CASE("What a plate or a card holds keeps inside it, the room left ending at its padding", "[ui][kit][plate]") {
    KitHarness h;
    float plate_room = 0.0f;
    float plate_inner = 0.0f;
    float card_room = 0.0f;
    float card_inner = 0.0f;
    h.set_ui([&] {
        if (auto plate = ui::Plate("##plate", {0.0f, 0.0f}, {600.0f, 400.0f}, ui::PlatePlace::TopCenter, 200.0f)) {
            plate_room = ui::space_left().x;
            plate_inner = plate.inner_width();
            ui::separator();
            h.mark("rule");
        }
        if (auto card = ui::Card("##card", {.view_min = {0.0f, 0.0f}, .view_max = {600.0f, 400.0f},
                                            .anchor_min = {100.0f, 200.0f}, .anchor_max = {140.0f, 216.0f},
                                            .width = 180.0f, .padding = {12.0f, 10.0f}})) {
            card_room = ui::space_left().x;
            card_inner = card.inner_width();
        }
    });
    h.step(2);
    CHECK(plate_room == plate_inner);
    CHECK(card_room == card_inner);
    // The rule across the room left ends inside the plate, centred in the
    // view: 200 to 400, its padding in from each side.
    CHECK(h.rect("rule").max.x <= 400.0f - (200.0f - plate_inner) * 0.5f + 0.5f);
}
