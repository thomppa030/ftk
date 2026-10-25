#include "kit_harness.hpp"

#include "ftk/ui/core/context.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/plate.hpp"
#include "ftk/ui/kit/region.hpp"
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

TEST_CASE("A card holding more than its view has room for stays inside it, what it holds scrolling under the wheel",
          "[ui][kit][plate]") {
    KitHarness h;
    const glm::vec2 view_max{600.0f, 300.0f};
    bool shown = true;
    glm::vec2 card_min{0.0f};
    glm::vec2 card_max{0.0f};
    int clicked = -1;
    h.set_ui([&] {
        // What it hangs over scrolls too: the wheel over the card is the card's.
        if (auto under = ui::Region("##under", {600.0f, 300.0f})) {
            for (int i = 0; i < 40; ++i) {
                (void)ui::button(("Under " + std::to_string(i)).c_str());
                if (i == 0) h.mark("under");
            }
        }
        if (!shown) return;
        if (auto card = ui::Card("##card", {.view_min = {0.0f, 0.0f}, .view_max = view_max,
                                            .anchor_min = {100.0f, 20.0f}, .anchor_max = {140.0f, 36.0f},
                                            .width = 200.0f, .padding = {12.0f, 10.0f}})) {
            const glm::vec2 top = ui::cursor();
            for (int i = 0; i < 30; ++i) {
                if (ui::button(("Row " + std::to_string(i)).c_str())) clicked = i;
                h.mark("row " + std::to_string(i));
            }
            card_min = card.min();
            card_max = card.max(ui::cursor().y - top.y);
        }
    });
    h.step(3);

    // No taller than the room under its anchor, in from the view's foot.
    CHECK(card_min.y > 36.0f);
    CHECK(card_max.y <= view_max.y - ui::PLATE_INSET + 0.5f);
    const ui::core::Rect first = h.rect("row 0");
    const ui::core::Rect last = h.rect("row 29");
    CHECK(last.min.y > card_max.y);
    // A row past its foot is out of sight, and takes no click.
    h.click_at({h.rect("row 12").centre().x, h.rect("row 12").max.y - 2.0f});
    if (h.rect("row 12").min.y > card_max.y) CHECK(clicked == -1);

    // The wheel over it scrolls what it holds, and nothing under it.
    const float under_before = h.rect("under").min.y;
    h.wheel({card_min.x + 60.0f, card_min.y + 60.0f}, -2.0f);
    h.step(30);
    CHECK(h.rect("row 0").min.y < first.min.y - 40.0f);
    CHECK(h.rect("under").min.y == under_before);

    // As far as it goes: its last row shows, and takes a click.
    h.wheel({card_min.x + 60.0f, card_min.y + 60.0f}, -100.0f);
    h.step(30);
    const ui::core::Rect bottom = h.rect("row 29");
    CHECK(bottom.max.y <= card_max.y);
    CHECK(bottom.max.y > card_max.y - 40.0f);
    h.click("row 29");
    CHECK(clicked == 29);

    // Its scrollbar down its right edge: a press near its top takes what it
    // holds back up.
    h.click_at({card_max.x - 4.0f, card_min.y + 10.0f});
    h.step(10);
    CHECK(h.rect("row 0").min.y > first.min.y - 30.0f);

    // Shut and opened again, it starts at its top.
    h.wheel({card_min.x + 60.0f, card_min.y + 60.0f}, -100.0f);
    h.step(30);
    shown = false;
    h.step(2);
    shown = true;
    h.step(3);
    CHECK(h.rect("row 0").min.y == first.min.y);
}

TEST_CASE("A card that fits takes the pointer as tall as what it holds, and has no scrollbar", "[ui][kit][plate]") {
    KitHarness h;
    int under = 0;
    glm::vec2 card_max{0.0f};
    h.set_ui([&] {
        ui::core::Context& context = ui::core::current();
        const auto in = context.interact(context.id("##view"), {.min = {0.0f, 0.0f}, .max = {600.0f, 400.0f}},
                                         {.beneath = true, .overlap = true});
        under += in.pressed ? 1 : 0;
        if (auto card = ui::Card("##card", {.view_min = {0.0f, 0.0f}, .view_max = {600.0f, 400.0f},
                                            .anchor_min = {100.0f, 20.0f}, .anchor_max = {140.0f, 36.0f},
                                            .width = 200.0f, .padding = {12.0f, 10.0f}})) {
            (void)ui::button("Go");
            h.mark("go");
            card_max = card.max(1000.0f);
        }
    });
    h.step(3);
    const ui::core::Rect go = h.rect("go");
    // The most it may show reaches the view's foot, but it takes the pointer
    // only down to its padding under the button.
    CHECK(card_max.y == 400.0f - ui::PLATE_INSET);
    h.click_at({go.centre().x, go.max.y + 10.0f - 2.0f});
    CHECK(under == 0);
    h.click_at({go.centre().x, go.max.y + 10.0f + 4.0f});
    CHECK(under == 1);
    // At its right edge, where a scrollbar would be, nothing scrolls.
    h.click_at({card_max.x - 4.0f, go.centre().y});
    CHECK(h.rect("go").min.y == go.min.y);
}
